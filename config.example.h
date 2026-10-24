#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
//  RACK IDENTIFICATION
// ============================================================
#define RACK_ID     "rack-001"            // short unique id (no spaces)
#define RACK_LABEL  "Rack A-01"           // human-readable label

// ============================================================
//  WIFI
// ============================================================
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ============================================================
//  MQTT BROKER
// ============================================================
#define MQTT_SERVER    "mqtt.example.com"   // or IP
#define MQTT_PORT      1883
#define MQTT_USERNAME  "YOUR_MQTT_USER"
#define MQTT_PASSWORD  "YOUR_MQTT_PASSWORD"

// ============================================================
//  TELEGRAM BOT
// ============================================================
// 1. Create bot via @BotFather -> get token
// 2. Send a message to your bot
// 3. Get your chat_id via https://api.telegram.org/bot<TOKEN>/getUpdates
#define TELEGRAM_BOT_TOKEN "123456789:REPLACE_WITH_YOUR_BOT_TOKEN"
#define TELEGRAM_CHAT_ID   "000000000"

// ============================================================
//  DEFAULT THRESHOLDS (can be overridden remotely)
// ============================================================
#define DEFAULT_TEMP_MAX 30.0
#define DEFAULT_TEMP_MIN 16.0
#define DEFAULT_HUM_MAX  70.0
#define DEFAULT_HUM_MIN  20.0

// ============================================================
//  TIMING
// ============================================================
#define PUBLISH_INTERVAL_MS       2000
#define TELEGRAM_POLL_INTERVAL_MS 5000

#endif // CONFIG_H
