# DoorLock + Telegram — Firmware Variant

A version of the DoorLock firmware that adds Telegram notifications for every lock/unlock event, plus an interactive remote unlock fallback when BLE authentication fails.

## How It Differs From `tap_detection.ino`

The base `tap_detection.ino` is fully offline — touch, BLE, servo, sleep. This variant adds WiFi connectivity after each action to send a Telegram message. When BLE auth fails (e.g., a catsitter without your beacon), it sends you a Telegram prompt and waits up to 2 minutes for you to reply YES or NO to unlock remotely.

All existing behavior (touch detection, BLE scan, servo control, deep sleep) is identical.

## Execution Flow

```
                    ┌─────────────┐
                    │  Deep Sleep  │
                    │   (~10 uA)  │
                    └──────┬──────┘
                           │ touch wake
                           ▼
                    ┌──────────────┐
                    │ Measure Touch│
                    │  Duration    │
                    └──────┬───────┘
                           │
                ┌──────────┴──────────┐
                ▼                     ▼
          Quick Tap              Long Hold
          (UNLOCK)                (LOCK)
                │                     │
                ▼                     ▼
         ┌────────────┐        ┌───────────┐
         │  BLE Scan   │        │ Servo LOCK │
         │ for beacon  │        └─────┬─────┘
         └──────┬──────┘              │
                │                     ▼
         ┌──────┴──────┐       WiFi → Telegram
         │             │       "Door locked"
      Found        Not Found         │
         │             │              ▼
         ▼             ▼           Sleep
   ┌───────────┐  ┌──────────────────────┐
   │Servo UNLOCK│  │ WiFi → Telegram      │
   └─────┬─────┘  │ "BLE failed. YES to  │
         │        │  unlock?"             │
         ▼        └──────────┬───────────┘
  WiFi → Telegram            │
  "Door unlocked"     ┌──────┴──────┐
         │            │             │
         ▼          "YES"         "NO" / timeout
       Sleep          │             │
                      ▼             ▼
               ┌───────────┐  Telegram:
               │Servo UNLOCK│  "Staying locked"
               └─────┬─────┘       │
                     │              ▼
                     ▼           Sleep
              Telegram:
              "Unlocked remotely"
                     │
                     ▼
                   Sleep
```

## Telegram Bot Setup

### 1. Create a Bot
1. Open Telegram and search for **@BotFather**
2. Send `/newbot`
3. Choose a name and username for your bot
4. BotFather will give you a **bot token** like `123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11`
5. Copy this into `BOT_TOKEN` in the .ino file

### 2. Get Your Chat ID
1. Send any message to your new bot in Telegram
2. Open this URL in a browser (replace `<TOKEN>` with your bot token):
   ```
   https://api.telegram.org/bot<TOKEN>/getUpdates
   ```
3. Look for `"chat":{"id":123456789}` in the JSON response
4. Copy that number into `CHAT_ID` in the .ino file

### 3. Set WiFi Credentials
Update these defines in the .ino file:
```cpp
#define WIFI_SSID      "YourNetworkName"
#define WIFI_PASSWORD   "YourPassword"
```

## Configuration

All tunables are `#define`s at the top of the file:

| Parameter | Default | Purpose |
|---|---|---|
| `WIFI_SSID` | `"YOUR_SSID"` | WiFi network name |
| `WIFI_PASSWORD` | `"YOUR_PASSWORD"` | WiFi password |
| `BOT_TOKEN` | `"YOUR_BOT_TOKEN"` | Telegram bot token from BotFather |
| `CHAT_ID` | `"YOUR_CHAT_ID"` | Your Telegram chat ID |
| `WIFI_TIMEOUT_MS` | `10000` | Max ms to wait for WiFi connection |
| `TELEGRAM_POLL_INTERVAL_MS` | `3000` | How often to check for a reply (ms) |
| `TELEGRAM_POLL_TIMEOUT_MS` | `120000` | Max wait for YES/NO reply (2 min) |

All existing parameters (touch, BLE, servo, powerbank) are unchanged from `tap_detection.ino`.

## Battery Life Comparison (3000 mAh, 10 events/day)

| Option | Description | Daily Draw | Battery Life |
|---|---|---|---|
| **A** | No Telegram | ~0.9 mAh | ~9 years* |
| **B** | Notify on lock/unlock/failed auth | ~3.0 mAh | ~2.7 years* |
| **C** | B + interactive remote unlock (3x/month) | ~3.05 mAh | ~2.7 years* |

*\*Theoretical max — Li-ion self-discharge (~2-3%/month) limits real battery life to ~1-2 years regardless.*

The 2-minute WiFi polling session is expensive per event (~5 mAh), but at only ~3 uses/month it adds negligible daily draw.

## Required Libraries

Install via Arduino IDE Library Manager:
- **ESP32Servo** — servo control for ESP32
- **UniversalTelegramBot** (by Brian Lough) — Telegram Bot API wrapper
- **ArduinoJson** (dependency of UniversalTelegramBot)

Built into ESP32 Arduino core (no install needed):
- `WiFi.h`, `WiFiClientSecure.h`
- `BLEDevice.h`, `BLEScan.h`, `BLEAdvertisedDevice.h`
- `esp_sleep.h`

## Board Settings

- Board: ESP32 (any variant)
- Serial baud: 115200
- Framework: Arduino
