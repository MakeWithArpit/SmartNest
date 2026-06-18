#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <HardwareSerial.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <AsyncWebSocket.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#define NUM_RELAYS 6
#define FIRMWARE_VERSION "1.0.0"

#define RELAY_1_PIN 26
#define RELAY_2_PIN 27
#define RELAY_3_PIN 14
#define RELAY_4_PIN 23
#define RELAY_5_PIN 25
#define RELAY_6_PIN 33

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

#define HEARTBEAT_MS   30000
#define MDNS_HOSTNAME "smart-nest"
#define PROV_AP_SSID "SmartNest"
#define NVS_NAMESPACE "smartnest"
#define NVS_SSID_KEY "wifi_ssid"
#define NVS_PASS_KEY "wifi_pass"
#define NVS_PROV_KEY "provisioned"

// MQTT Configuration
#define MQTT_ENABLED       true
#define MQTT_BROKER_HOST   "broker.hivemq.com"
#define MQTT_BROKER_PORT   1883
#define MQTT_CLIENT_ID     "SmartNest_001"
#define MQTT_USERNAME      ""
#define MQTT_PASSWORD      ""
#define MQTT_KEEPALIVE_S   60
#define MQTT_BASE_TOPIC    "smartnest"

struct RelayEvent {
  int index;
  bool state;
  unsigned long timestamp;
};

struct SystemState {
  // Direct GPIO Relays (0-5)
  bool  relayStates[NUM_RELAYS];
  bool  dashboardStates[NUM_RELAYS];
  bool  masterShutdown;
  bool  lockedStates[NUM_RELAYS];
  bool  wifiConnected;
  int   wifiRSSI;
  char  wifiSSID[33];
  unsigned long lastChangeMs;
  float currentAmps; // Internet Board's own combined ACS712 current

  // Telemetry from Master via UART
  float  acsCurrentA;        // Digital Slave's ACS712 current
  float  pzemVoltage;        // PZEM voltage (V)
  float  pzemCurrentA;       // PZEM current (A)
  float  pzemPowerW;         // PZEM apparent power (W)
  double energyDailyWh;      // Daily energy (Wh)
  double energyMonthlyWh;    // Monthly energy (Wh)
  double energyLifetimeWh;   // Lifetime energy (Wh)
  bool   digitalSlaveOnline;
  bool   pzemSlaveOnline;
  int    digitalSlaveRSSI;
  int    pzemSlaveRSSI;

  // 7th Relay (Digital Board Slave)
  bool   digitalRelayState;
  bool   digitalRelayLocked;
  bool   digitalSwitchState;
};

const int RELAY_PINS[NUM_RELAYS] = {
  RELAY_1_PIN, RELAY_2_PIN, RELAY_3_PIN,
  RELAY_4_PIN, RELAY_5_PIN, RELAY_6_PIN
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

// UART command items queue
struct UartCmdItem {
  char text[256];
};
QueueHandle_t UartCmdQueue = NULL;

static TaskHandle_t hRelaySwitch = NULL;
static TaskHandle_t hCurrentSensor = NULL;
static TaskHandle_t hUartComm = NULL;
static TaskHandle_t hWifiReconnect = NULL;
static TaskHandle_t hResetButton = NULL;
static TaskHandle_t hNtpSync = NULL;
static TaskHandle_t hMqttTask = NULL;

// WebSocket
AsyncWebSocket ws("/ws");

// MQTT objects
#if MQTT_ENABLED
WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);
#endif

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
void dashboardInit();
void pushWsUpdate();

void relaySwitchTask(void *pvParameters);
void currentSensorTask(void *pvParameters);
void resetButtonTask(void *pvParameters);
void wifiReconnectTask(void *pvParameters);
void uartCommTask(void *pvParameters);
void ntpSyncTask(void *pvParameters);
void mqttTask(void *pvParameters);

