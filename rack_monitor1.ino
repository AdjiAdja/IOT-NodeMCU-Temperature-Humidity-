#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <time.h>

#include "config.h"   // <-- credentials live here (git-ignored)

// ---------------- DHT22 ----------------
#define DHTPIN  D2
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// ---------------- Thresholds (runtime, overridable via MQTT/Telegram) ----------------
float TEMP_MAX = DEFAULT_TEMP_MAX;
float TEMP_MIN = DEFAULT_TEMP_MIN;
float HUM_MAX  = DEFAULT_HUM_MAX;
float HUM_MIN  = DEFAULT_HUM_MIN;

// ---------------- MQTT topics (namespace per-rack) ----------------
String base_topic = String("dc/") + RACK_ID;
String temperature_topic;
String humidity_topic;
String status_topic;   // online/offline (retained, LWT)
String data_topic;     // JSON aggregate payload
String alert_topic;    // breach / recovered events
String control_topic;  // commands: restart, threshold:<tmax>,<tmin>,<hmax>,<hmin>

WiFiClient espClient;
PubSubClient client(espClient);

unsigned long lastPublish  = 0;
const long publishInterval = PUBLISH_INTERVAL_MS;

unsigned long lastReconnectAttempt = 0;
uint8_t reconnectFailCount = 0;
const uint8_t MAX_RECONNECT_FAIL = 12; // ~1 min consecutive failures -> forced restart

bool alertActiveTemp = false;
bool alertActiveHum  = false;

uint8_t sensorFailCount = 0;
bool sensorFailNotified = false;
const uint8_t SENSOR_FAIL_THRESHOLD = 5;

bool startupNotified = false;

// ---------------- Telegram polling state ----------------
long telegramLastUpdateId = 0;
unsigned long lastTelegramPoll = 0;

float lastTemperature = NAN;
float lastHumidity    = NAN;

// ================================================================
//  TELEGRAM HELPERS
// ================================================================
String urlEncode(const String& str) {
  String encoded = "";
  encoded.reserve(str.length() * 3 + 1);
  char c, code0, code1;
  for (unsigned int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (isalnum(c)) {
      encoded += c;
    } else if (c == ' ') {
      encoded += "%20";
    } else if (c == '\n') {
      encoded += "%0A";
    } else {
      code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9) code1 = (c & 0xf) - 10 + 'A';
      c = (c >> 4) & 0xf;
      code0 = c + '0';
      if (c > 9) code0 = c - 10 + 'A';
      encoded += '%';
      encoded += code0;
      encoded += code1;
    }
  }
  return encoded;
}

void sendTelegramMessage(const String& text) {
  if (WiFi.status() != WL_CONNECTED) return;

  std::unique_ptr<BearSSL::WiFiClientSecure> secureClient(new BearSSL::WiFiClientSecure);
  secureClient->setInsecure();
  secureClient->setBufferSizes(1024, 1024);

  HTTPClient https;
  String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN +
               "/sendMessage?chat_id=" + TELEGRAM_CHAT_ID +
               "&parse_mode=HTML&text=" + urlEncode(text);

  if (https.begin(*secureClient, url)) {
    int httpCode = https.GET();
    if (httpCode != HTTP_CODE_OK) {
      Serial.print("Telegram send failed, code: ");
      Serial.println(httpCode);
    }
    https.end();
  } else {
    Serial.println("HTTPS connect to Telegram failed");
  }
}

