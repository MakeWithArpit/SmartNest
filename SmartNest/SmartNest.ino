#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HardwareSerial.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#define NUM_RELAYS 6
#define FIRMWARE_VERSION "1.0.0"

#define RELAY_1_PIN 26
#define RELAY_2_PIN 27
#define RELAY_3_PIN 14
#define RELAY_4_PIN 23
#define RELAY_5_PIN 25
#define RELAY_6_PIN 33

#define SWITCH_1_PIN 13
#define SWITCH_2_PIN 15
#define SWITCH_3_PIN 19
#define SWITCH_4_PIN 4
#define SWITCH_5_PIN 32
#define SWITCH_6_PIN 18

#define ACS712_PIN            34
#define ACS712_SENSITIVITY    0.066f
#define ACS712_DIVIDER_RATIO  1.5f
#define ACS712_RMS_SAMPLES    500
#define ACS712_READ_INTERVAL_MS 500
#define ACS712_DEADBAND_A     0.3f
#define ACS712_AUTO_CALIBRATE false
#define ACS712_ZERO_POINT_MV  2569.3f
static float acs712ZeroMv = ACS712_ZERO_POINT_MV;

#define RESET_BTN_PIN 0
#define RESET_HOLD_MS 3000

#define UART2_RX_PIN 16
#define UART2_TX_PIN 17
#define UART2_BAUD 115200

#define CLOUD_ENABLED         false
#define CLOUD_URL             "https://your-server.com/api/relay-status"
#define HEARTBEAT_MS          30000
#define HTTP_TIMEOUT_MS       5000

#define MDNS_HOSTNAME "smart-nest"
#define PROV_AP_SSID "SmartNest"
#define NVS_NAMESPACE "smartnest"
#define NVS_SSID_KEY "wifi_ssid"
#define NVS_PASS_KEY "wifi_pass"
#define NVS_PROV_KEY "provisioned"

struct RelayEvent {
  int index;
  bool state;
  unsigned long timestamp;
};

struct UartData {
  float deviceCurrent;
  bool errors[4];
  char errorMsg[64];
  bool hasNewData;
};

struct SystemState {
  bool relayStates[NUM_RELAYS];
  bool dashboardStates[NUM_RELAYS];
  bool masterShutdown;
  bool lockedStates[NUM_RELAYS];
  float currentAmps;
  UartData uartData;
  bool wifiConnected;
  int wifiRSSI;
  char wifiSSID[33];
  unsigned long lastChangeMs;
};

const int RELAY_PINS[NUM_RELAYS] = {
  RELAY_1_PIN, RELAY_2_PIN, RELAY_3_PIN,
  RELAY_4_PIN, RELAY_5_PIN, RELAY_6_PIN
};

const int SWITCH_PINS[NUM_RELAYS] = {
  SWITCH_1_PIN, SWITCH_2_PIN, SWITCH_3_PIN,
  SWITCH_4_PIN, SWITCH_5_PIN, SWITCH_6_PIN
};

const char *const RELAY_NAMES[NUM_RELAYS] = {
  "Light 1", "Light 2", "Light 3",
  "Light 4", "Light 5", "Power Socket"
};

QueueHandle_t relayEventQueue = NULL;
SemaphoreHandle_t stateMutex = NULL;
SystemState sysState;
String wifiPassword = "";
volatile bool isProvisioningMode = false;

static TaskHandle_t hRelaySwitch = NULL;
static TaskHandle_t hCurrentSensor = NULL;
static TaskHandle_t hCloudClient = NULL;
static TaskHandle_t hUartComm = NULL;
static TaskHandle_t hWifiReconnect = NULL;
static TaskHandle_t hResetButton = NULL;

void setRelayState(int index, bool state);
bool getRelayState(int index);
void toggleRelay(int index);
void updateRelayHardware();
void relaySwitchInit();
void currentSensorInit();
float readCurrent();
bool wifiManagerInit();
void wifiManagerLoop();
void resetWiFiCredentials();
void uartCommInit();
void uartSendRelayCommand(int relayIndex, bool state);
void dashboardInit();