void enqueueUartCmd(const String &cmd) {
  if (UartCmdQueue == NULL) return;
  UartCmdItem item;
  strncpy(item.text, cmd.c_str(), sizeof(item.text) - 1);
  item.text[sizeof(item.text) - 1] = '\0';
  xQueueSend(UartCmdQueue, &item, 0);
}

void relaySwitchInit() {
  for (int i = 0; i < NUM_RELAYS; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], LOW);
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
      bool R = false;

      if (sysState.masterShutdown || sysState.lockedStates[i]) {
        R = false;
      } else {
        R = D;
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
    updateRelayHardware();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void currentSensorInit() {
  pinMode(ACS712_PIN, INPUT);
  analogSetAttenuation(ADC_11db);
  analogReadResolution(12);
  delay(300);
  acs712ZeroMv = ACS712_ZERO_POINT_MV;
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

// WebSocket publisher helper
void pushWsUpdate() {
  if (ws.count() == 0) return;
  String payload = buildStatusJSON();
  ws.textAll(payload);
}

// MQTT Callback and publishing helpers
#if MQTT_ENABLED
int getRelayIndexFromTopic(const String &topic) {
  int prefixLen = strlen(MQTT_BASE_TOPIC) + 7; // "smartnest/relay/"
  if (!topic.startsWith(MQTT_BASE_TOPIC "/relay/")) return -1;
  int nextSlash = topic.indexOf('/', prefixLen);
  if (nextSlash < 0) return -1;
  String idxStr = topic.substring(prefixLen, nextSlash);
  return idxStr.toInt();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t = String(topic);
  String p = String((char*)payload, length);
  p.trim();

  // Relay set: smartnest/relay/N/set
  if (t.startsWith(MQTT_BASE_TOPIC "/relay/") && t.endsWith("/set")) {
    int idx = getRelayIndexFromTopic(t);
    if (idx >= 0 && idx < NUM_RELAYS) {
      setRelayState(idx, p == "true");
    } else if (idx == 6) {
      enqueueUartCmd(String("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"") +
                     (p == "true" ? "relay_on" : "relay_off") + "\"}");
    }
  }
  // Relay lock: smartnest/relay/N/lock
  else if (t.startsWith(MQTT_BASE_TOPIC "/relay/") && t.endsWith("/lock")) {
    int idx = getRelayIndexFromTopic(t);
    if (idx >= 0 && idx < NUM_RELAYS) {
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        sysState.lockedStates[idx] = (p == "true");
        xSemaphoreGive(stateMutex);
      }
      updateRelayHardware();
    } else if (idx == 6) {
      enqueueUartCmd(String("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"") +
                     (p == "true" ? "relay_lock" : "relay_unlock") + "\"}");
    }
  }
  // Master shutdown
  else if (t == MQTT_BASE_TOPIC "/cmd/shutdown") {
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      sysState.masterShutdown = (p == "true");
      xSemaphoreGive(stateMutex);
    }
    updateRelayHardware();
  }
  // Slave commands
  else if (t == MQTT_BASE_TOPIC "/cmd/slave/d1") {
    enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"" + p + "\"}");
  }
  else if (t == MQTT_BASE_TOPIC "/cmd/slave/pzem") {
    enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"pzem\",\"cmd\":\"" + p + "\"}");
  }

  pushWsUpdate();
  
  // Republish changed retained state
  if (t.startsWith(MQTT_BASE_TOPIC "/relay/")) {
    int idx = getRelayIndexFromTopic(t);
    bool stateVal = false;
    bool lockedVal = false;
    bool gotMutex = false;
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      if (idx >= 0 && idx < NUM_RELAYS) {
        stateVal = sysState.relayStates[idx];
        lockedVal = sysState.lockedStates[idx];
      } else if (idx == 6) {
        stateVal = sysState.digitalRelayState;
        lockedVal = sysState.digitalRelayLocked;
      }
      gotMutex = true;
      xSemaphoreGive(stateMutex);
    }
    if (gotMutex) {
      if (idx >= 0 && idx < NUM_RELAYS) {
        mqttClient.publish((MQTT_BASE_TOPIC "/relay/" + String(idx) + "/state").c_str(),
                           stateVal ? "true" : "false", true);
        mqttClient.publish((MQTT_BASE_TOPIC "/relay/" + String(idx) + "/locked").c_str(),
                           lockedVal ? "true" : "false", true);
      } else if (idx == 6) {
        mqttClient.publish(MQTT_BASE_TOPIC "/relay/6/state", stateVal ? "true" : "false", true);
        mqttClient.publish(MQTT_BASE_TOPIC "/relay/6/locked", lockedVal ? "true" : "false", true);
      }
    }
  } else if (t == MQTT_BASE_TOPIC "/cmd/shutdown") {
    bool shutdownVal = false;
    bool gotMutex = false;
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      shutdownVal = sysState.masterShutdown;
      gotMutex = true;
      xSemaphoreGive(stateMutex);
    }
    if (gotMutex) {
      mqttClient.publish(MQTT_BASE_TOPIC "/shutdown", shutdownVal ? "true" : "false", true);
    }
  }
}

