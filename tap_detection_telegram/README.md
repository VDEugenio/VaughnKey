# DoorLock + Telegram — Retrofit Smart Lock with Notifications

## Overview
A battery-powered retrofit smart lock built on an ESP32 with Telegram integration. The user interacts with the lock by touching a capacitive touch sensor mounted on the door's peephole. The ESP32 stays in deep sleep (~10uA draw) at all times until a touch wakes it. Based on the touch gesture and BLE authentication, it drives a servo to turn the existing deadbolt. After every action, a Telegram notification is sent over WiFi. If BLE authentication fails (e.g., a catsitter without a beacon), the lock sends an interactive Telegram prompt with Yes/No buttons and waits up to 2 minutes for a remote unlock decision.

This is a variant of `tap_detection.ino` — all existing behavior (touch detection, BLE scan, servo control, deep sleep) is preserved, with WiFi + Telegram layered on top.

## Hardware
- **ESP32** — main microcontroller, runs on battery, deep sleep between interactions
- **Capacitive touch sensor on T0 (GPIO4)** — mounted on the peephole, acts as the only user input
- **DS-S012 servo on GPIO18** — physically turns the deadbolt
- **BLE beacon (nRF51822 iBeacon)** — keychain beacon that constantly advertises a service UUID. This replaces a phone-based BLE peripheral approach because iOS kills background BLE advertising from apps like LightBlue/nRF Connect
- **Powerbank** — provides 5V to the servo. A 2N2222A transistor on GPIO14 electronically "presses" the powerbank's power button to wake its USB output before each servo operation

## Gesture System
The lock uses touch **duration**:
- **Quick tap (< 1 second)** — triggers `TAP_ACTION`
- **Long hold (>= 1 second)** — triggers `HOLD_ACTION`

Which gesture maps to lock vs unlock is controlled by two `#define`s at the top of the file — a one-line swap. **Current mapping:**
```
TAP_ACTION  = UNLOCK
HOLD_ACTION = LOCK
```

## Full Execution Flow

### 1. Deep Sleep (default state)
The ESP32 is in deep sleep. The only thing running is the ULP (ultra-low-power) touch sensor peripheral on GPIO4, waiting for a capacitive touch event. Power draw is ~10uA. The servo pin (GPIO18) is pulled LOW so the servo doesn't jitter.