void relaySwitchTask(void *pvParameters);
void currentSensorTask(void *pvParameters);
void resetButtonTask(void *pvParameters);
void wifiReconnectTask(void *pvParameters);
void cloudClientTask(void *pvParameters);
void uartCommTask(void *pvParameters);

static bool lastSwitchState[NUM_RELAYS] = { false };

void relaySwitchInit() {
  for (int i = 0; i < NUM_RELAYS; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], LOW);
    pinMode(SWITCH_PINS[i], INPUT_PULLDOWN);
    lastSwitchState[i] = (digitalRead(SWITCH_PINS[i]) == HIGH);
  }

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    for (int i = 0; i < NUM_RELAYS; i++) {
      sysState.relayStates[i] = false;
      sysState.dashboardStates[i] = false;
      sysState.lockedStates[i] = false;
    }
    sysState.masterShutdown = false;
    xSemaphoreGive(stateMutex);
  }
}

void setRelayState(int index, bool state) {
  if (index < 0 || index >= NUM_RELAYS) return;

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    sysState.dashboardStates[index] = state;
    xSemaphoreGive(stateMutex);
  }

  updateRelayHardware();
}

bool getRelayState(int index) {
  if (index < 0 || index >= NUM_RELAYS) return false;

  bool s = false;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    s = sysState.relayStates[index];
    xSemaphoreGive(stateMutex);
  }
  return s;
}

void toggleRelay(int index) {
  bool currentCmd = false;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    currentCmd = sysState.dashboardStates[index];
    xSemaphoreGive(stateMutex);
  }
  setRelayState(index, !currentCmd);
}

void updateRelayHardware() {
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    for (int i = 0; i < NUM_RELAYS; i++) {
      bool D = sysState.dashboardStates[i];
      bool S = (digitalRead(SWITCH_PINS[i]) == HIGH);
      bool R = false;

      if (sysState.masterShutdown || sysState.lockedStates[i]) {
        R = false;
      } else {
        R = D || S;
      }

      if (sysState.relayStates[i] != R) {
        sysState.relayStates[i] = R;
        digitalWrite(RELAY_PINS[i], R ? HIGH : LOW);
        sysState.lastChangeMs = millis();

        RelayEvent evt;
        evt.index = i;
        evt.state = R;
        evt.timestamp = millis();
        xQueueSend(relayEventQueue, &evt, 0);
      }
    }
    xSemaphoreGive(stateMutex);
  }
}

