#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SinricPro.h>
#include <SinricProTemperaturesensor.h>
#include <SinricProSwitch.h>
#include <SoftwareSerial.h>
#include <EEPROM.h>

// ================= API KEYS =================
#define BOT_TOKEN         "YOUR_TELEGRAM_BOT_TOKEN" 
#define CHAT_ID           "YOUR_TELEGRAM_CHAT_ID"
#define APP_KEY           "YOUR_SINRIC_APP_KEY"
#define APP_SECRET        "YOUR_SINRIC_APP_SECRET"
#define TEMP_ID           "YOUR_SINRIC_TEMP_DEVICE_ID"
#define SWITCH_ID         "YOUR_SINRIC_SWITCH_DEVICE_ID"

// ================= PERSISTENT SETTINGS STRUCT =================
struct Settings {
  uint32_t magic;
  int emptyDist;
  int fullDist;
  int diameter;
  int maxPercent;
  int minPercent;
  bool autoOnEnabled;
  char ssid[36];
  char pass[36];
  int history[7]; // 7-day daily usage %
};

Settings cfg;
SoftwareSerial mySerial(D1, D2); // A02YYUW Sensor (RX=D1, TX=D2)
ESP8266WebServer server(80);

int waterPercent = 0;
bool isMotorOn = false;
int drainSpeed = 0;
long lastValidDistance = 100;

bool lowNotified = false;
bool fullNotified = false;
unsigned long lastSinricUpdate = 0;
unsigned long lastDrainCheck = 0;
int lastPercentForDrain = 0;

// 5-Minute Emergency Hotspot (SoftAP) & Auto-Recovery Lifecycle
bool inSoftApMode = false;
unsigned long softApStartTime = 0;
const unsigned long SOFT_AP_TIMEOUT = 300000; // 5 minutes = 300,000 ms

void loadSettings() {
  EEPROM.begin(sizeof(Settings));
  EEPROM.get(0, cfg);
  if (cfg.magic != 0xA02BEEF) {
    // Default initial values
    cfg.magic = 0xA02BEEF;
    cfg.emptyDist = 100;
    cfg.fullDist = 5;
    cfg.diameter = 110;
    cfg.maxPercent = 95;
    cfg.minPercent = 20;
    cfg.autoOnEnabled = false;
    strncpy(cfg.ssid, "M Audible Studio", sizeof(cfg.ssid));
    strncpy(cfg.pass, "Fkhan@17811780", sizeof(cfg.pass));
    for (int i = 0; i < 7; i++) cfg.history[i] = 40;
    EEPROM.put(0, cfg);
    EEPROM.commit();
  }
}

void saveSettingsToEEPROM() {
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "*");
}

long getAccurateDistance() {
  unsigned char data[4] = {0};
  while (mySerial.available() >= 4) {
    if (mySerial.read() == 0xFF) {
      data[0] = 0xFF;
      data[1] = mySerial.read();
      data[2] = mySerial.read();
      data[3] = mySerial.read();
      int sum = (data[0] + data[1] + data[2]) & 0x00FF;
      if (sum == data[3]) {
        long distance_mm = (data[1] << 8) + data[2];
        lastValidDistance = distance_mm / 10;
        if (lastValidDistance > 450) lastValidDistance = 450;
        return lastValidDistance;
      }
    }
  }
  return lastValidDistance;
}

void sendTelegramAlert(String message) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/sendMessage?chat_id=" + String(CHAT_ID) + "&text=" + message;
  if (https.begin(client, url)) { https.GET(); https.end(); }
}

bool onPowerState(const String &deviceId, bool &state) {
  isMotorOn = state;
  return true;
}

void setup() {
  Serial.begin(115200);
  mySerial.begin(9600);
  loadSettings();

  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid, cfg.pass);
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
    delay(500);
  }
  // Fallback 5-Minute Emergency SoftAP if router connection fails
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("SmartWaterTank_Setup", "12345678");
    inSoftApMode = true;
    softApStartTime = millis();
    Serial.println("Started Emergency Hotspot (SmartWaterTank_Setup, 192.168.4.1)");
  }

  // JSON Data Endpoint for the APK (includes ssid & ap state)
  server.on("/data", HTTP_GET, []() {
    addCorsHeaders();
    String json = "{\"percent\":" + String(waterPercent) +
                  ",\"motor\":" + String(isMotorOn ? 1 : 0) +
                  ",\"drain\":" + String(drainSpeed) +
                  ",\"ssid\":\"" + String(cfg.ssid) + "\"" +
                  ",\"ap\":" + String(inSoftApMode ? 1 : 0) +
                  ",\"history\":[" +
                  String(cfg.history[0]) + "," + String(cfg.history[1]) + "," +
                  String(cfg.history[2]) + "," + String(cfg.history[3]) + "," +
                  String(cfg.history[4]) + "," + String(cfg.history[5]) + "," +
                  String(cfg.history[6]) + "]}";
    server.send(200, "application/json", json);
  });

  // Manual Motor Control Endpoint
  server.on("/motor", HTTP_GET, []() {
    addCorsHeaders();
    if (server.hasArg("state")) {
      isMotorOn = (server.arg("state").toInt() == 1);
      SinricProSwitch &mySwitch = SinricPro[SWITCH_ID];
      mySwitch.sendPowerStateEvent(isMotorOn);
    }
    server.send(200, "text/plain", "OK");
  });

  // App Settings Sync Endpoint
  server.on("/config", HTTP_GET, []() {
    addCorsHeaders();
    if (server.hasArg("h")) cfg.emptyDist = server.arg("h").toInt();
    if (server.hasArg("d")) cfg.diameter = server.arg("d").toInt();
    if (server.hasArg("f")) cfg.fullDist = server.arg("f").toInt();
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

    saveSettingsToEEPROM();
    server.send(200, "application/json", "{\"status\":\"saved\"}");

    if (wifiChanged) {
      delay(1000);
      ESP.restart();
    }
  });

  server.begin();

  SinricProSwitch &mySwitch = SinricPro[SWITCH_ID];
  mySwitch.onPowerState(onPowerState);
  SinricPro.begin(APP_KEY, APP_SECRET);
}