// ---------------- Telegram command handler ----------------
void handleTelegramCommand(const String& rawText) {
  String cmd = rawText;
  cmd.trim();

  if (cmd == "/status") {
    String reply = "===Rack Monitoring===\n";
    reply += "<b>Status Report</b> 📊\n";
    reply += "<b>Rack:</b> " + String(RACK_LABEL) + "\n\n";
    reply += "🌡️ <b>Temp:</b> " + String(lastTemperature, 1) + " C\n";
    reply += "💧 <b>Humidity:</b> " + String(lastHumidity, 1) + " %RH\n\n";
    reply += "⏱️ <b>Uptime:</b> " + String(millis() / 1000) + " s\n";
    reply += "🌐 <b>MQTT:</b> " + String(client.connected() ? "Connected ✅" : "Disconnected ❌");
    sendTelegramMessage(reply);

  } else if (cmd == "/threshold") {
    String reply = "===Rack Monitoring===\n";
    reply += "<b>Safety Thresholds</b> ⚙️\n";
    reply += "<b>Rack:</b> " + String(RACK_LABEL) + "\n\n";
    reply += "🌡️ <b>Temp:</b> " + String(TEMP_MIN, 1) + " C - " + String(TEMP_MAX, 1) + " C\n";
    reply += "💧 <b>Humidity:</b> " + String(HUM_MIN, 1) + " %RH - " + String(HUM_MAX, 1) + " %RH\n\n";
    reply += "<i>Use /setthreshold to change.</i>";
    sendTelegramMessage(reply);

  } else if (cmd.startsWith("/setthreshold")) {
    String args = cmd.substring(13);
    args.trim();
    float t_max, t_min, h_max, h_min;
    int n = sscanf(args.c_str(), "%f %f %f %f", &t_max, &t_min, &h_max, &h_min);
    if (n == 4) {
      TEMP_MAX = t_max; TEMP_MIN = t_min;
      HUM_MAX  = h_max; HUM_MIN  = h_min;
      String reply = "===Rack Monitoring===\n";
      reply += "<b>Thresholds Updated!</b> ✅\n";
      reply += "<b>Rack:</b> " + String(RACK_LABEL) + "\n\n";
      reply += "🌡️ <b>Temp:</b> " + String(TEMP_MIN, 1) + " C - " + String(TEMP_MAX, 1) + " C\n";
      reply += "💧 <b>Humidity:</b> " + String(HUM_MIN, 1) + " %RH - " + String(HUM_MAX, 1) + " %RH";
      sendTelegramMessage(reply);
    } else {
      String reply = "===Rack Monitoring===\n";
      reply += "⚠️ <b>Invalid format!</b>\n";
      reply += "Correct format:\n<code>/setthreshold [T-Max] [T-Min] [H-Max] [H-Min]</code>\n";
      reply += "Example: <code>/setthreshold 30 16 70 20</code>";
      sendTelegramMessage(reply);
    }

  } else if (cmd == "/restart") {
    String reply = "====Rack Monitor====\n";
    reply += "🔄 <b>Restarting node...</b>\n";
    reply += "<b>Rack:</b> " + String(RACK_LABEL);
    sendTelegramMessage(reply);
    delay(300);
    ESP.restart();

  } else {
    String reply = "====Rack Monitor====\n";
    reply += "Hello! I'm your monitoring assistant. 🤖\n";
    reply += "Available commands for <b>" + String(RACK_LABEL) + "</b>:\n\n";
    reply += "📊 /status - Current metrics\n";
    reply += "⚙️ /threshold - Show safety thresholds\n";
    reply += "🛠️ /setthreshold - Change thresholds\n";
    reply += "🔄 /restart - Restart monitoring node";
    sendTelegramMessage(reply);
  }
}

// ---------------- Telegram short polling ----------------
void pollTelegramUpdates() {
  if (WiFi.status() != WL_CONNECTED) return;

  std::unique_ptr<BearSSL::WiFiClientSecure> secureClient(new BearSSL::WiFiClientSecure);
  secureClient->setInsecure();
  secureClient->setBufferSizes(1024, 1024);

  HTTPClient https;
  String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN +
               "/getUpdates?offset=" + String(telegramLastUpdateId + 1) +
               "&limit=1&timeout=0";

  if (!https.begin(*secureClient, url)) {
    Serial.println("HTTPS connect for getUpdates failed");
    return;
  }

  int httpCode = https.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.print("getUpdates failed, code: ");
    Serial.println(httpCode);
    https.end();
    return;
  }

  String payload = https.getString();
  https.end();

  DynamicJsonDocument doc(768);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("getUpdates JSON parse failed: ");
    Serial.println(err.c_str());
    return;
  }

  JsonArray results = doc["result"].as<JsonArray>();
  if (results.size() == 0) return;

  JsonObject update = results[0];
  telegramLastUpdateId = update["update_id"].as<long>();

  if (!update.containsKey("message")) return;
  JsonObject message = update["message"];
  String fromChatId = message["chat"]["id"].as<String>();
  String text = message["text"] | "";

  if (fromChatId != String(TELEGRAM_CHAT_ID)) {
    Serial.println("Telegram message from unknown chat_id ignored");
    return;
  }

  handleTelegramCommand(text);
}

// ---------------- WiFi ----------------
void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

