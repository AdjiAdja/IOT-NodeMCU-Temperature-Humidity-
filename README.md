# IOT-NodeMCU-Temperature-Humidity-
IoT environmental monitoring for server racks using ESP8266 + DHT22. Publishes temperature/humidity to MQTT, sends alerts via Telegram, supports OTA updates, and remote threshold configuration.
# ESP8266 Rack Monitor

IoT environmental monitoring node for server racks based on **ESP8266 + DHT22**.

## Features

- 🌡️ Temperature & humidity monitoring (DHT22)
- 📡 MQTT publish (temp, humidity, JSON data, LWT status)
- 🚨 Threshold-based alerts (MQTT + Telegram)
- 🤖 Telegram bot with remote commands (`/status`, `/threshold`, `/setthreshold`, `/restart`)
- 🔄 OTA firmware updates (ArduinoOTA)
- 🕒 NTP timestamping (ISO-8601)
- 🔁 Auto-reconnect with forced restart after repeated failures
- 💓 Sensor failure detection & notification

## Hardware

| Component | Pin |
|-----------|-----|
| DHT22 Data | D2 (GPIO4) |
| VCC | 3.3V |
| GND | GND |

> Add a 4.7k–10kΩ pull-up resistor between DHT22 DATA and VCC.

## Quick Start

1. Copy `config.example.h` → `config.h`
2. Fill in your WiFi, MQTT, and Telegram credentials
3. Open `rack_monitor1.ino` in Arduino IDE
4. Install libraries (see below)
5. Select board: **NodeMCU 1.0 (ESP-12E Module)**
6. Upload

### Required Libraries

Install via Arduino Library Manager:

- `PubSubClient` (Nick O'Leary)
- `DHT sensor library` (Adafruit)
- `Adafruit Unified Sensor`
- `ArduinoJson` (v6.x)
- `ESP8266WiFi`, `ESP8266HTTPClient`, `ESP8266mDNS`, `ArduinoOTA` (bundled with ESP8266 core)

### Board Manager URL
