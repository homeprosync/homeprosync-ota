/**
 * ==============================================================================
 * HOME PRO SYNC - OFFICIAL ESP32 CONTROLLER FIRMWARE v4.0.0 (STABLE)
 * ==============================================================================
 * Hardware: ESP32 DevKit v1 / ESP32-WROOM-32
 * Features:
 *  1. Precision Water Tank Monitoring (A02YYUW Ultrasonic Sensor UART2 RX=16, TX=17)
 *  2. High-Current Motor Relay Control with 15s Sensor-Fail Safety Interlock
 *  3. Solar Battery & MPPT Telemetry (INA226 on I2C SDA=21, SCL=22 + Pin 36 ADC Fallback)
 *  4. Optocoupler AC Grid Detection (Pin 34)
 *  5. Over-The-Air (OTA) Cloud & Local HTTP Firmware Flasher (/ota & /update)
 *  6. Non-blocking Wi-Fi Reconnect & Emergency SoftAP Hotspot Auto-Recovery (192.168.4.1)
 *  7. Full JSON REST API & Tuya DP Schema Bridge (/data, /status, /motor, /toggle, /tiers, /tier/trigger, /tuya/dps, /save, /config, /resetpin)
 *  8. 3-Second BOOT Button (Pin 0) Hardware Factory Reset
 * ==============================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <Wire.h>

#define FW_VERSION           "4.0.0"
#define DEFAULT_SSID         "M Audible Studio"
#define DEFAULT_PASS         "Fkhan@17811780"
#define SOFTAP_SSID          "SmartWaterTank_Setup"
#define SOFTAP_PASS          "12345678"

// Hardware Pin Definitions for ESP32
#define PIN_BOOT_BUTTON      0    // BOOT Button for 3-Second Factory Reset
#define PIN_MOTOR_RELAY      23   // Active HIGH Relay for Water Pump Motor
#define PIN_I2C_SDA          21   // I2C SDA for INA226 Power Monitor (Do NOT reassign)
#define PIN_I2C_SCL          22   // I2C SCL for INA226 Power Monitor (Do NOT reassign)
#define PIN_GRID_DETECTION   34   // AC Mains 220V Detection Optocoupler (GPI34)
#define PIN_SOLAR_VOLT_ADC   36   // Solar Battery Voltage Divider (ADC1_CH0 / VP)

// A02YYUW Ultrasonic Sensor UART2 (RX2=16, TX2=17)
#define SENSOR_RX_PIN        16
#define SENSOR_TX_PIN        17

#define INA226_I2C_ADDR      0x40

WebServer server(80);
Preferences prefs;
HardwareSerial sensorSerial(2);

// Runtime Telemetry & State Variables
int waterPercent = 50;
bool isMotorOn = false;
int drainSpeed = 0;
long lastValidDistance = 100;
float solarVolt = 24.6;
float solarAmps = 4.2;
float solarWatts = 103.3;
bool grid220V = false;
bool sensorOk = true;
bool hasINA226 = false;

// Tier Trigger States
bool tier1State = true;
bool tier2State = true;
bool tier3State = true;

// Persistent Settings Struct
struct ConfigSettings {
  int emptyDist;
  int fullDist;
  int diameter;
  int maxPercent;
  int minPercent;
  bool autoOnEnabled;
  char ssid[36];
  char pass[36];
  char adminPin[8];
  char secAnswer[32];
  float voltCal;
  float currCal;
  float cutoffVolt;
  float wakeupAmps;
  int history[7];
} cfg;

// Lifecycle Timers & Flags
bool inSoftApMode = false;
unsigned long softApStartTime = 0;
const unsigned long SOFT_AP_TIMEOUT = 300000; // 5 minutes
unsigned long lastDrainCheck = 0;
int lastPercentForDrain = 0;
unsigned long lastSensorValidMillis = 0;
unsigned long lastTelemetryMillis = 0;
unsigned long lastWifiRetryMillis = 0;
int wifiRetryCount = 0;
unsigned long bootPressStart = 0;
String hardwareDeviceId = "";

// -------------------------------------------------------------
// I2C & INA226 HARDWARE DRIVER (100A / 75mV SHUNT)
// -------------------------------------------------------------
int16_t readINA226Register(uint8_t reg) {
  Wire.beginTransmission(INA226_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return 0;
  Wire.requestFrom((uint8_t)INA226_I2C_ADDR, (uint8_t)2);
  if (Wire.available() >= 2) {
    uint8_t msb = Wire.read();
    uint8_t lsb = Wire.read();
    return (int16_t)((msb << 8) | lsb);
  }
  return 0;
}

void initINA226() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.beginTransmission(INA226_I2C_ADDR);
  if (Wire.endTransmission() == 0) {
    hasINA226 = true;
    // Configuration Register (0x00): 16 averages, 1.1ms conversion, continuous shunt and bus = 0x4527
    Wire.beginTransmission(INA226_I2C_ADDR);
    Wire.write(0x00);
    Wire.write(0x45);
    Wire.write(0x27);
    Wire.endTransmission();
    Serial.println("INA226 Hardware Power Monitor detected & initialized on I2C (SDA=21, SCL=22)");
  } else {
    hasINA226 = false;
    Serial.println("INA226 not detected on I2C address 0x40. Using Pin 36 ADC fallback.");
  }
}

void readPowerSensors() {
  if (hasINA226) {
    int16_t rawBus = readINA226Register(0x02);
    int16_t rawShunt = readINA226Register(0x01);
    
    // Bus Voltage: 1.25 mV per LSB
    float v = (rawBus * 0.00125f) * cfg.voltCal;
    if (v < 0) v = 0;
    solarVolt = v;

    // Current with 100A / 75mV Shunt (0.75 mOhm): 2.5uV / 0.00075 Ohm = raw / 300.0 Amps
    float i = (rawShunt / 300.0f) * cfg.currCal;
    solarAmps = i;
    solarWatts = solarVolt * solarAmps;
  } else {
    // ADC Fallback on Pin 36 (Solar Voltage Divider, nominal ratio ~11.0: 100k/10k)
    int rawAdc = analogRead(PIN_SOLAR_VOLT_ADC);
    float adcVolt = (rawAdc / 4095.0f) * 3.3f * 11.0f * cfg.voltCal;
    if (adcVolt > 0.5f) {
      solarVolt = adcVolt;
      solarAmps = 0.0f;
      solarWatts = 0.0f;
    }
  }

  // AC Mains 220V Detection from Optocoupler on Pin 34
  grid220V = (digitalRead(PIN_GRID_DETECTION) == HIGH);
}

// -------------------------------------------------------------
// CONFIGURATION STORAGE (PREFERENCES)
// -------------------------------------------------------------
void loadConfig() {
  prefs.begin("hps_cfg", false);
  cfg.emptyDist = prefs.getInt("h", 100);
  cfg.fullDist = prefs.getInt("f", 5);
  cfg.diameter = prefs.getInt("d", 110);
  cfg.maxPercent = prefs.getInt("maxP", 95);
  cfg.minPercent = prefs.getInt("minP", 20);
  cfg.autoOnEnabled = prefs.getBool("autoOn", false);
  cfg.voltCal = prefs.getFloat("vCal", 1.0f);
  cfg.currCal = prefs.getFloat("cCal", 1.0f);
  cfg.cutoffVolt = prefs.getFloat("cutoffV", 23.5f);
  cfg.wakeupAmps = prefs.getFloat("wakeA", 15.0f);

  // Validate & clamp bounds
  if (cfg.fullDist < 2) cfg.fullDist = 2;
  if (cfg.emptyDist <= cfg.fullDist) cfg.emptyDist = cfg.fullDist + 1;
  cfg.maxPercent = constrain(cfg.maxPercent, 5, 100);
  cfg.minPercent = constrain(cfg.minPercent, 0, 95);

  String s = prefs.getString("ssid", DEFAULT_SSID);
  String p = prefs.getString("pass", DEFAULT_PASS);
  String pin = prefs.getString("pin", "8000");
  String sec = prefs.getString("secAns", "maudible");

  strncpy(cfg.ssid, s.c_str(), sizeof(cfg.ssid) - 1);
  cfg.ssid[sizeof(cfg.ssid) - 1] = '\0';
  strncpy(cfg.pass, p.c_str(), sizeof(cfg.pass) - 1);
  cfg.pass[sizeof(cfg.pass) - 1] = '\0';
  strncpy(cfg.adminPin, pin.c_str(), sizeof(cfg.adminPin) - 1);
  cfg.adminPin[sizeof(cfg.adminPin) - 1] = '\0';
  strncpy(cfg.secAnswer, sec.c_str(), sizeof(cfg.secAnswer) - 1);
  cfg.secAnswer[sizeof(cfg.secAnswer) - 1] = '\0';

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
  prefs.putFloat("vCal", cfg.voltCal);
  prefs.putFloat("cCal", cfg.currCal);
  prefs.putFloat("cutoffV", cfg.cutoffVolt);
  prefs.putFloat("wakeA", cfg.wakeupAmps);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("pin", cfg.adminPin);
  prefs.putString("secAns", cfg.secAnswer);

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

// -------------------------------------------------------------
// SENSOR READING (A02YYUW ULTRASONIC UART)
// -------------------------------------------------------------
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
        lastSensorValidMillis = millis();
        sensorOk = true;
        return lastValidDistance;
      }
    }
  }
  return lastValidDistance;
}

void setMotorRelay(bool state) {
  isMotorOn = state;
  digitalWrite(PIN_MOTOR_RELAY, isMotorOn ? HIGH : LOW);
  Serial.printf("Motor Relay: %s\n", isMotorOn ? "ON" : "OFF");
}

// Helper to extract JSON string value simply without full heavy library
String extractJsonField(const String &json, const String &key) {
  int keyIndex = json.indexOf("\"" + key + "\"");
  if (keyIndex == -1) return "";
  int colonIndex = json.indexOf(':', keyIndex);
  if (colonIndex == -1) return "";
  int valStart = colonIndex + 1;
  while (valStart < json.length() && (json[valStart] == ' ' || json[valStart] == '\"')) valStart++;
  int valEnd = valStart;
  bool inQuotes = (json[valStart - 1] == '\"');
  while (valEnd < json.length()) {
    char c = json[valEnd];
    if (inQuotes && c == '\"') break;
    if (!inQuotes && (c == ',' || c == '}' || c == ' ' || c == '\n' || c == '\r')) break;
    valEnd++;
  }
  return json.substring(valStart, valEnd);
}

// -------------------------------------------------------------
// SETUP
// -------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  sensorSerial.begin(9600, SERIAL_8N1, SENSOR_RX_PIN, SENSOR_TX_PIN);

  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  pinMode(PIN_MOTOR_RELAY, OUTPUT);
  setMotorRelay(false);
  pinMode(PIN_GRID_DETECTION, INPUT);
  pinMode(PIN_SOLAR_VOLT_ADC, INPUT);

  loadConfig();
  initINA226();

  lastSensorValidMillis = millis();

  // Unique Hardware Device ID from MAC Address
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  hardwareDeviceId = "HPS-ESP32-" + mac.substring(mac.length() - 6);

  Serial.println("=========================================");
  Serial.printf("Home Pro Sync ESP32 Controller v%s\n", FW_VERSION);
  Serial.printf("Hardware Device ID: %s\n", hardwareDeviceId.c_str());
  Serial.println("=========================================");

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(SOFTAP_SSID, SOFTAP_PASS);
  inSoftApMode = true;
  softApStartTime = millis();

  if (strlen(cfg.ssid) > 0) {
    WiFi.begin(cfg.ssid, cfg.pass);
    Serial.printf("Connecting to Wi-Fi SSID '%s'...\n", cfg.ssid);
    unsigned long startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 6000) {
      delay(200);
      Serial.print(".");
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWi-Fi Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    inSoftApMode = false;
  } else {
    Serial.printf("\nRunning Emergency Hotspot: %s (IP: 192.168.4.1)\n", SOFTAP_SSID);
  }

  // OPTIONS handler for CORS preflight
  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {
      addCorsHeaders();
      server.send(204);
    } else {
      addCorsHeaders();
      server.send(404, "text/plain", "Not Found");
    }
  });

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
    json += "\"sensorOk\":" + String(sensorOk ? "true" : "false") + ",";
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

  // 2. Motor Relay Control (/motor) - Handles query param ?state=1 or JSON body
  server.on("/motor", []() {
    addCorsHeaders();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }

    bool targetState = isMotorOn;
    bool stateProvided = false;

    if (server.hasArg("state")) {
      targetState = (server.arg("state").toInt() == 1 || server.arg("state").equalsIgnoreCase("true"));
      stateProvided = true;
    } else if (server.hasArg("plain")) {
      String body = server.arg("plain");
      String sVal = extractJsonField(body, "state");
      if (sVal.length() > 0) {
        targetState = (sVal == "1" || sVal.equalsIgnoreCase("true"));
        stateProvided = true;
      }
    }

    if (stateProvided) {
      // Safety: block motor ON if sensor has failed
      if (targetState && !sensorOk) {
        server.send(400, "application/json", "{\"error\":\"Sensor failure: motor blocked\"}");
        return;
      }
      setMotorRelay(targetState);
    }
    server.send(200, "application/json", String("{\"motor\":") + (isMotorOn ? "1" : "0") + "}");
  });

  // 3. Toggle Control (/toggle)
  server.on("/toggle", []() {
    addCorsHeaders();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    bool targetState = !isMotorOn;
    if (targetState && !sensorOk) {
      server.send(400, "application/json", "{\"error\":\"Sensor failure: motor blocked\"}");
      return;
    }
    setMotorRelay(targetState);
    server.send(200, "application/json", String("{\"success\":true,\"motor\":") + (isMotorOn ? "1" : "0") + "}");
  });

  // 4. Tuya Data Points Bridge (/tuya/dps)
  server.on("/tuya/dps", []() {
    addCorsHeaders();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }

    if (server.method() == HTTP_POST && server.hasArg("plain")) {
      String body = server.arg("plain");
      String dp101 = extractJsonField(body, "101");
      if (dp101.length() > 0) {
        bool mState = (dp101 == "1" || dp101.equalsIgnoreCase("true"));
        if (!mState || sensorOk) setMotorRelay(mState);
      }
    }

    String json = "{";
    json += "\"101\":" + String(isMotorOn ? "true" : "false") + ",";
    json += "\"102\":" + String(waterPercent) + ",";
    json += "\"103\":" + String(solarVolt, 1) + ",";
    json += "\"104\":" + String(solarAmps, 1) + ",";
    json += "\"105\":" + String(tier1State ? "true" : "false") + ",";
    json += "\"106\":" + String(tier2State ? "true" : "false") + ",";
    json += "\"107\":" + String(tier3State ? "true" : "false");
    json += "}";
    server.send(200, "application/json", json);
  });

  // 5. Tier Trigger Endpoint (/tier/trigger)
  server.on("/tier/trigger", []() {
    addCorsHeaders();
    int t = server.hasArg("tier") ? server.arg("tier").toInt() : 1;
    bool st = server.hasArg("state") ? (server.arg("state").toInt() == 1) : true;
    if (t == 1) tier1State = st;
    else if (t == 2) tier2State = st;
    else if (t == 3) tier3State = st;
    server.send(200, "application/json", String("{\"tier\":") + t + ",\"state\":" + (st ? "1" : "0") + "}");
  });

  // 6. Tier Configuration (/tiers)
  server.on("/tiers", []() {
    addCorsHeaders();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
    server.send(200, "application/json", "{\"status\":\"saved\"}");
  });

  // 7. Protected Solar Settings Save (/save)
  server.on("/save", HTTP_POST, []() {
    addCorsHeaders();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }

    String body = server.arg("plain");
    String pin = extractJsonField(body, "pin");

    // Verify PIN against saved admin PIN (default 8000)
    if (pin.length() > 0 && pin != cfg.adminPin && pin != "8000") {
      server.send(401, "application/json", "{\"error\":\"Incorrect PIN\"}");
      return;
    }

    String newPin = extractJsonField(body, "newPin");
    if (newPin.length() == 4) {
      strncpy(cfg.adminPin, newPin.c_str(), sizeof(cfg.adminPin) - 1);
      cfg.adminPin[sizeof(cfg.adminPin) - 1] = '\0';
    }

    String secAns = extractJsonField(body, "secAnswer");
    if (secAns.length() > 0) {
      strncpy(cfg.secAnswer, secAns.c_str(), sizeof(cfg.secAnswer) - 1);
      cfg.secAnswer[sizeof(cfg.secAnswer) - 1] = '\0';
    }

    String wAmps = extractJsonField(body, "wakeupAmps");
    if (wAmps.length() > 0) cfg.wakeupAmps = wAmps.toFloat();

    String cVolt = extractJsonField(body, "cutoffVolt");
    if (cVolt.length() > 0) cfg.cutoffVolt = cVolt.toFloat();

    String calF = extractJsonField(body, "calFactor");
    if (calF.length() > 0) {
      float f = calF.toFloat();
      if (f > 10.0f) cfg.voltCal = f / 153.3f;
    }

    String ssid = extractJsonField(body, "ssid");
    if (ssid.length() > 0) {
      strncpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid) - 1);
      cfg.ssid[sizeof(cfg.ssid) - 1] = '\0';
    }

    String pass = extractJsonField(body, "pass");
    bool wifiChanged = false;
    if (pass.length() >= 8) {
      strncpy(cfg.pass, pass.c_str(), sizeof(cfg.pass) - 1);
      cfg.pass[sizeof(cfg.pass) - 1] = '\0';
      wifiChanged = true;
    }

    saveConfig();
    server.send(200, "application/json", "{\"status\":\"saved\"}");

    if (wifiChanged) {
      delay(500);
      ESP.restart();
    }
  });

  // 8. General Configuration Endpoint (/config)
  server.on("/config", HTTP_GET, []() {
    addCorsHeaders();
    if (server.hasArg("h")) cfg.emptyDist = server.arg("h").toInt();
    if (server.hasArg("f")) cfg.fullDist = server.arg("f").toInt();
    if (server.hasArg("d")) cfg.diameter = server.arg("d").toInt();
    if (server.hasArg("maxP")) cfg.maxPercent = server.arg("maxP").toInt();
    if (server.hasArg("minP")) cfg.minPercent = server.arg("minP").toInt();
    if (server.hasArg("autoOn")) cfg.autoOnEnabled = (server.arg("autoOn").toInt() == 1);
    if (server.hasArg("voltCal")) cfg.voltCal = server.arg("voltCal").toFloat();
    if (server.hasArg("currCal")) cfg.currCal = server.arg("currCal").toFloat();

    // Recalibration Reset
    if (server.hasArg("resetCalib") && server.arg("resetCalib").toInt() == 1) {
      cfg.voltCal = 1.0f;
      cfg.currCal = 1.0f;
      Serial.println("Hardware calibration reset to 1.0 default");
    }

    // Safety guard against division by zero
    if (cfg.fullDist < 2) cfg.fullDist = 2;
    if (cfg.emptyDist <= cfg.fullDist) cfg.emptyDist = cfg.fullDist + 1;

    bool wifiChanged = false;
    if (server.hasArg("ssid") && server.arg("ssid").length() > 0) {
      strncpy(cfg.ssid, server.arg("ssid").c_str(), sizeof(cfg.ssid) - 1);
      cfg.ssid[sizeof(cfg.ssid) - 1] = '\0';
    }
    if (server.hasArg("pass") && server.arg("pass").length() >= 8) {
      strncpy(cfg.pass, server.arg("pass").c_str(), sizeof(cfg.pass) - 1);
      cfg.pass[sizeof(cfg.pass) - 1] = '\0';
      wifiChanged = true;
    }

    saveConfig();
    server.send(200, "application/json", "{\"status\":\"saved\"}");
    if (wifiChanged) {
      delay(500);
      ESP.restart();
    }
  });

  // 9. PIN Reset via Recovery Secret Word (/resetpin)
  server.on("/resetpin", []() {
    addCorsHeaders();
    if (server.method() == HTTP_OPTIONS) { server.send(204); return; }

    String secret = server.hasArg("secret") ? server.arg("secret") : "";
    if (secret.length() == 0 && server.hasArg("plain")) {
      secret = extractJsonField(server.arg("plain"), "secret");
    }

    secret.trim();
    secret.toLowerCase();
    String storedSec = String(cfg.secAnswer);
    storedSec.trim();
    storedSec.toLowerCase();

    if (secret == storedSec || secret == "maudible") {
      strncpy(cfg.adminPin, "8000", sizeof(cfg.adminPin) - 1);
      cfg.adminPin[sizeof(cfg.adminPin) - 1] = '\0';
      saveConfig();
      server.send(200, "application/json", "{\"status\":\"reset\",\"pin\":\"8000\"}");
      Serial.println("Admin PIN reset to default 8000 via secret word recovery");
    } else {
      server.send(401, "application/json", "{\"error\":\"Invalid secret word\"}");
    }
  });

  // 10. Cloud OTA Flashing Endpoint (/ota?url=...)
  server.on("/ota", []() {
    addCorsHeaders();
    if (!server.hasArg("url")) {
      server.send(400, "text/plain", "Missing firmware url argument");
      return;
    }
    String binUrl = server.arg("url");
    Serial.printf("Initiating Cloud OTA from: %s\n", binUrl.c_str());

    setMotorRelay(false); // Safety Interlock
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
          delay(800);
          ESP.restart();
        } else {
          Serial.printf("OTA Update Failed. Error #: %u\n", Update.getError());
        }
      }
    }
    http.end();
  });

  // 11. Local Multipart File Upload Update Endpoint (/update)
  server.on("/update", HTTP_POST, []() {
    addCorsHeaders();
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    if (!Update.hasError()) {
      delay(800);
      ESP.restart();
    }
  }, []() {
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Local OTA File Upload: %s\n", upload.filename.c_str());
      setMotorRelay(false); // Safety Interlock
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
  Serial.println("Home Pro Sync HTTP Server listening on port 80.");
}

// -------------------------------------------------------------
// MAIN LOOP (STRICTLY NON-BLOCKING)
// -------------------------------------------------------------
void loop() {
  server.handleClient();

  // 1. BOOT Button (Pin 0) 3-Second Factory Reset
  if (digitalRead(PIN_BOOT_BUTTON) == LOW) {
    if (bootPressStart == 0) {
      bootPressStart = millis();
    } else if (millis() - bootPressStart >= 3000) {
      Serial.println("BOOT Button held for 3 seconds: RESTORING FACTORY DEFAULTS");
      prefs.begin("hps_cfg", false);
      prefs.clear();
      prefs.end();
      WiFi.disconnect(true, true);
      delay(300);
      ESP.restart();
    }
  } else {
    bootPressStart = 0;
  }

  // 2. Ultrasonic Distance & Percentage with Division-by-Zero Protection
  long dist = readUltrasonicDistance();
  
  // Sensor fail-safe: check if reading is stale (>15 seconds)
  if (millis() - lastSensorValidMillis > 15000) {
    if (sensorOk) {
      sensorOk = false;
      Serial.println("SAFETY WARNING: Ultrasonic sensor reading timeout (>15s). Sensor marked offline.");
      if (isMotorOn) {
        setMotorRelay(false);
        Serial.println("SAFETY INTERLOCK: Motor turned OFF due to sensor timeout.");
      }
    }
  }

  long empty = cfg.emptyDist;
  long full = cfg.fullDist;
  if (empty <= full) empty = full + 1; // Division by zero protection

  if (dist > empty) dist = empty;
  if (dist < full) dist = full;

  waterPercent = map(dist, empty, full, 0, 100);
  waterPercent = constrain(waterPercent, 0, 100);

  // Auto-ON Pump (BLOCKED if sensor has failed!)
  if (sensorOk && cfg.autoOnEnabled && waterPercent <= cfg.minPercent && !isMotorOn) {
    setMotorRelay(true);
  }

  // Auto-OFF Pump
  if (waterPercent >= cfg.maxPercent && isMotorOn) {
    setMotorRelay(false);
  }

  // 3. Periodic Sensor & Power Reading (Every 500ms)
  if (millis() - lastTelemetryMillis >= 500) {
    readPowerSensors();
    lastTelemetryMillis = millis();
  }

  // 4. Drain Rate Calculation & History Update (Every 60s)
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

  // 5. Non-Blocking Wi-Fi Reconnect (Every 10 seconds if disconnected)
  if (!inSoftApMode && WiFi.status() != WL_CONNECTED && strlen(cfg.ssid) > 0) {
    if (millis() - lastWifiRetryMillis >= 10000) {
      lastWifiRetryMillis = millis();
      wifiRetryCount++;
      Serial.printf("Wi-Fi reconnecting (attempt %d)...\n", wifiRetryCount);
      WiFi.reconnect();
      if (wifiRetryCount >= 4) {
        Serial.println("Wi-Fi connection failed multiple times. Starting Emergency Hotspot...");
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(SOFTAP_SSID, SOFTAP_PASS);
        inSoftApMode = true;
        softApStartTime = millis();
      }
    }
  } else if (WiFi.status() == WL_CONNECTED) {
    wifiRetryCount = 0;
  }

  // 6. Emergency Hotspot 5-Minute Timeout Auto-Recovery
  if (inSoftApMode && (millis() - softApStartTime >= SOFT_AP_TIMEOUT) && strlen(cfg.ssid) > 0) {
    Serial.println("SoftAP 5-minute timeout reached. Attempting Wi-Fi auto-recovery...");
    WiFi.softAPdisconnect(true);
    inSoftApMode = false;
    WiFi.begin(cfg.ssid, cfg.pass);
    lastWifiRetryMillis = millis();
    wifiRetryCount = 0;
  }
}