// ---------------- NTP ----------------
void setup_time() {
  configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com"); // WIB (UTC+7)
  Serial.print("NTP sync");
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {
    delay(300);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println(" done");
}

String isoTimestamp() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buf);
}

// ---------------- OTA ----------------
void setup_ota() {
  ArduinoOTA.setHostname((String("rack-") + RACK_ID).c_str());
  ArduinoOTA.onStart([]() { Serial.println("OTA start"); });
  ArduinoOTA.onEnd([]()   { Serial.println("\nOTA done"); });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]\n", error);
  });
  ArduinoOTA.begin();
}

// ---------------- MQTT callback ----------------
void callback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

  Serial.print("Incoming [");
  Serial.print(topic);
  Serial.print("] ");
  Serial.println(message);

  if (String(topic) == control_topic) {
    if (message == "restart") {
      Serial.println("Restart command received");
      client.publish(status_topic.c_str(), "restarting", true);
      delay(500);
      ESP.restart();
    } else if (message.startsWith("threshold:")) {
      String vals = message.substring(10);
      float t_max, t_min, h_max, h_min;
      int n = sscanf(vals.c_str(), "%f,%f,%f,%f", &t_max, &t_min, &h_max, &h_min);
      if (n == 4) {
        TEMP_MAX = t_max; TEMP_MIN = t_min;
        HUM_MAX  = h_max; HUM_MIN  = h_min;
        Serial.println("Thresholds updated via MQTT");
      }
    }
  }
}

// ---------------- MQTT reconnect with LWT ----------------
bool reconnect_once() {
  String clientId = String("ESP8266-") + RACK_ID;

  bool ok = client.connect(
    clientId.c_str(),
    MQTT_USERNAME, MQTT_PASSWORD,
    status_topic.c_str(), 1, true, "offline"
  );

  if (ok) {
    Serial.println("MQTT connected");
    client.publish(status_topic.c_str(), "online", true);
    client.subscribe(control_topic.c_str());
    reconnectFailCount = 0;

    if (!startupNotified) {
      sendTelegramMessage(
        "<b>Node online</b>\n" +
        String("Rack: ") + RACK_LABEL + "\n" +
        "Status: online"
      );
      startupNotified = true;
    }
  } else {
    reconnectFailCount++;
    Serial.print("MQTT fail, rc=");
    Serial.print(client.state());
    Serial.print(" attempt #");
    Serial.println(reconnectFailCount);
  }
  return ok;
}

void handleReconnect() {
  unsigned long nowMs = millis();
  if (nowMs - lastReconnectAttempt < 5000) return;
  lastReconnectAttempt = nowMs;

  if (!reconnect_once()) {
    if (reconnectFailCount >= MAX_RECONNECT_FAIL) {
      Serial.println("Too many failures, force restart");
      ESP.restart();
    }
  }
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(9600);

  temperature_topic = base_topic + "/temp";
  humidity_topic    = base_topic + "/hum";
  status_topic      = base_topic + "/status";
  data_topic        = base_topic + "/data";
  alert_topic       = base_topic + "/alert";
  control_topic     = base_topic + "/ctrl";

  setup_wifi();
  setup_time();
  setup_ota();

  client.setServer(MQTT_SERVER, MQTT_PORT);
  client.setCallback(callback);
  dht.begin();
}