void publishAllRetainedStates() {
  bool localRelays[NUM_RELAYS];
  bool localLocks[NUM_RELAYS];
  bool localDigitalRelay = false;
  bool localDigitalLocked = false;
  bool localShutdown = false;
  bool gotMutex = false;
  
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    for (int i = 0; i < NUM_RELAYS; i++) {
      localRelays[i] = sysState.relayStates[i];
      localLocks[i] = sysState.lockedStates[i];
    }
    localDigitalRelay = sysState.digitalRelayState;
    localDigitalLocked = sysState.digitalRelayLocked;
    localShutdown = sysState.masterShutdown;
    gotMutex = true;
    xSemaphoreGive(stateMutex);
  }
  
  if (gotMutex) {
    for (int i = 0; i < NUM_RELAYS; i++) {
      mqttClient.publish((MQTT_BASE_TOPIC "/relay/" + String(i) + "/state").c_str(),
                         localRelays[i] ? "true" : "false", true);
      mqttClient.publish((MQTT_BASE_TOPIC "/relay/" + String(i) + "/locked").c_str(),
                         localLocks[i] ? "true" : "false", true);
    }
    mqttClient.publish(MQTT_BASE_TOPIC "/relay/6/state", localDigitalRelay ? "true" : "false", true);
    mqttClient.publish(MQTT_BASE_TOPIC "/relay/6/locked", localDigitalLocked ? "true" : "false", true);
    mqttClient.publish(MQTT_BASE_TOPIC "/shutdown", localShutdown ? "true" : "false", true);
  }
}

void publishTelemetry() {
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    mqttClient.publish(MQTT_BASE_TOPIC "/sensor/voltage", String(sysState.pzemVoltage, 1).c_str(), false);
    mqttClient.publish(MQTT_BASE_TOPIC "/sensor/acs", String(sysState.acsCurrentA, 2).c_str(), false);
    mqttClient.publish(MQTT_BASE_TOPIC "/sensor/load", String(sysState.currentAmps, 2).c_str(), false);
    mqttClient.publish(MQTT_BASE_TOPIC "/sensor/power", String(sysState.pzemPowerW, 1).c_str(), false);
    
    mqttClient.publish(MQTT_BASE_TOPIC "/energy/daily", String(sysState.energyDailyWh / 1000.0, 3).c_str(), false);
    mqttClient.publish(MQTT_BASE_TOPIC "/energy/monthly", String(sysState.energyMonthlyWh / 1000.0, 3).c_str(), false);
    mqttClient.publish(MQTT_BASE_TOPIC "/energy/lifetime", String(sysState.energyLifetimeWh / 1000.0, 3).c_str(), false);
    
    mqttClient.publish(MQTT_BASE_TOPIC "/slave/d1/online", sysState.digitalSlaveOnline ? "true" : "false", true);
    mqttClient.publish(MQTT_BASE_TOPIC "/slave/pzem/online", sysState.pzemSlaveOnline ? "true" : "false", true);
    
    mqttClient.publish(MQTT_BASE_TOPIC "/switch/6/state", sysState.digitalSwitchState ? "true" : "false", false);
    
    // Heartbeat payload
    String heartbeat = "{\"uptime\":" + String(millis()) + ",\"ssid\":\"" + String(sysState.wifiSSID) + "\",\"rssi\":" + String(sysState.wifiRSSI) + "}";
    mqttClient.publish(MQTT_BASE_TOPIC "/status", heartbeat.c_str(), false);
    
    xSemaphoreGive(stateMutex);
  }
}