void relaySwitchTask(void *pvParameters) {
  relaySwitchInit();
  updateRelayHardware();

  while (true) {
    bool changed = false;
    for (int i = 0; i < NUM_RELAYS; i++) {
      bool currentSwitch = (digitalRead(SWITCH_PINS[i]) == HIGH);

      if (currentSwitch != lastSwitchState[i]) {
        lastSwitchState[i] = currentSwitch;
        changed = true;
      }
    }

    if (changed) {
      updateRelayHardware();
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void currentSensorInit() {
  pinMode(ACS712_PIN, INPUT);
  analogSetAttenuation(ADC_11db);
  analogReadResolution(12);

  delay(300);

#if ACS712_AUTO_CALIBRATE
  long sum = 0;
  for (int i = 0; i < 1000; i++) {
    sum += analogReadMilliVolts(ACS712_PIN);
    delayMicroseconds(100);
  }
  acs712ZeroMv = sum / 1000.0f;
#else
  acs712ZeroMv = ACS712_ZERO_POINT_MV;
#endif
}

float readCurrent() {
  float sumSq = 0.0f;

  for (int i = 0; i < ACS712_RMS_SAMPLES; i++) {
    float mv       = analogReadMilliVolts(ACS712_PIN);
    float deltaMv  = (mv - acs712ZeroMv) * ACS712_DIVIDER_RATIO;
    float iSample  = (deltaMv / 1000.0f) / ACS712_SENSITIVITY;
    sumSq         += iSample * iSample;
    delayMicroseconds(100);
  }

  float rms = sqrtf(sumSq / ACS712_RMS_SAMPLES);

  return (rms < ACS712_DEADBAND_A) ? 0.0f : rms;
}

void currentSensorTask(void *pvParameters) {
  currentSensorInit();

  while (true) {
    float amps = readCurrent();

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      sysState.currentAmps = amps;
      xSemaphoreGive(stateMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(ACS712_READ_INTERVAL_MS));
  }
}

static DNSServer *pDnsServer = nullptr;
static AsyncWebServer *pProvServer = nullptr;

static volatile int connectStatus = 0;
static String pendingSSID = "";
static String pendingPass = "";

#include "provision_html.h"

static String scanNetworks() {
  int n = WiFi.scanNetworks(false, true);
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    String ssid = WiFi.SSID(i);
    ssid.replace("\"", "\\\"");
    json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
  }
  json += "]";
  WiFi.scanDelete();

  return json;
}

static void wifiConnectAttemptTask(void *pvParams) {
  connectStatus = 1;

  WiFi.begin(pendingSSID.c_str(), pendingPass.c_str());

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    vTaskDelay(pdMS_TO_TICKS(500));
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString(NVS_SSID_KEY, pendingSSID);
    prefs.putString(NVS_PASS_KEY, pendingPass);
    prefs.putBool(NVS_PROV_KEY, true);
    prefs.end();

    wifiPassword = pendingPass;
    connectStatus = 2;

    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP.restart();
  } else {
    connectStatus = 3;
    WiFi.disconnect();
  }

  vTaskDelete(NULL);
}

static void setupProvisioningRoutes() {
  pProvServer->on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send_P(200, "text/html", PROVISION_HTML);
  });

  pProvServer->on("/scan", HTTP_GET, [](AsyncWebServerRequest *req) {
    String freshNets = scanNetworks();
    req->send(200, "application/json", freshNets);
  });

  pProvServer->on("/connect", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (req->hasParam("ssid", true) && req->hasParam("password", true)) {
      pendingSSID = req->getParam("ssid", true)->value();
      pendingPass = req->getParam("password", true)->value();
      connectStatus = 0;

      xTaskCreatePinnedToCore(
        wifiConnectAttemptTask, "ConnAttempt",
        4096, NULL, 1, NULL, 0);

      req->send(200, "application/json", "{\"status\":\"connecting\"}");
    } else {
      req->send(400, "application/json", "{\"status\":\"error\",\"msg\":\"Missing SSID or password\"}");
    }
  });

  pProvServer->on("/connect-status", HTTP_GET, [](AsyncWebServerRequest *req) {
    String s;
    switch (connectStatus) {
      case 0: s = "idle"; break;
      case 1: s = "connecting"; break;
      case 2: s = "connected"; break;
      case 3: s = "failed"; break;
      default: s = "unknown";
    }
    if (connectStatus == 2) {
      req->send(200, "application/json", "{\"status\":\"connected\",\"ip\":\"" + WiFi.localIP().toString() + "\"}");
    } else {
      req->send(200, "application/json", "{\"status\":\"" + s + "\"}");
    }
  });

  pProvServer->on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->redirect("/");
  });
  pProvServer->on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->redirect("/");
  });
  pProvServer->on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->redirect("/");
  });
  pProvServer->on("/fwlink", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->redirect("/");
  });

  pProvServer->onNotFound([](AsyncWebServerRequest *req) {
    req->redirect("/");
  });
}

bool wifiManagerInit() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);
  bool provisioned = prefs.getBool(NVS_PROV_KEY, false);
  String savedSSID = prefs.getString(NVS_SSID_KEY, "");
  wifiPassword = prefs.getString(NVS_PASS_KEY, "");
  prefs.end();

  if (provisioned && savedSSID.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(MDNS_HOSTNAME);
    WiFi.begin(savedSSID.c_str(), wifiPassword.c_str());

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 30) {
      delay(500);
      retries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sysState.wifiConnected = true;
        sysState.wifiRSSI = WiFi.RSSI();
        strncpy(sysState.wifiSSID, savedSSID.c_str(), sizeof(sysState.wifiSSID) - 1);
        sysState.wifiSSID[sizeof(sysState.wifiSSID) - 1] = '\0';
        xSemaphoreGive(stateMutex);
      }

      if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
      }

      return true;
    }
  }

  isProvisioningMode = true;

  WiFi.mode(WIFI_AP_STA);
  scanNetworks();

  WiFi.softAP(PROV_AP_SSID);
  delay(100);

  pDnsServer = new DNSServer();
  pDnsServer->start(53, "*", WiFi.softAPIP());

  pProvServer = new AsyncWebServer(80);
  setupProvisioningRoutes();
  pProvServer->begin();

  return false;
}