void loop() {
  SinricPro.handle();
  server.handleClient();

  // Auto-recovery: If in SoftAP mode and 5 minutes elapsed without provisioning, attempt reconnect
  if (inSoftApMode && (millis() - softApStartTime >= SOFT_AP_TIMEOUT)) {
    Serial.println("SoftAP 5-min timeout reached. Attempting home Wi-Fi auto-recovery...");
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.ssid, cfg.pass);
    inSoftApMode = false;
    unsigned long retryAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - retryAttempt < 15000) {
      delay(500);
    }
    if (WiFi.status() != WL_CONNECTED) {
      // Re-engage SoftAP for another 5-minute window if router still unreachable
      WiFi.mode(WIFI_AP_STA);
      WiFi.softAP("SmartWaterTank_Setup", "12345678");
      inSoftApMode = true;
      softApStartTime = millis();
    }
  }

  long distance = getAccurateDistance();
  if (distance == 0 || distance > cfg.emptyDist) distance = cfg.emptyDist;
  if (distance < cfg.fullDist) distance = cfg.fullDist;

  waterPercent = map(distance, cfg.emptyDist, cfg.fullDist, 0, 100);
  waterPercent = constrain(waterPercent, 0, 100);

  // Dynamic Auto-ON Logic (Controlled by App Settings Toggle)
  if (cfg.autoOnEnabled && waterPercent <= cfg.minPercent && !isMotorOn) {
    isMotorOn = true;
    SinricProSwitch &mySwitch = SinricPro[SWITCH_ID];
    mySwitch.sendPowerStateEvent(true);
  }

  // Dynamic Auto-OFF Logic (Uses User's Custom Full Percentage)
  if (waterPercent >= cfg.maxPercent && isMotorOn) {
    isMotorOn = false;
    SinricProSwitch &mySwitch = SinricPro[SWITCH_ID];
    mySwitch.sendPowerStateEvent(false);
  }

  // Drain Speed & Daily Usage History Tracker (Every 60 Seconds)
  if (millis() - lastDrainCheck > 60000) {
    int dropAmount = lastPercentForDrain - waterPercent;
    if (!isMotorOn) {
      if (dropAmount >= 4) drainSpeed = 2;
      else if (dropAmount > 0) drainSpeed = 1;
      else drainSpeed = 0;

      if (dropAmount > 0) {
        cfg.history[6] += dropAmount; // Accumulate today's usage %
      }
    } else {
      drainSpeed = 0;
    }
    lastPercentForDrain = waterPercent;
    lastDrainCheck = millis();
  }

  // Telegram Alerts using dynamic user thresholds
  if (waterPercent <= cfg.minPercent && !lowNotified) {
    sendTelegramAlert(cfg.autoOnEnabled
      ? "%F0%9F%9A%A8 Tank below " + String(cfg.minPercent) + "%. Auto-ON started Motor!"
      : "%F0%9F%9A%A8 Tank below " + String(cfg.minPercent) + "%. Please start Motor from App!");
    lowNotified = true; fullNotified = false;
  } else if (waterPercent >= cfg.maxPercent && !fullNotified) {
    sendTelegramAlert("%F0%9F%92%A7 Tank reached " + String(cfg.maxPercent) + "%. Motor Auto-Stopped!");
    fullNotified = true; lowNotified = false;
  } else if (waterPercent > (cfg.minPercent + 10) && waterPercent < (cfg.maxPercent - 5)) {
    lowNotified = false; fullNotified = false;
  }

  if (millis() - lastSinricUpdate > 60000) {
    SinricProTemperaturesensor &mySensor = SinricPro[TEMP_ID];
    mySensor.sendTemperatureEvent((float)waterPercent);
    lastSinricUpdate = millis();
  }
}
