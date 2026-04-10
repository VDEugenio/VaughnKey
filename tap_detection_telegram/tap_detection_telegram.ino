/*
 * DoorLock — Touch + BLE + Servo + Telegram with Deep Sleep
 *
 * Hardware:
 *   ESP32, capacitive touch on T0 (GPIO4)
 *   DS-S012 servo on GPIO18
 *   Powerbank button via 2N2222A on GPIO14
 *
 * Flow:
 *   ESP32 sleeps until peephole is touched (wake)
 *   Measures how long the touch is held:
 *   - Quick tap  → TAP_ACTION
 *   - Long hold  → HOLD_ACTION
 *   UNLOCK requires BLE verification — scans for trusted phone nearby.
 *   LOCK always executes immediately (no BLE needed).
 *   After every action, a Telegram notification is sent over WiFi.
 *   If BLE auth fails, sends an interactive Telegram message and waits
 *   up to 2 minutes for the user to reply YES/NO to unlock remotely.
 *   Then goes back to deep sleep.
 *
 * To swap which gesture locks vs unlocks, just flip TAP_ACTION and HOLD_ACTION below.
 */

#include <esp_sleep.h>
#include <ESP32Servo.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include "config.h"  // WiFi + Telegram credentials (gitignored)

// ---- Action Mapping (swap these to change behavior) ----
#define TAP_ACTION   UNLOCK      // quick tap triggers this
#define HOLD_ACTION  LOCK        // long hold triggers this

enum Action { LOCK, UNLOCK };

// ---- Tunable Parameters ----
#define TOUCH_PIN        T0        // GPIO4
#define TOUCH_THRESHOLD  40        // Below this = touched (calibrate!)
#define LONG_HOLD_MS     1000      // Hold this long for HOLD_ACTION
#define HOLD_TIMEOUT_MS  5000      // Safety: max time to wait for release

// ---- BLE Config ----
struct TrustedDevice {
  const char* bleName;      // BLE advertised name to scan for
  const char* friendlyName; // Used in Telegram messages
};
const TrustedDevice trustedDevices[] = TRUSTED_DEVICES;
const int NUM_TRUSTED = sizeof(trustedDevices) / sizeof(trustedDevices[0]);
#define BLE_SCAN_TIME    1         // Scan duration in seconds
#define BLE_RSSI_MIN     -70       // Must be closer than this (less negative = closer)

// ---- Servo Config ----
#define SERVO_PIN   18
const int START_POS  = 90;   // neutral
const int LOCK_POS   = 170;  // +80° from center
const int UNLOCK_POS = 10;   // -80° from center

// ---- Powerbank Button Control ----
#define POWERBANK_BTN_PIN     14
#define POWERBANK_PRESS_MS    200    // How long to "hold" the button
#define POWERBANK_WAKE_MS     1500   // Stabilization delay after release

// ---- WiFi/Telegram Timing ----
#define WIFI_TIMEOUT_MS    10000    // Max time to wait for WiFi connect
#define TELEGRAM_POLL_INTERVAL_MS  2000     // How often to check for reply
#define TELEGRAM_POLL_TIMEOUT_MS   120000   // 2 minutes max wait for reply

Servo servo;
bool bleInitialized = false;
String matchedName = "";  // Friendly name of the BLE device that authenticated

// WiFi/Telegram globals — created on demand to save memory
WiFiClientSecure secured_client;
UniversalTelegramBot* bot = nullptr;
bool wifiConnected = false;

