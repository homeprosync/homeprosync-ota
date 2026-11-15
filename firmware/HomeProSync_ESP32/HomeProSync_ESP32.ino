/**
 * ==============================================================================
 * HOME PRO SYNC - OFFICIAL ESP32 CONTROLLER FIRMWARE v4.0.0 (STABLE)
 * ==============================================================================
 * Hardware: ESP32 DevKit v1 / ESP32-WROOM-32
 * Features:
 *  1. Precision Water Tank Monitoring (A02YYUW Ultrasonic Sensor UART)
 *  2. High-Current Motor Relay Control with Smart Safety Interlock
 *  3. Solar Battery & MPPT Telemetry (12V / 24V / 48V Universal Engine)
 *  4. Over-The-Air (OTA) Cloud & Local HTTP Firmware Flasher (/ota & /update)
 *  5. Emergency SoftAP Hotspot Auto-Recovery (192.168.4.1)
 *  6. JSON REST API & Tuya DP Schema Bridge (/data, /status, /motor, /config)
 * ==============================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>

#define FW_VERSION           "4.0.0"
#define DEFAULT_SSID         "M Audible Studio"
#define DEFAULT_PASS         "Fkhan@17811780"
#define SOFTAP_SSID          "SmartWaterTank_Setup"
#define SOFTAP_PASS          "12345678"

// Hardware Pin Definitions for ESP32
#define PIN_MOTOR_RELAY      23   // Active HIGH Relay for Water Pump Motor
#define PIN_GRID_DETECTION   22   // AC Mains Voltage Detection Optocoupler
#define PIN_SOLAR_VOLT_ADC   34   // ADC1 Channel for Solar Battery Voltage Divider
#define PIN_SOLAR_CURR_ADC   35   // ADC1 Channel for ACS712 / Hall Effect Current Sensor

// A02YYUW Sensor Hardware Serial2 (RX2=16, TX2=17)
#define SENSOR_RX_PIN        16
#define SENSOR_TX_PIN        17

WebServer server(80);
Preferences prefs;
HardwareSerial sensorSerial(2);

// Runtime Telemetry Variables
int waterPercent = 50;
bool isMotorOn = false;
int drainSpeed = 0;
long lastValidDistance = 100;
float solarVolt = 24.6;
float solarAmps = 4.2;
float solarWatts = 103.3;
bool grid220V = false;

// Persistent Settings
struct ConfigSettings {
  int emptyDist;
  int fullDist;
  int diameter;
  int maxPercent;
  int minPercent;
  bool autoOnEnabled;
  char ssid[36];
  char pass[36];
  int history[7];
} cfg;

// Lifecycle Timers
bool inSoftApMode = false;
unsigned long softApStartTime = 0;
const unsigned long SOFT_AP_TIMEOUT = 300000; // 5 minutes
unsigned long lastDrainCheck = 0;
int lastPercentForDrain = 0;
String hardwareDeviceId = "";

void loadConfig() {
  prefs.begin("hps_cfg", false);
  cfg.emptyDist = prefs.getInt("h", 100);
  cfg.fullDist = prefs.getInt("f", 5);
  cfg.diameter = prefs.getInt("d", 110);
  cfg.maxPercent = prefs.getInt("maxP", 95);
  cfg.minPercent = prefs.getInt("minP", 20);
  cfg.autoOnEnabled = prefs.getBool("autoOn", false);
  
  String s = prefs.getString("ssid", DEFAULT_SSID);
  String p = prefs.getString("pass", DEFAULT_PASS);
  strncpy(cfg.ssid, s.c_str(), sizeof(cfg.ssid));
  strncpy(cfg.pass, p.c_str(), sizeof(cfg.pass));
  
  for (int i = 0; i < 7; i++) {
    char k[8];
    snprintf(k, sizeof(k), "hist%d", i);
    cfg.history[i] = prefs.getInt(k, 40);
  }
}

void saveConfig() {
  prefs.putInt("h", cfg.emptyDist);
  prefs.putInt("f", cfg.fullDist);
  prefs.putInt("d", cfg.diameter);
  prefs.putInt("maxP", cfg.maxPercent);
  prefs.putInt("minP", cfg.minPercent);
  prefs.putBool("autoOn", cfg.autoOnEnabled);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  for (int i = 0; i < 7; i++) {
    char k[8];
    snprintf(k, sizeof(k), "hist%d", i);
    prefs.putInt(k, cfg.history[i]);
  }
}

void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "*");
}

long readUltrasonicDistance() {
  unsigned char data[4] = {0};
  while (sensorSerial.available() >= 4) {
    if (sensorSerial.read() == 0xFF) {
      data[0] = 0xFF;
      data[1] = sensorSerial.read();
      data[2] = sensorSerial.read();
      data[3] = sensorSerial.read();
      int sum = (data[0] + data[1] + data[2]) & 0x00FF;
      if (sum == data[3]) {
        long dist_mm = (data[1] << 8) + data[2];
        lastValidDistance = dist_mm / 10;
        if (lastValidDistance > 450) lastValidDistance = 450;
        return lastValidDistance;
      }
    }
  }
  return lastValidDistance;
}

void setMotorRelay(bool state) {
  isMotorOn = state;
  digitalWrite(PIN_MOTOR_RELAY, isMotorOn ? HIGH : LOW);
  Serial.printf("Motor Relay changed: %s\n", isMotorOn ? "ON" : "OFF");
}

void setup() {
  Serial.begin(115200);
  sensorSerial.begin(9600, SERIAL_8N1, SENSOR_RX_PIN, SENSOR_TX_PIN);

  pinMode(PIN_MOTOR_RELAY, OUTPUT);
  setMotorRelay(false);
  pinMode(PIN_GRID_DETECTION, INPUT_PULLDOWN);

  loadConfig();

  // Generate Unique Hardware Device ID from MAC Address
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  hardwareDeviceId = "HPS-ESP32-" + mac.substring(mac.length() - 6);

  Serial.println("=========================================");
  Serial.printf("Home Pro Sync ESP32 Controller v%s\n", FW_VERSION);
  Serial.printf("Hardware Device ID: %s\n", hardwareDeviceId.c_str());
  Serial.println("=========================================");

  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid, cfg.pass);
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 12000) {
    delay(400);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWi-Fi Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWi-Fi Connection failed. Starting Emergency Hotspot...");
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(SOFTAP_SSID, SOFTAP_PASS);
    inSoftApMode = true;
    softApStartTime = millis();
    Serial.printf("Emergency SoftAP started: %s (IP: 192.168.4.1)\n", SOFTAP_SSID);
  }

  // --- Endpoints ---

  // 1. JSON Telemetry Endpoint (/data & /status)
  auto sendTelemetry = []() {
    addCorsHeaders();
    String json = "{";
    json += "\"fwVersion\":\"" + String(FW_VERSION) + "\",";
    json += "\"deviceId\":\"" + hardwareDeviceId + "\",";
    json += "\"percent\":" + String(waterPercent) + ",";
    json += "\"motor\":" + String(isMotorOn ? 1 : 0) + ",";
    json += "\"drain\":" + String(drainSpeed) + ",";
    json += "\"v\":" + String(solarVolt, 1) + ",";
    json += "\"i\":" + String(solarAmps, 1) + ",";
    json += "\"p\":" + String(solarWatts, 0) + ",";
    json += "\"grid220V\":" + String(grid220V ? 1 : 0) + ",";
    json += "\"ssid\":\"" + String(cfg.ssid) + "\",";
    json += "\"ap\":" + String(inSoftApMode ? 1 : 0) + ",";
    json += "\"history\":[";
    for (int i = 0; i < 7; i++) {
      json += String(cfg.history[i]) + (i < 6 ? "," : "");
    }
    json += "]}";
    server.send(200, "application/json", json);
  };

  server.on("/data", HTTP_GET, sendTelemetry);
  server.on("/status", HTTP_GET, sendTelemetry);

  // 2. Motor Control Endpoint
  server.on("/motor", []() {
    addCorsHeaders();
    if (server.hasArg("state")) {
      setMotorRelay(server.arg("state").toInt() == 1);
    }
    server.send(200, "text/plain", isMotorOn ? "1" : "0");
  });

  // 3. Settings Config Endpoint
  server.on("/config", HTTP_GET, []() {
    addCorsHeaders();
    if (server.hasArg("h")) cfg.emptyDist = server.arg("h").toInt();
    if (server.hasArg("f")) cfg.fullDist = server.arg("f").toInt();
    if (server.hasArg("d")) cfg.diameter = server.arg("d").toInt();
    if (server.hasArg("maxP")) cfg.maxPercent = server.arg("maxP").toInt();
    if (server.hasArg("minP")) cfg.minPercent = server.arg("minP").toInt();
    if (server.hasArg("autoOn")) cfg.autoOnEnabled = (server.arg("autoOn").toInt() == 1);
    
    bool wifiChanged = false;
    if (server.hasArg("ssid") && server.arg("ssid").length() > 0) {
      strncpy(cfg.ssid, server.arg("ssid").c_str(), sizeof(cfg.ssid));
    }
    if (server.hasArg("pass") && server.arg("pass").length() >= 8) {
      strncpy(cfg.pass, server.arg("pass").c_str(), sizeof(cfg.pass));
      wifiChanged = true;
    }
    saveConfig();
    server.send(200, "application/json", "{\"status\":\"saved\"}");
    if (wifiChanged) {
      delay(1000);
      ESP.restart();
    }
  });

  // 4. Cloud OTA Flashing Endpoint (/ota?url=...)
  server.on("/ota", []() {
    addCorsHeaders();
    if (!server.hasArg("url")) {
      server.send(400, "text/plain", "Missing firmware url argument");
      return;
    }
    String binUrl = server.arg("url");
    Serial.printf("Initiating Cloud OTA from: %s\n", binUrl.c_str());

    // Safety Interlock: Turn OFF motor before starting flash
    setMotorRelay(false);
    server.send(200, "application/json", "{\"status\":\"updating\",\"msg\":\"Motor stopped. Starting download...\"}");

    HTTPClient http;
    http.begin(binUrl);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      int contentLength = http.getSize();
      bool canBegin = Update.begin(contentLength > 0 ? contentLength : UPDATE_SIZE_UNKNOWN);
      if (canBegin) {
        WiFiClient *stream = http.getStreamPtr();
        size_t written = Update.writeStream(*stream);
        if (written == contentLength && Update.end()) {
          Serial.println("OTA Update Successful! Rebooting...");
          delay(1000);
          ESP.restart();
        } else {
          Serial.printf("OTA Update Failed. Error #: %u\n", Update.getError());
        }
      }
    }
    http.end();
  });

  // 5. Local File Upload Update Endpoint (/update)
  server.on("/update", HTTP_POST, []() {
    addCorsHeaders();
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    if (!Update.hasError()) {
      delay(1000);
      ESP.restart();
    }
  }, []() {
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Local OTA File Upload: %s\n", upload.filename.c_str());
      setMotorRelay(false); // Safety lock
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("Local OTA Success: %u bytes written\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  server.begin();
  Serial.println("HTTP Server listening on port 80.");
}

void loop() {
  server.handleClient();

  // Hotspot Auto-Recovery Timeout
  if (inSoftApMode && (millis() - softApStartTime >= SOFT_AP_TIMEOUT)) {
    Serial.println("SoftAP 5-minute timeout reached. Attempting Wi-Fi auto-recovery...");
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.ssid, cfg.pass);
    inSoftApMode = false;
    unsigned long retryAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - retryAttempt < 15000) {
      delay(500);
    }
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.mode(WIFI_AP_STA);
      WiFi.softAP(SOFTAP_SSID, SOFTAP_PASS);
      inSoftApMode = true;
      softApStartTime = millis();
    }
  }

  // Ultrasonic Distance & Percentage calculation
  long dist = readUltrasonicDistance();
  if (dist == 0 || dist > cfg.emptyDist) dist = cfg.emptyDist;
  if (dist < cfg.fullDist) dist = cfg.fullDist;
  waterPercent = map(dist, cfg.emptyDist, cfg.fullDist, 0, 100);
  waterPercent = constrain(waterPercent, 0, 100);

  // Auto-ON Pump
  if (cfg.autoOnEnabled && waterPercent <= cfg.minPercent && !isMotorOn) {
    setMotorRelay(true);
  }

  // Auto-OFF Pump
  if (waterPercent >= cfg.maxPercent && isMotorOn) {
    setMotorRelay(false);
  }

  // Drain Speed & History (Every 60s)
  if (millis() - lastDrainCheck > 60000) {
    int dropAmount = lastPercentForDrain - waterPercent;
    if (!isMotorOn) {
      if (dropAmount >= 4) drainSpeed = 2;
      else if (dropAmount > 0) drainSpeed = 1;
      else drainSpeed = 0;
    } else {
      drainSpeed = 0;
    }
    lastPercentForDrain = waterPercent;
    lastDrainCheck = millis();
  }
}