### 2. Wake on Touch
When the peephole is touched, the touch peripheral triggers `ESP_SLEEP_WAKEUP_TOUCHPAD`. The ESP32 boots fresh into `setup()` (deep sleep wipes RAM — there's no `loop()` usage at all, everything happens in `setup()` then it goes back to sleep).

### 3. Measure Touch Duration
`measureTouch()` immediately starts timing how long the finger stays on the sensor:
- Polls `touchRead(T0)` every 10ms
- If the value stays below `TOUCH_THRESHOLD` (currently 40, tunable) for >= `LONG_HOLD_MS` (currently 1000ms), it returns `true` (long hold)
- If the finger lifts before that, it returns `false` (quick tap)
- The duration is printed to Serial for debugging

### 4. Determine Action
Based on the touch duration and the action mapping:
```cpp
Action action = isLongHold ? HOLD_ACTION : TAP_ACTION;
```

### 5a. If LOCK
Lock executes **immediately** with no authentication:
1. `wakePowerbank()` — pulses GPIO14 HIGH for 200ms to electronically press the powerbank button, then waits 1500ms for the USB 5V output to stabilize
2. `servoInit()` — attaches servo at 50Hz PWM, pulse range 500-2500us, moves to neutral (90 degrees)
3. `doAction(LOCK)` — sweeps servo to `LOCK_POS` (170 degrees), waits 400ms, then returns to `START_POS` (90 degrees)
4. `servoDetach()` — detaches servo, pulls GPIO18 LOW
5. `wifiConnect()` — connects to WiFi (up to 10 seconds timeout)
6. `telegramSend("Door locked")` — sends notification
7. `wifiDisconnect()` — tears down WiFi before sleep

### 5b. If UNLOCK — BLE succeeds
Unlock requires **BLE authentication** before the servo moves:
1. `bleScanForTrusted()` is called:
   - Initializes BLE stack (`BLEDevice::init("DoorLock")`)
   - Runs an active BLE scan for `BLE_SCAN_TIME` (currently 1 second)
   - Scan settings: interval 100ms, window 99ms (near-continuous scanning)
   - Iterates through all discovered devices, checking each against the `TRUSTED_DEVICES` map (defined in `config.h`)
   - If a match is found, checks RSSI against `BLE_RSSI_MIN` (currently -70 dBm) to verify proximity
   - On match, stores the friendly name (e.g., "Vaughn") in `matchedName` for use in the Telegram notification
   - Returns `true` only if a trusted device is found AND close enough
2. `bleDeinit()` — frees ~170KB BLE RAM **before** WiFi init (both use the same radio and RAM is limited)
3. Servo unlocks (sweeps to `UNLOCK_POS` 10 degrees, returns to 90 degrees)
4. WiFi connects, sends Telegram notification (e.g., "Door unlocked by Vaughn"), WiFi disconnects

### 5c. If UNLOCK — BLE fails (Telegram remote unlock)
If no trusted BLE device is found or the device is too far away:
1. `bleDeinit()` — frees BLE RAM
2. `wifiConnect()` — connects to WiFi
3. `telegramAskToUnlock()` is called:
   - Flushes any old pending messages with `getUpdates()` to prevent stale responses
   - Sends an inline keyboard with **Yes** and **No** buttons to the authorized chat
   - Polls `getUpdates()` every 2 seconds for up to 2 minutes
   - Only accepts `callback_query` messages from the authorized `CHAT_ID` — ignores all other chats
   - If **Yes** button pressed: returns `true`, servo unlocks, sends "Door unlocked remotely via Telegram"
   - If **No** button pressed: returns `false`, sends "Unlock denied or timed out. Staying locked."
   - If timeout (2 minutes, no response): returns `false`, sends denial message
4. `wifiDisconnect()` — tears down WiFi

### 6. Cleanup & Sleep
- `bleDeinit()` — safety net call (no-op if already deinited)
- `goToSleep()` — re-enables touch wakeup via `touchSleepWakeUpEnable(T0, 40)`, then calls `esp_deep_sleep_start()`
- The cycle repeats

## Memory Management — BLE/WiFi Sequencing
The ESP32 BLE stack allocates ~170KB of RAM. WiFi + TLS needs ~50-80KB. The ESP32 only has ~320KB total. Running both simultaneously risks heap exhaustion. The firmware always follows this sequence:
1. BLE scan runs first
2. `bleDeinit()` frees BLE memory
3. Only then does `wifiConnect()` initialize WiFi

For the LOCK path, BLE is never initialized (no authentication needed), so WiFi can start directly.

## Servo Behavior
The servo always returns to neutral (90 degrees) after actuating. This is important because the servo is not continuously powered — it moves the deadbolt, returns to center, and gets detached. The mechanical linkage between the servo and deadbolt needs to accommodate this (servo horn engages the thumbturn, rotates it, returns to neutral).

- **Lock position:** 170 degrees (+80 degrees from center)
- **Unlock position:** 10 degrees (-80 degrees from center)
- **Neutral/start:** 90 degrees
- **Movement delay:** 400ms per move (two moves per action: go to position + return to center)

## BLE Authentication — Trusted Devices Map
The firmware supports **multiple trusted BLE devices** via a map defined in `config.h`:
```cpp
#define TRUSTED_DEVICES { \
  { "VaughnKey", "Vaughn" }, \
  { "CatsitterBeacon", "Catsitter" }, \
}
```
Each entry is a `{ BLE_name, friendly_name }` pair. The BLE name is what the ESP32 scans for. The friendly name is included in Telegram notifications (e.g., "Door unlocked by Vaughn"). To add or remove authorized users, edit this map in `config.h`.

**Known limitation:** BLE device names are not authenticated — they can be spoofed by anyone with a $5 BLE beacon. UUID scanning (planned) is slightly better. Proper security would require encrypted challenge-response pairing.

**RSSI threshold:** Currently set to -70 dBm. A calibration sketch (`rssi_calibration/rssi_calibration.ino`) exists for dialing this in.

## Telegram Setup

### 1. Create a Bot
1. Open Telegram and search for **@BotFather**
2. Send `/newbot`
3. Choose a name and username for your bot
4. BotFather will give you a **bot token** like `123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11`

### 2. Get Your Chat ID
1. Send any message to your new bot in Telegram
2. Open this URL in a browser (replace `<TOKEN>` with your bot token):
   ```
   https://api.telegram.org/bot<TOKEN>/getUpdates
   ```
3. Look for `"chat":{"id":123456789}` in the JSON response

### 3. Configure config.h
Copy `config.example.h` to `config.h` and fill in your values:
```cpp
#define WIFI_SSID          "YourNetworkName"
#define WIFI_PASSWORD      "YourPassword"
#define BOT_TOKEN          "your-bot-token-here"
#define CHAT_ID            "your-chat-id-here"
#define TRUSTED_DEVICES { \
  { "XYZ_KEY", "XYZ Name" }, \
}
```
`config.h` is gitignored — your credentials are never pushed to GitHub.

## TLS Security
The firmware uses `setCACert(TELEGRAM_CERTIFICATE_ROOT)` for TLS certificate validation when connecting to the Telegram API. This prevents man-in-the-middle attacks on the WiFi network from intercepting bot traffic. The root certificate is provided by the UniversalTelegramBot library.

## Tunable Parameters

### In the .ino file
| Parameter | Current Value | Purpose |
|-----------|---------------|---------|
| `TAP_ACTION` | `UNLOCK` | What a quick tap does (swap with HOLD_ACTION to flip) |
| `HOLD_ACTION` | `LOCK` | What a long hold does |
| `TOUCH_THRESHOLD` | 40 | Capacitive touch sensitivity (lower = more sensitive) |
| `LONG_HOLD_MS` | 1000 | Milliseconds to distinguish tap from hold |
| `BLE_SCAN_TIME` | 1 | Seconds to scan for BLE devices |
| `BLE_RSSI_MIN` | -70 | Minimum signal strength (closer = less negative) |
| `LOCK_POS` | 170 | Servo angle for locking |
| `UNLOCK_POS` | 10 | Servo angle for unlocking |
| `START_POS` | 90 | Servo neutral position |
| `POWERBANK_PRESS_MS` | 200 | How long to "hold" the powerbank button |
| `POWERBANK_WAKE_MS` | 1500 | Stabilization delay after powerbank wake |
| `WIFI_TIMEOUT_MS` | 10000 | Max ms to wait for WiFi connection |
| `TELEGRAM_POLL_INTERVAL_MS` | 2000 | How often to check for a Telegram reply |
| `TELEGRAM_POLL_TIMEOUT_MS` | 120000 | Max wait for remote unlock reply (2 min) |

### In config.h (gitignored)
| Parameter | Purpose |
|-----------|---------|
| `WIFI_SSID` | WiFi network name |
| `WIFI_PASSWORD` | WiFi password |
| `BOT_TOKEN` | Telegram bot token from BotFather |
| `CHAT_ID` | Your Telegram chat ID |
| `TRUSTED_DEVICES` | Map of BLE device names to friendly names |

## Battery Life Estimate (3000 mAh, 10 events/day)

| Option | Description | Daily Draw | Battery Life |
|--------|-------------|------------|-------------|
| **A** | No Telegram | ~0.9 mAh | ~9 years* |
| **B** | Notify on lock/unlock/failed auth | ~3.0 mAh | ~2.7 years* |
| **C** | B + interactive remote unlock (~3x/month) | ~3.05 mAh | ~2.7 years* |

*\*Theoretical max — Li-ion self-discharge (~2-3%/month) limits real battery life to ~1-2 years regardless.*

The 2-minute WiFi polling session costs ~5 mAh per event, but at ~3 uses/month it adds negligible daily draw.

## Known Security Considerations
1. **BLE name spoofing** — device names are unauthenticated and can be copied by anyone with a cheap BLE beacon. UUID scanning is planned as a slight improvement; proper fix is encrypted challenge-response.
3. **Locking has no authentication** — anyone who touches the peephole can lock the door (by design, but worth noting).

## Libraries Required (Arduino IDE)

Install via Library Manager:
- **ESP32Servo** — servo control for ESP32
- **UniversalTelegramBot** (by Brian Lough) — Telegram Bot API wrapper
- **ArduinoJson** — dependency of UniversalTelegramBot

Built into ESP32 Arduino core (no install needed):
- `WiFi.h`, `WiFiClientSecure.h`
- `BLEDevice.h`, `BLEScan.h`, `BLEAdvertisedDevice.h`
- `esp_sleep.h`

## Board Settings
- Board: ESP32 (any variant)
- Partition Scheme: **Huge APP (3MB No OTA/No SPIFFS)** — needed because WiFi + TLS + BLE exceeds the default 1.3MB partition
- Framework: Arduino