void setup() {
  Serial.begin(115200);
  delay(300);

  // Init powerbank button pin (must be LOW so we don't accidentally hold the button)
  pinMode(POWERBANK_BTN_PIN, OUTPUT);
  digitalWrite(POWERBANK_BTN_PIN, LOW);

  esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();

  if (wakeReason == ESP_SLEEP_WAKEUP_TOUCHPAD) {
    Serial.println("\n--- Woke up from touch! ---");

    bool isLongHold = measureTouch();
    Action action = isLongHold ? HOLD_ACTION : TAP_ACTION;

    if (action == LOCK) {
      // Lock always executes immediately
      servoInit();
      doAction(LOCK);
      servoDetach();

      if (wifiConnect()) {
        telegramSend("Door locked");
        wifiDisconnect();
      }

    } else {
      // Unlock requires BLE verification
      if (bleScanForTrusted()) {
        bleDeinit();  // Free ~170KB BLE RAM before WiFi
        servoInit();
        doAction(UNLOCK);
        servoDetach();

        if (wifiConnect()) {
          telegramSend(("Door unlocked by " + matchedName).c_str());
          wifiDisconnect();
        }

      } else {
        bleDeinit();  // Free ~170KB BLE RAM before WiFi
        Serial.println(">> BLE auth failed — trying Telegram remote unlock...");

        if (wifiConnect()) {
          if (telegramAskToUnlock()) {
            servoInit();
            doAction(UNLOCK);
            servoDetach();
            telegramSend("Door unlocked remotely via Telegram");
          } else {
            telegramSend("Unlock denied or timed out. Staying locked.");
          }

          wifiDisconnect();
        } else {
          Serial.println(">> WiFi failed — cannot do remote unlock fallback.");
        }
      }
    }

    bleDeinit();  // Safety net — no-op if already deinited
    delay(100);

  } else {
    // First boot (power on / reset)
    Serial.println("=== DoorLock + Telegram ===");
    Serial.printf("Quick tap = %s | Long hold = %s\n",
      TAP_ACTION == LOCK ? "LOCK" : "UNLOCK",
      HOLD_ACTION == LOCK ? "LOCK" : "UNLOCK");
    Serial.printf("Trusted BLE devices: %d\n", NUM_TRUSTED);
    for (int i = 0; i < NUM_TRUSTED; i++) {
      Serial.printf("  \"%s\" → %s\n", trustedDevices[i].bleName, trustedDevices[i].friendlyName);
    }
    Serial.printf("WiFi SSID: \"%s\"\n", WIFI_SSID);
    Serial.println("Going to sleep... touch GPIO4 to wake.\n");
  }

  goToSleep();
}

void loop() {
  // Never reached — setup() always ends with deep sleep
}

// ---- Touch Duration Detection ----

bool measureTouch() {
  unsigned long touchStart = millis();

  while (touchRead(TOUCH_PIN) < TOUCH_THRESHOLD) {
    unsigned long held = millis() - touchStart;

    if (held >= LONG_HOLD_MS) {
      Serial.printf("  held %lums — LONG HOLD\n", held);
      return true;
    }

    delay(10);
  }

  unsigned long duration = millis() - touchStart;
  Serial.printf("  held %lums — QUICK TAP\n", duration);
  return false;
}

// ---- BLE Functions ----

bool bleScanForTrusted() {
  Serial.printf("BLE: scanning %ds for %d trusted device(s)...\n", BLE_SCAN_TIME, NUM_TRUSTED);

  BLEDevice::init("DoorLock");
  bleInitialized = true;

  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);

  BLEScanResults* results = scan->start(BLE_SCAN_TIME, false);
  int count = results->getCount();
  Serial.printf("BLE: found %d device(s)\n", count);

  for (int i = 0; i < count; i++) {
    BLEAdvertisedDevice device = results->getDevice(i);
    if (!device.haveName()) continue;

    String name = String(device.getName().c_str());

    for (int t = 0; t < NUM_TRUSTED; t++) {
      if (name == trustedDevices[t].bleName) {
        int rssi = device.getRSSI();
        Serial.printf("BLE: matched \"%s\" (%s) RSSI: %d\n",
          trustedDevices[t].bleName, trustedDevices[t].friendlyName, rssi);

        if (rssi >= BLE_RSSI_MIN) {
          Serial.println("BLE: trusted device is nearby!");
          matchedName = trustedDevices[t].friendlyName;
          scan->clearResults();
          return true;
        } else {
          Serial.println("BLE: trusted device found but too far away.");
          scan->clearResults();
          return false;
        }
      }
    }
  }

  Serial.println("BLE: no trusted device found.");
  scan->clearResults();
  return false;
}

void bleDeinit() {
  if (bleInitialized) {
    BLEDevice::deinit(false);
    bleInitialized = false;
  }
}

// ---- WiFi Functions ----