void mqttTask(void* pvParameters) {
  mqttClient.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(MQTT_KEEPALIVE_S);

  uint32_t lastReconnectAttempt = 0;
  uint32_t lastHeartbeat = 0;
  bool wasMqttConnected = false;

  while (true) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!mqttClient.connected()) {
        if (wasMqttConnected) {
          enqueueUartCmd("{\"t\":\"cloud\",\"up\":false}");
          wasMqttConnected = false;
        }
        if (millis() - lastReconnectAttempt > 5000) {
          lastReconnectAttempt = millis();
          bool ok;
          if (strlen(MQTT_USERNAME) > 0)
            ok = mqttClient.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD);
          else
            ok = mqttClient.connect(MQTT_CLIENT_ID);
          if (ok) {
            mqttClient.subscribe(MQTT_BASE_TOPIC "/relay/+/set");
            mqttClient.subscribe(MQTT_BASE_TOPIC "/relay/+/lock");
            mqttClient.subscribe(MQTT_BASE_TOPIC "/cmd/shutdown");
            mqttClient.subscribe(MQTT_BASE_TOPIC "/cmd/slave/d1");
            mqttClient.subscribe(MQTT_BASE_TOPIC "/cmd/slave/pzem");
            
            publishAllRetainedStates();
            enqueueUartCmd("{\"t\":\"cloud\",\"up\":true}");
            wasMqttConnected = true;
          }
        }
      } else {
        mqttClient.loop();
        if (millis() - lastHeartbeat > HEARTBEAT_MS) {
          lastHeartbeat = millis();
          publishTelemetry();
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
#endif

// UART COMM TASK (Internet ESP32 Side)
void uartCommInit() {
  Serial2.begin(UART2_BAUD, SERIAL_8N1, UART2_RX_PIN, UART2_TX_PIN);
}

void uartCommTask(void *pvParameters) {
  uartCommInit();
  
  String rxBuffer = "";
  uint32_t lastAcsSentTime = 0;

  while (true) {
    // TX: Send pending slave command or combined current telemetry
    UartCmdItem txItem;
    if (xQueueReceive(UartCmdQueue, &txItem, 0) == pdTRUE) {
      Serial2.println(txItem.text);
    }
    
    uint32_t now = millis();
    if (now - lastAcsSentTime >= 10000) {
      lastAcsSentTime = now;
      float ampsVal = 0.0f;
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        ampsVal = sysState.currentAmps;
        xSemaphoreGive(stateMutex);
      }
      String acsMsg = "{\"t\":\"acs\",\"i\":" + String(ampsVal, 2) + "}";
      Serial2.println(acsMsg);
    }

    // RX: Read incoming serial line from Master
    while (Serial2.available() > 0) {
      char c = Serial2.read();
      if (c == '\n') {
        rxBuffer.trim();
        if (rxBuffer.length() > 0) {
          // Parse JSON
          StaticJsonDocument<1024> doc;
          DeserializationError err = deserializeJson(doc, rxBuffer);
          if (!err) {
            String type = doc["t"];
            if (type == "tel") {
              if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                sysState.acsCurrentA = doc["acs"];
                sysState.pzemVoltage = doc["v"];
                sysState.pzemCurrentA = doc["pi"];
                sysState.pzemPowerW = doc["pp"];
                sysState.energyDailyWh = doc["d_wh"];
                sysState.energyMonthlyWh = doc["m_wh"];
                sysState.energyLifetimeWh = doc["l_wh"];
                sysState.digitalSlaveOnline = doc["d_on"];
                sysState.pzemSlaveOnline = doc["p_on"];
                sysState.digitalSlaveRSSI = doc["d_rssi"];
                sysState.pzemSlaveRSSI = doc["p_rssi"];
                sysState.digitalRelayState = doc["d_relay"];
                sysState.digitalSwitchState = doc["d_sw"];
                sysState.digitalRelayLocked = doc["d_lock"];
                xSemaphoreGive(stateMutex);
              }
              pushWsUpdate();
              #if MQTT_ENABLED
              if (mqttClient.connected()) {
                publishTelemetry();
              }
              #endif
            }
            else if (type == "sw") {
              int idx = doc["idx"];
              int s = doc["s"];
              if (idx >= 0 && idx < NUM_RELAYS) {
                setRelayState(idx, s == 1);
              }
            }
            else if (type == "off") {
              String dev = doc["dev"];
              if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                if (dev == "d1") sysState.digitalSlaveOnline = false;
                else if (dev == "pzem") sysState.pzemSlaveOnline = false;
                xSemaphoreGive(stateMutex);
              }
              pushWsUpdate();
              #if MQTT_ENABLED
              if (mqttClient.connected()) {
                mqttClient.publish((MQTT_BASE_TOPIC "/slave/" + dev + "/online").c_str(), "false", true);
              }
              #endif
            }
            else if (type == "on") {
              String dev = doc["dev"];
              if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                if (dev == "d1") sysState.digitalSlaveOnline = true;
                else if (dev == "pzem") sysState.pzemSlaveOnline = true;
                xSemaphoreGive(stateMutex);
              }
              pushWsUpdate();
              #if MQTT_ENABLED
              if (mqttClient.connected()) {
                mqttClient.publish((MQTT_BASE_TOPIC "/slave/" + dev + "/online").c_str(), "true", true);
              }
              #endif
            }
            else if (type == "hist") {
              uint32_t batch = doc["batch"];
              JsonArray recs = doc["recs"];
              bool allPublished = true;
              uint32_t lastEpoch = 0;
              
              #if MQTT_ENABLED
              if (mqttClient.connected()) {
                for (JsonObject rec : recs) {
                  uint32_t ep = rec["epoch"];
                  float v = rec["v"];
                  float load = rec["load"];
                  float pi = rec["pi"];
                  float pva = rec["powerVA"];
                  lastEpoch = ep;
                  
                  String recJson = "{\"epoch\":" + String(ep) + ",\"v\":" + String(v, 1) + 
                                   ",\"load\":" + String(load, 2) + ",\"pi\":" + String(pi, 3) + 
                                   ",\"powerVA\":" + String(pva, 1) + "}";
                  
                  bool pubOk = mqttClient.publish(MQTT_BASE_TOPIC "/history", recJson.c_str(), false);
                  if (!pubOk) {
                    allPublished = false;
                    break;
                  }
                }
              } else {
                allPublished = false;
              }
              #else
              allPublished = false;
              #endif
              
              if (allPublished && lastEpoch > 0) {
                enqueueUartCmd("{\"t\":\"hist_ack\",\"batch\":" + String(batch) + ",\"upto\":" + String(lastEpoch) + "}");
              }
            }
          }
        }
        rxBuffer = "";
      } else if (c != '\r') {
        rxBuffer += c;
        if (rxBuffer.length() > 1024) {
          rxBuffer = "";
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// NTP Synchronizer Task
void ntpSyncTask(void *pvParameters) {
  while (true) {
    if (WiFi.status() == WL_CONNECTED) {
      configTime(19800, 0, "pool.ntp.org"); // IST (UTC + 5:30)
      struct tm timeinfo;
      int retry = 0;
      bool success = false;
      while (retry < 5) {
        if (getLocalTime(&timeinfo, 1000)) {
          success = true;
          break;
        }
        retry++;
      }
      if (success) {
        time_t nowSecs;
        time(&nowSecs);
        String msg = "{\"t\":\"ntp\",\"epoch\":" + String((uint32_t)nowSecs) + ",\"tz_h\":5,\"tz_m\":30}";
        enqueueUartCmd(msg);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(300000)); // Every 5 minutes
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
    
    // masters / slaves data fields
    json += "\"acs\":"       + String(sysState.acsCurrentA, 2) + ",";
    json += "\"load\":"      + String(sysState.currentAmps, 2) + ",";
    json += "\"v\":"         + String(sysState.pzemVoltage, 1) + ",";
    json += "\"pw\":"        + String(sysState.pzemPowerW, 1) + ",";
    json += "\"daily\":"     + String(sysState.energyDailyWh / 1000.0, 3) + ",";
    json += "\"monthly\":"   + String(sysState.energyMonthlyWh / 1000.0, 3) + ",";
    json += "\"lifetime\":"  + String(sysState.energyLifetimeWh / 1000.0, 3) + ",";
    json += "\"d_on\":"      + String(sysState.digitalSlaveOnline ? "true":"false") + ",";
    json += "\"p_on\":"      + String(sysState.pzemSlaveOnline ? "true":"false") + ",";
    json += "\"d_rssi\":"    + String(sysState.digitalSlaveRSSI) + ",";
    json += "\"p_rssi\":"    + String(sysState.pzemSlaveRSSI) + ",";
    json += "\"d_relay\":"   + String(sysState.digitalRelayState ? "true" : "false") + ",";
    json += "\"d_lock\":"    + String(sysState.digitalRelayLocked ? "true" : "false") + ",";
    json += "\"d_sw\":"      + String(sysState.digitalSwitchState ? "true" : "false");
    
    xSemaphoreGive(stateMutex);
  } else {
    json += "],\"locks\":[false,false,false,false,false,false],\"shutdown\":false,\"current\":0,\"rssi\":0,\"ssid\":\"\",";
    json += "\"acs\":0.00,\"load\":0.00,\"v\":0.0,\"pw\":0.0,\"daily\":0.000,\"monthly\":0.000,\"lifetime\":0.000,\"d_on\":false,\"p_on\":false,\"d_rssi\":0,\"p_rssi\":0,\"d_relay\":false,\"d_lock\":false,\"d_sw\":false";
  }
  json += ",\"uptime\":" + String(millis());
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
      StaticJsonDocument<128> doc;
      DeserializationError err = deserializeJson(doc, data, len);
      if (err) {
        req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      if (!doc.containsKey("relay") || !doc.containsKey("state")) {
        req->send(400, "application/json", "{\"error\":\"Missing fields\"}");
        return;
      }
      int relayIdx = doc["relay"];
      bool state = doc["state"];

      if (relayIdx >= 0 && relayIdx < NUM_RELAYS) {
        setRelayState(relayIdx, state);
        req->send(200, "application/json",
                  "{\"success\":true,\"relay\":" + String(relayIdx) + ",\"state\":" + (state ? "true" : "false") + "}");
      } else if (relayIdx == 6) {
        String cmd = state ? "relay_on" : "relay_off";
        enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"" + cmd + "\"}");
        req->send(200, "application/json",
                  "{\"success\":true,\"relay\":6,\"state\":" + String(state ? "true" : "false") + "}");
      } else {
        req->send(400, "application/json", "{\"error\":\"Invalid index\"}");
      }
    });

  dashServer.on(
    "/api/lock", HTTP_POST,
    [](AsyncWebServerRequest *req) {},
    NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<128> doc;
      DeserializationError err = deserializeJson(doc, data, len);
      if (err) {
        req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      if (!doc.containsKey("relay") || !doc.containsKey("state")) {
        req->send(400, "application/json", "{\"error\":\"Missing fields\"}");
        return;
      }
      int relayIdx = doc["relay"];
      bool state = doc["state"];

      if (relayIdx >= 0 && relayIdx < NUM_RELAYS) {
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
          sysState.lockedStates[relayIdx] = state;
          xSemaphoreGive(stateMutex);
        }
        updateRelayHardware();
        req->send(200, "application/json",
                  "{\"success\":true,\"relay\":" + String(relayIdx) + ",\"locked\":" + (state ? "true" : "false") + "}");
      } else if (relayIdx == 6) {
        String cmd = state ? "relay_lock" : "relay_unlock";
        enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"" + cmd + "\"}");
        req->send(200, "application/json",
                  "{\"success\":true,\"relay\":6,\"locked\":" + String(state ? "true" : "false") + "}");
      } else {
        req->send(400, "application/json", "{\"error\":\"Invalid index\"}");
      }
    });

  dashServer.on(
    "/api/shutdown", HTTP_POST,
    [](AsyncWebServerRequest *req) {},
    NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<128> doc;
      DeserializationError err = deserializeJson(doc, data, len);
      if (err) {
        req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      if (!doc.containsKey("state")) {
        req->send(400, "application/json", "{\"error\":\"Missing fields\"}");
        return;
      }
      bool state = doc["state"];

      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        sysState.masterShutdown = state;
        xSemaphoreGive(stateMutex);
      }
      updateRelayHardware();
      req->send(200, "application/json",
                "{\"success\":true,\"shutdown\":" + String(state ? "true" : "false") + "}");
    });

  // Post handler for slave commands from the web UI
  dashServer.on(
    "/api/slave-cmd", HTTP_POST,
    [](AsyncWebServerRequest *req) {},
    NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<256> doc;
      DeserializationError err = deserializeJson(doc, data, len);
      if (err) {
        req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      String target = doc["target"];
      String cmd = doc["cmd"];
      
      if ((target == "d1" && (cmd == "relay_on" || cmd == "relay_off" || cmd == "relay_lock" || cmd == "relay_unlock" || cmd == "reboot")) ||
          (target == "pzem" && (cmd == "reboot" || cmd == "energy_reset"))) {
        enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"" + target + "\",\"cmd\":\"" + cmd + "\"}");
        req->send(200, "application/json", "{\"success\":true}");
      } else {
        req->send(400, "application/json", "{\"error\":\"Invalid target or command\"}");
      }
    });

  dashServer.onNotFound([](AsyncWebServerRequest *req) {
    req->redirect("/");
  });

  // Register WebSocket server
  ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {});
  dashServer.addHandler(&ws);

  dashServer.begin();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  memset(&sysState, 0, sizeof(SystemState));

  relayEventQueue = xQueueCreate(10, sizeof(RelayEvent));
  stateMutex = xSemaphoreCreateMutex();
  UartCmdQueue = xQueueCreate(5, sizeof(UartCmdItem));

  if (!relayEventQueue || !stateMutex || !UartCmdQueue) {
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

    xTaskCreatePinnedToCore(
      ntpSyncTask,
      "NtpSync",
      4096,
      NULL,
      1,
      &hNtpSync,
      0
    );

    #if MQTT_ENABLED
    xTaskCreatePinnedToCore(
      mqttTask,
      "MqttTask",
      8192,
      NULL,
      1,
      &hMqttTask,
      0
    );
    #endif
  }
}

void loop() {
  if (isProvisioningMode) {
    wifiManagerLoop();
    vTaskDelay(pdMS_TO_TICKS(10));
  } else {
    // Periodically cleanup clients to free memory
    ws.cleanupClients();
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