void wifiManagerLoop() {
  if (isProvisioningMode && pDnsServer) {
    pDnsServer->processNextRequest();
  }
}

void resetWiFiCredentials() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.clear();
  prefs.end();

  delay(1000);
  ESP.restart();
}

void wifiReconnectTask(void *pvParameters) {
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(15000));

    if (isProvisioningMode) continue;

    bool connected = (WiFi.status() == WL_CONNECTED);

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      sysState.wifiConnected = connected;
      if (connected) {
        sysState.wifiRSSI = WiFi.RSSI();
      }
      xSemaphoreGive(stateMutex);
    }

    if (!connected) {
      WiFi.reconnect();

      int retries = 0;
      while (WiFi.status() != WL_CONNECTED && retries < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        retries++;
      }

      if (WiFi.status() == WL_CONNECTED) {
        MDNS.end();
        if (MDNS.begin(MDNS_HOSTNAME)) {
          MDNS.addService("http", "tcp", 80);
        }

        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
          sysState.wifiConnected = true;
          sysState.wifiRSSI = WiFi.RSSI();
          xSemaphoreGive(stateMutex);
        }
      }
    }
  }
}

void resetButtonTask(void *pvParameters) {
  pinMode(RESET_BTN_PIN, INPUT_PULLUP);

  while (true) {
    if (digitalRead(RESET_BTN_PIN) == LOW) {
      unsigned long pressStart = millis();

      while (digitalRead(RESET_BTN_PIN) == LOW) {
        vTaskDelay(pdMS_TO_TICKS(100));

        unsigned long held = millis() - pressStart;

        if (held >= RESET_HOLD_MS) {
          resetWiFiCredentials();
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

static HardwareSerial UartSerial(2);

static bool lastSentRelayStates[NUM_RELAYS] = { false };
static UartData lastReceivedData;
static bool uartInitialized = false;

void uartCommInit() {
  UartSerial.begin(UART2_BAUD, SERIAL_8N1, UART2_RX_PIN, UART2_TX_PIN);
  memset(&lastReceivedData, 0, sizeof(lastReceivedData));
  uartInitialized = true;
}

void uartSendRelayCommand(int relayIndex, bool state) {
  if (!uartInitialized) return;
  if (relayIndex < 0 || relayIndex >= NUM_RELAYS) return;

  String msg = "{\"type\":\"relay\",\"index\":";
  msg += String(relayIndex);
  msg += ",\"state\":";
  msg += state ? "true" : "false";
  msg += "}";

  UartSerial.println(msg);
}

static void checkAndSendRelayUpdates() {
  for (int i = 0; i < NUM_RELAYS; i++) {
    bool current = getRelayState(i);
    if (current != lastSentRelayStates[i]) {
      lastSentRelayStates[i] = current;
      uartSendRelayCommand(i, current);
    }
  }
}

static bool parseIncomingData(const String &msg) {
  if (msg.indexOf("\"type\":\"status\"") < 0) {
    if (msg.indexOf("\"type\":\"cmd\"") >= 0) {
      int idxPos = msg.indexOf("\"index\":");
      int stPos = msg.indexOf("\"state\":");
      if (idxPos >= 0 && stPos >= 0) {
        int idx = msg.substring(idxPos + 8, msg.indexOf(",", idxPos + 8)).toInt();
        String stStr = msg.substring(stPos + 8, stPos + 13);
        stStr.trim();
        bool st = stStr.startsWith("true");
        if (idx >= 0 && idx < NUM_RELAYS) {
          setRelayState(idx, st);
        }
      }
    }
    return false;
  }

  UartData newData;
  memset(&newData, 0, sizeof(newData));
  newData.hasNewData = true;

  int curPos = msg.indexOf("\"current\":");
  if (curPos >= 0) {
    int valStart = curPos + 10;
    int valEnd = msg.indexOf(",", valStart);
    if (valEnd < 0) valEnd = msg.indexOf("}", valStart);
    newData.deviceCurrent = msg.substring(valStart, valEnd).toFloat();
  }

  int errPos = msg.indexOf("\"err\":[");
  if (errPos >= 0) {
    int arrStart = errPos + 7;
    for (int i = 0; i < 4; i++) {
      if (arrStart < (int)msg.length()) {
        newData.errors[i] = (msg.charAt(arrStart) == '1' || msg.charAt(arrStart) == 't');
        arrStart = msg.indexOf(",", arrStart);
        if (arrStart >= 0) arrStart++;
        else break;
      }
    }
  }

  int msgPos = msg.indexOf("\"msg\":\"");
  if (msgPos >= 0) {
    int msgStart = msgPos + 7;
    int msgEnd = msg.indexOf("\"", msgStart);
    if (msgEnd >= 0) {
      String errMsg = msg.substring(msgStart, msgEnd);
      strncpy(newData.errorMsg, errMsg.c_str(), sizeof(newData.errorMsg) - 1);
      newData.errorMsg[sizeof(newData.errorMsg) - 1] = '\0';
    }
  }

  bool changed = false;
  if (fabs(newData.deviceCurrent - lastReceivedData.deviceCurrent) > 0.01f) changed = true;
  for (int i = 0; i < 4; i++) {
    if (newData.errors[i] != lastReceivedData.errors[i]) changed = true;
  }
  if (strcmp(newData.errorMsg, lastReceivedData.errorMsg) != 0) changed = true;

  if (changed) {
    memcpy(&lastReceivedData, &newData, sizeof(UartData));

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      memcpy(&sysState.uartData, &newData, sizeof(UartData));
      xSemaphoreGive(stateMutex);
    }

    return true;
  }

  return false;
}

void uartCommTask(void *pvParameters) {
  uartCommInit();

  String rxBuffer = "";

  while (true) {
    checkAndSendRelayUpdates();

    while (UartSerial.available()) {
      char c = UartSerial.read();

      if (c == '\n') {
        rxBuffer.trim();
        if (rxBuffer.length() > 0) {
          parseIncomingData(rxBuffer);
        }
        rxBuffer = "";
      } else if (c != '\r') {
        rxBuffer += c;

        if (rxBuffer.length() > 256) {
          rxBuffer = "";
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

static String buildPayload() {
  String json = "{";

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    json += "\"relays\":[";
    for (int i = 0; i < NUM_RELAYS; i++) {
      if (i > 0) json += ",";
      json += sysState.relayStates[i] ? "true" : "false";
    }
    json += "],";

    json += "\"current\":" + String(sysState.currentAmps, 2) + ",";

    json += "\"rssi\":" + String(sysState.wifiRSSI) + ",";

    if (sysState.uartData.hasNewData) {
      json += "\"uart\":{";
      json += "\"current\":" + String(sysState.uartData.deviceCurrent, 2) + ",";
      json += "\"errors\":[";
      for (int i = 0; i < 4; i++) {
        if (i > 0) json += ",";
        json += sysState.uartData.errors[i] ? "true" : "false";
      }
      json += "]";
      if (strlen(sysState.uartData.errorMsg) > 0) {
        json += ",\"msg\":\"" + String(sysState.uartData.errorMsg) + "\"";
      }
      json += "},";
    }

    xSemaphoreGive(stateMutex);
  }

  json += "\"uptime\":" + String(millis());
  json += "}";

  return json;
}

static void parseCommands(const String &response) {
  int searchFrom = 0;

  while (true) {
    int relayKeyPos = response.indexOf("\"relay\":", searchFrom);
    if (relayKeyPos < 0) break;

    int valStart = relayKeyPos + 8;
    int valEnd = response.indexOf(",", valStart);
    if (valEnd < 0) valEnd = response.indexOf("}", valStart);
    if (valEnd < 0) break;

    String relayStr = response.substring(valStart, valEnd);
    relayStr.trim();
    int relayIdx = relayStr.toInt();

    int stateKeyPos = response.indexOf("\"state\":", valEnd);
    if (stateKeyPos < 0 || stateKeyPos > valEnd + 30) {
      searchFrom = valEnd + 1;
      continue;
    }

    int stateStart = stateKeyPos + 8;
    String stateStr = response.substring(stateStart, stateStart + 5);
    stateStr.trim();
    bool state = stateStr.startsWith("true");

    if (relayIdx >= 0 && relayIdx < NUM_RELAYS) {
      setRelayState(relayIdx, state);
    }

    searchFrom = stateStart + 5;
  }
}

void cloudClientTask(void *pvParameters) {
  while (true) {
    RelayEvent event;
    bool gotEvent = (xQueueReceive(relayEventQueue, &event,
                                   pdMS_TO_TICKS(HEARTBEAT_MS))
                     == pdTRUE);

    if (gotEvent) {
      RelayEvent extra;
      while (xQueueReceive(relayEventQueue, &extra, 0) == pdTRUE) {
      }
    }

    if (WiFi.status() != WL_CONNECTED) {
      continue;
    }

    String payload = buildPayload();

    WiFiClientSecure *client = new WiFiClientSecure;
    if (!client) {
      continue;
    }

    client->setInsecure();

    HTTPClient https;
    if (https.begin(*client, CLOUD_URL)) {
      https.addHeader("Content-Type", "application/json");
      https.setTimeout(HTTP_TIMEOUT_MS);

      int code = https.POST(payload);

      if (code > 0) {
        if (code == HTTP_CODE_OK) {
          String response = https.getString();
          if (response.indexOf("\"commands\"") >= 0) {
            parseCommands(response);
          }
        }
      }

      https.end();
    }

    delete client;

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

static AsyncWebServer dashServer(80);

#include "dashboard_html.h"

static String buildStatusJSON() {
  String json = "{";
  json += "\"relays\":[";
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    for (int i = 0; i < NUM_RELAYS; i++) {
      if (i > 0) json += ",";
      json += sysState.relayStates[i] ? "true" : "false";
    }
    json += "],";
    json += "\"locks\":[";
    for (int i = 0; i < NUM_RELAYS; i++) {
      if (i > 0) json += ",";
      json += sysState.lockedStates[i] ? "true" : "false";
    }
    json += "],";
    json += "\"shutdown\":" + String(sysState.masterShutdown ? "true" : "false") + ",";
    json += "\"current\":" + String(sysState.currentAmps, 2) + ",";
    json += "\"rssi\":" + String(sysState.wifiRSSI) + ",";
    json += "\"ssid\":\"" + String(sysState.wifiSSID) + "\",";
    xSemaphoreGive(stateMutex);
  } else {
    json += "],\"locks\":[false,false,false,false,false,false],\"shutdown\":false,\"current\":0,\"rssi\":0,\"ssid\":\"\",";
  }
  json += "\"uptime\":" + String(millis());
  json += "}";
  return json;
}

void dashboardInit() {
  dashServer.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send_P(200, "text/html", DASHBOARD_HTML);
  });

  dashServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", buildStatusJSON());
  });

  dashServer.on(
    "/api/relay", HTTP_POST,
    [](AsyncWebServerRequest *req) {},
    NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
      String body = String((char *)data).substring(0, len);
      int relayPos = body.indexOf("\"relay\":");
      int statePos = body.indexOf("\"state\":");

      if (relayPos < 0 || statePos < 0) {
        req->send(400, "application/json", "{\"error\":\"Invalid request\"}");
        return;
      }

      int valStart = relayPos + 8;
      int valEnd = body.indexOf(",", valStart);
      if (valEnd < 0) valEnd = body.indexOf("}", valStart);
      int relayIdx = body.substring(valStart, valEnd).toInt();

      int stStart = statePos + 8;
      String stStr = body.substring(stStart, stStart + 5);
      stStr.trim();
      bool state = stStr.startsWith("true");

      if (relayIdx >= 0 && relayIdx < NUM_RELAYS) {
        setRelayState(relayIdx, state);
        req->send(200, "application/json",
                  "{\"success\":true,\"relay\":" + String(relayIdx) + ",\"state\":" + (state ? "true" : "false") + "}");
      } else {
        req->send(400, "application/json", "{\"error\":\"Invalid index\"}");
      }
    });

  dashServer.on(
    "/api/lock", HTTP_POST,
    [](AsyncWebServerRequest *req) {},
    NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
      String body = String((char *)data).substring(0, len);
      int relayPos = body.indexOf("\"relay\":");
      int statePos = body.indexOf("\"state\":");

      if (relayPos < 0 || statePos < 0) {
        req->send(400, "application/json", "{\"error\":\"Invalid request\"}");
        return;
      }

      int valStart = relayPos + 8;
      int valEnd = body.indexOf(",", valStart);
      if (valEnd < 0) valEnd = body.indexOf("}", valStart);
      int relayIdx = body.substring(valStart, valEnd).toInt();

      int stStart = statePos + 8;
      String stStr = body.substring(stStart, stStart + 5);
      stStr.trim();
      bool state = stStr.startsWith("true");

      if (relayIdx >= 0 && relayIdx < NUM_RELAYS) {
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
          sysState.lockedStates[relayIdx] = state;
          xSemaphoreGive(stateMutex);
        }
        updateRelayHardware();
        req->send(200, "application/json",
                  "{\"success\":true,\"relay\":" + String(relayIdx) + ",\"locked\":" + (state ? "true" : "false") + "}");
      } else {
        req->send(400, "application/json", "{\"error\":\"Invalid index\"}");
      }
    });

  dashServer.on(
    "/api/shutdown", HTTP_POST,
    [](AsyncWebServerRequest *req) {},
    NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
      String body = String((char *)data).substring(0, len);
      int statePos = body.indexOf("\"state\":");

      if (statePos < 0) {
        req->send(400, "application/json", "{\"error\":\"Invalid request\"}");
        return;
      }

      int stStart = statePos + 8;
      String stStr = body.substring(stStart, stStart + 5);
      stStr.trim();
      bool state = stStr.startsWith("true");

      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        sysState.masterShutdown = state;
        xSemaphoreGive(stateMutex);
      }

      updateRelayHardware();

      req->send(200, "application/json",
                "{\"success\":true,\"shutdown\":" + String(state ? "true" : "false") + "}");
    });

  dashServer.onNotFound([](AsyncWebServerRequest *req) {
    req->redirect("/");
  });

  dashServer.begin();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  memset(&sysState, 0, sizeof(SystemState));

  relayEventQueue = xQueueCreate(10, sizeof(RelayEvent));
  stateMutex = xSemaphoreCreateMutex();

  if (!relayEventQueue || !stateMutex) {
    delay(1000);
    ESP.restart();
  }

  xTaskCreatePinnedToCore(
    relaySwitchTask,
    "RelaySwitch",
    4096,
    NULL,
    2,
    &hRelaySwitch,
    1
  );

  xTaskCreatePinnedToCore(
    currentSensorTask,
    "CurrentSensor",
    4096,
    NULL,
    1,
    &hCurrentSensor,
    1
  );

  xTaskCreatePinnedToCore(
    resetButtonTask,
    "ResetBtn",
    2048,
    NULL,
    1,
    &hResetButton,
    0
  );

  bool wifiConnected = wifiManagerInit();

  if (wifiConnected) {
    dashboardInit();

    #if CLOUD_ENABLED
    xTaskCreatePinnedToCore(
      cloudClientTask,
      "CloudClient",
      8192,
      NULL,
      1,
      &hCloudClient,
      0
    );
    #endif

    xTaskCreatePinnedToCore(
      uartCommTask,
      "UartComm",
      4096,
      NULL,
      1,
      &hUartComm,
      0
    );

    xTaskCreatePinnedToCore(
      wifiReconnectTask,
      "WifiRecon",
      4096,
      NULL,
      1,
      &hWifiReconnect,
      0
    );
  }
}

void loop() {
  if (isProvisioningMode) {
    wifiManagerLoop();
    vTaskDelay(pdMS_TO_TICKS(10));
  } else {
    vTaskDelay(portMAX_DELAY);
  }
}