bool wifiConnect() {
  Serial.printf("WiFi: connecting to \"%s\"...\n", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT_MS) {
      Serial.println("WiFi: connection timed out.");
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      return false;
    }
    delay(500);
    Serial.print(".");
  }

  Serial.printf("\nWiFi: connected! IP: %s\n", WiFi.localIP().toString().c_str());

  secured_client.setCACert(TELEGRAM_CERTIFICATE_ROOT);

  bot = new UniversalTelegramBot(BOT_TOKEN, secured_client);
  wifiConnected = true;
  return true;
}

void wifiDisconnect() {
  if (bot != nullptr) {
    delete bot;
    bot = nullptr;
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiConnected = false;
  Serial.println("WiFi: disconnected.");
}

// ---- Telegram Functions ----

bool telegramSend(const char* message) {
  if (bot == nullptr) return false;

  bool sent = bot->sendMessage(CHAT_ID, message, "");
  if (sent) {
    Serial.printf("Telegram: sent — \"%s\"\n", message);
  } else {
    Serial.println("Telegram: send failed.");
  }
  return sent;
}

// Send an inline keyboard with Yes/No buttons and wait for a callback.
// Returns true if YES button pressed within the timeout.
bool telegramAskToUnlock() {
  if (bot == nullptr) return false;

  // Flush any old pending messages so we don't read stale callbacks
  bot->getUpdates(bot->last_message_received + 1);

  // Build inline keyboard JSON: one row with Yes and No buttons
  String keyboard = "[[{\"text\":\"Yes\",\"callback_data\":\"unlock_yes\"},"
                     "{\"text\":\"No\",\"callback_data\":\"unlock_no\"}]]";

  bool sent = bot->sendMessageWithInlineKeyboard(CHAT_ID,
    "BLE auth failed. Unlock remotely?", "", keyboard);

  if (!sent) {
    Serial.println("Telegram: failed to send inline keyboard.");
    return false;
  }

  Serial.println("Telegram: sent inline keyboard, waiting for button press...");
  unsigned long pollStart = millis();

  while (millis() - pollStart < TELEGRAM_POLL_TIMEOUT_MS) {
    int numNewMessages = bot->getUpdates(bot->last_message_received + 1);

    for (int i = 0; i < numNewMessages; i++) {
      // Only accept callbacks from the authorized chat
      if (bot->messages[i].chat_id != String(CHAT_ID)) continue;
      if (bot->messages[i].type != "callback_query") continue;

      String data = bot->messages[i].text;

      if (data == "unlock_yes") {
        Serial.println("Telegram: YES button pressed — unlocking.");
        return true;
      }
      if (data == "unlock_no") {
        Serial.println("Telegram: NO button pressed — staying locked.");
        return false;
      }
    }

    delay(TELEGRAM_POLL_INTERVAL_MS);
  }

  Serial.println("Telegram: poll timed out.");
  return false;
}

// ---- Powerbank Wake ----

void wakePowerbank() {
  Serial.println("Powerbank: pressing button...");
  digitalWrite(POWERBANK_BTN_PIN, HIGH);
  delay(POWERBANK_PRESS_MS);
  digitalWrite(POWERBANK_BTN_PIN, LOW);
  delay(POWERBANK_WAKE_MS);
  Serial.println("Powerbank: USB 5V should be live.");
}

// ---- Servo Functions ----

void servoInit() {
  wakePowerbank();
  servo.setPeriodHertz(50);
  servo.attach(SERVO_PIN, 500, 2500);
  moveTo(START_POS);
}

void servoDetach() {
  servo.detach();
  pinMode(SERVO_PIN, OUTPUT);
  digitalWrite(SERVO_PIN, LOW);
}

void moveTo(int angle) {
  angle = constrain(angle, 0, 180);
  servo.write(angle);
  Serial.printf("Servo -> %d°\n", angle);
  delay(400);
}

void doAction(Action action) {
  if (action == LOCK) {
    Serial.println("LOCK");
    moveTo(LOCK_POS);
  } else {
    Serial.println("UNLOCK");
    moveTo(UNLOCK_POS);
  }
  moveTo(START_POS);
}

// ---- Sleep ----

void goToSleep() {
  touchSleepWakeUpEnable(TOUCH_PIN, TOUCH_THRESHOLD);

  Serial.println("Zzz... sleeping until touch.");
  delay(100);

  esp_deep_sleep_start();
}