// ---------------- Threshold checks -> MQTT + Telegram ----------------
void checkThresholds(float t, float h, const String& ts) {
  StaticJsonDocument<192> doc;
  char buf[192];

  bool tempOut = (t > TEMP_MAX || t < TEMP_MIN);
  bool humOut  = (h > HUM_MAX  || h < HUM_MIN);

  if (tempOut && !alertActiveTemp) {
    doc.clear();
    doc["rack"] = RACK_ID; doc["type"] = "temperature"; doc["value"] = t;
    doc["limit_max"] = TEMP_MAX; doc["limit_min"] = TEMP_MIN;
    doc["state"] = "breach"; doc["timestamp"] = ts;
    serializeJson(doc, buf);
    client.publish(alert_topic.c_str(), buf, true);

    sendTelegramMessage(
      "<b>Temperature out of range</b>\n" +
      String("Rack: ") + RACK_LABEL + "\n" +
      "Value: " + String(t, 1) + " C\n" +
      "Normal: " + String(TEMP_MIN, 1) + " - " + String(TEMP_MAX, 1) + " C\n" +
      "Time: " + ts
    );
    alertActiveTemp = true;
  } else if (!tempOut && alertActiveTemp) {
    doc.clear();
    doc["rack"] = RACK_ID; doc["type"] = "temperature"; doc["value"] = t;
    doc["state"] = "recovered"; doc["timestamp"] = ts;
    serializeJson(doc, buf);
    client.publish(alert_topic.c_str(), buf, true);

    sendTelegramMessage(
      "<b>Temperature back to normal</b>\n" +
      String("Rack: ") + RACK_LABEL + "\n" +
      "Value: " + String(t, 1) + " C\n" +
      "Time: " + ts
    );
    alertActiveTemp = false;
  }

  if (humOut && !alertActiveHum) {
    doc.clear();
    doc["rack"] = RACK_ID; doc["type"] = "humidity"; doc["value"] = h;
    doc["limit_max"] = HUM_MAX; doc["limit_min"] = HUM_MIN;
    doc["state"] = "breach"; doc["timestamp"] = ts;
    serializeJson(doc, buf);
    client.publish(alert_topic.c_str(), buf, true);

    sendTelegramMessage(
      "<b>Humidity out of range</b>\n" +
      String("Rack: ") + RACK_LABEL + "\n" +
      "Value: " + String(h, 1) + " %RH\n" +
      "Normal: " + String(HUM_MIN, 1) + " - " + String(HUM_MAX, 1) + " %RH\n" +
      "Time: " + ts
    );
    alertActiveHum = true;
  } else if (!humOut && alertActiveHum) {
    doc.clear();
    doc["rack"] = RACK_ID; doc["type"] = "humidity"; doc["value"] = h;
    doc["state"] = "recovered"; doc["timestamp"] = ts;
    serializeJson(doc, buf);
    client.publish(alert_topic.c_str(), buf, true);

    sendTelegramMessage(
      "<b>Humidity back to normal</b>\n" +
      String("Rack: ") + RACK_LABEL + "\n" +
      "Value: " + String(h, 1) + " %RH\n" +
      "Time: " + ts
    );
    alertActiveHum = false;
  }
}

// ---------------- Main loop ----------------
void loop() {
  ArduinoOTA.handle();

  if (!client.connected()) {
    handleReconnect();
  } else {
    client.loop();
  }

  unsigned long nowMs = millis();

  if (nowMs - lastTelegramPoll >= TELEGRAM_POLL_INTERVAL_MS) {
    lastTelegramPoll = nowMs;
    pollTelegramUpdates();
  }

  if (nowMs - lastPublish < publishInterval) return;
  lastPublish = nowMs;

  float temperature = dht.readTemperature();
  float humidity    = dht.readHumidity();

  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("DHT22 read failed");
    sensorFailCount++;
    if (client.connected()) {
      client.publish(status_topic.c_str(), "sensor_error");
    }
    if (sensorFailCount >= SENSOR_FAIL_THRESHOLD && !sensorFailNotified) {
      sendTelegramMessage(
        "<b>DHT22 sensor problem</b>\n" +
        String("Rack: ") + RACK_LABEL + "\n" +
        "Failed " + String(sensorFailCount) + "x consecutively, check wiring/sensor."
      );
      sensorFailNotified = true;
    }
    return;
  }
  sensorFailCount = 0;
  sensorFailNotified = false;
  lastTemperature = temperature;
  lastHumidity = humidity;

  String ts = isoTimestamp();

  char temp_buffer[8]; dtostrf(temperature, 4, 2, temp_buffer);
  char hum_buffer[8];  dtostrf(humidity, 4, 2, hum_buffer);

  if (client.connected()) {
    client.publish(temperature_topic.c_str(), temp_buffer);
    client.publish(humidity_topic.c_str(), hum_buffer);

    StaticJsonDocument<192> doc;
    doc["rack"] = RACK_ID;
    doc["temp"] = temperature;
    doc["hum"] = humidity;
    doc["timestamp"] = ts;
    char json_buffer[192];
    serializeJson(doc, json_buffer);
    client.publish(data_topic.c_str(), json_buffer);
  }

  checkThresholds(temperature, humidity, ts);

  Serial.print("Rack "); Serial.print(RACK_ID);
  Serial.print(" | T: "); Serial.print(temp_buffer);
  Serial.print(" C, H: "); Serial.print(hum_buffer);
  Serial.println(" %");
}
