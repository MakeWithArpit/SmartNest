#include <Arduino.h>
#include <ArduinoJson.h>
#include <AsyncWebSocket.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <HardwareSerial.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#define NUM_RELAYS 6
#define FIRMWARE_VERSION "1.0.0"

#define RELAY_1_PIN 2
#define RELAY_2_PIN 4
#define RELAY_3_PIN 5
#define RELAY_4_PIN 19
#define RELAY_5_PIN 18
#define RELAY_6_PIN 15

#define ACS712_PIN 34
#define ACS712_SENSITIVITY 0.066f
#define ACS712_DIVIDER_RATIO 1.5f
static float acs712ZeroMv = 2500.0f;

#define RESET_BTN_PIN 0
#define RESET_HOLD_MS 3000

#define UART2_RX_PIN 16
#define UART2_TX_PIN 17
#define UART2_BAUD 115200

#define HEARTBEAT_MS 30000
#define MDNS_HOSTNAME "smart-nest"
#define PROV_AP_SSID "SmartNest"
#define NVS_NAMESPACE "smartnest"
#define NVS_SSID_KEY "wifi_ssid"
#define NVS_PASS_KEY "wifi_pass"
#define NVS_PROV_KEY "provisioned"

// MQTT Configuration
#define MQTT_ENABLED true
#define MQTT_BROKER_HOST "broker.hivemq.com"
#define MQTT_BROKER_PORT 1883
#define MQTT_CLIENT_ID "SmartNest_001"
#define MQTT_USERNAME ""
#define MQTT_PASSWORD ""
#define MQTT_KEEPALIVE_S 60
#define MQTT_BASE_TOPIC "smartnest"

struct MqttConfig {
  bool enabled;
  char broker[64];
  int port;
  char clientId[64];
  char username[64];
  char password[64];
  char baseTopic[64];
  int keepAlive;
};

MqttConfig g_mqttConfig;
volatile bool g_mqttConfigChanged = false;

void saveMqttConfig() {
  Preferences prefs;
  prefs.begin("mqtt_cfg", false);
  prefs.putBool("enabled", g_mqttConfig.enabled);
  prefs.putString("broker", String(g_mqttConfig.broker));
  prefs.putInt("port", g_mqttConfig.port);
  prefs.putString("clientId", String(g_mqttConfig.clientId));
  prefs.putString("username", String(g_mqttConfig.username));
  prefs.putString("password", String(g_mqttConfig.password));
  prefs.putString("baseTopic", String(g_mqttConfig.baseTopic));
  prefs.putInt("keepAlive", g_mqttConfig.keepAlive);
  prefs.end();
  Serial.println("[MQTT] Configuration saved to NVS Preferences");
}

void loadMqttConfig() {
  Preferences prefs;
  prefs.begin("mqtt_cfg", true);
  g_mqttConfig.enabled = prefs.getBool("enabled", MQTT_ENABLED);

  String broker = prefs.getString("broker", MQTT_BROKER_HOST);
  strncpy(g_mqttConfig.broker, broker.c_str(), sizeof(g_mqttConfig.broker) - 1);
  g_mqttConfig.broker[sizeof(g_mqttConfig.broker) - 1] = '\0';

  g_mqttConfig.port = prefs.getInt("port", MQTT_BROKER_PORT);

  String clientId = prefs.getString("clientId", MQTT_CLIENT_ID);
  strncpy(g_mqttConfig.clientId, clientId.c_str(),
          sizeof(g_mqttConfig.clientId) - 1);
  g_mqttConfig.clientId[sizeof(g_mqttConfig.clientId) - 1] = '\0';

  String username = prefs.getString("username", MQTT_USERNAME);
  strncpy(g_mqttConfig.username, username.c_str(),
          sizeof(g_mqttConfig.username) - 1);
  g_mqttConfig.username[sizeof(g_mqttConfig.username) - 1] = '\0';

  String password = prefs.getString("password", MQTT_PASSWORD);
  strncpy(g_mqttConfig.password, password.c_str(),
          sizeof(g_mqttConfig.password) - 1);
  g_mqttConfig.password[sizeof(g_mqttConfig.password) - 1] = '\0';

  String baseTopic = prefs.getString("baseTopic", MQTT_BASE_TOPIC);
  strncpy(g_mqttConfig.baseTopic, baseTopic.c_str(),
          sizeof(g_mqttConfig.baseTopic) - 1);
  g_mqttConfig.baseTopic[sizeof(g_mqttConfig.baseTopic) - 1] = '\0';

  g_mqttConfig.keepAlive = prefs.getInt("keepAlive", MQTT_KEEPALIVE_S);
  prefs.end();
}

void resetMqttConfigToDefault() {
  g_mqttConfig.enabled = MQTT_ENABLED;
  strncpy(g_mqttConfig.broker, MQTT_BROKER_HOST,
          sizeof(g_mqttConfig.broker) - 1);
  g_mqttConfig.broker[sizeof(g_mqttConfig.broker) - 1] = '\0';
  g_mqttConfig.port = MQTT_BROKER_PORT;
  strncpy(g_mqttConfig.clientId, MQTT_CLIENT_ID,
          sizeof(g_mqttConfig.clientId) - 1);
  g_mqttConfig.clientId[sizeof(g_mqttConfig.clientId) - 1] = '\0';
  strncpy(g_mqttConfig.username, MQTT_USERNAME,
          sizeof(g_mqttConfig.username) - 1);
  g_mqttConfig.username[sizeof(g_mqttConfig.username) - 1] = '\0';
  strncpy(g_mqttConfig.password, MQTT_PASSWORD,
          sizeof(g_mqttConfig.password) - 1);
  g_mqttConfig.password[sizeof(g_mqttConfig.password) - 1] = '\0';
  strncpy(g_mqttConfig.baseTopic, MQTT_BASE_TOPIC,
          sizeof(g_mqttConfig.baseTopic) - 1);
  g_mqttConfig.baseTopic[sizeof(g_mqttConfig.baseTopic) - 1] = '\0';
  g_mqttConfig.keepAlive = MQTT_KEEPALIVE_S;
  saveMqttConfig();
  Serial.println("[MQTT] Configuration reset to defaults");
}

struct RelayEvent {
  int index;
  bool state;
  unsigned long timestamp;
};

struct SystemState {
  // Direct GPIO Relays (0-5)
  bool relayStates[NUM_RELAYS];
  bool dashboardStates[NUM_RELAYS];
  bool masterShutdown;
  bool lockedStates[NUM_RELAYS];
  bool wifiConnected;
  int wifiRSSI;
  char wifiSSID[33];
  unsigned long lastChangeMs;
  float currentAmps; // Internet Board's own combined ACS712 current

  // Telemetry from Master via UART
  float acsCurrentA;       // Digital Slave's ACS712 current
  float pzemVoltage;       // PZEM voltage (V)
  float pzemCurrentA;      // PZEM current (A)
  float pzemPowerW;        // PZEM apparent power (W)
  double energyDailyWh;    // Daily energy (Wh)
  double energyMonthlyWh;  // Monthly energy (Wh)
  double energyLifetimeWh; // Lifetime energy (Wh)
  bool digitalSlaveOnline;
  bool pzemSlaveOnline;
  int digitalSlaveRSSI;
  int pzemSlaveRSSI;

  // 7th Relay (Digital Board Slave)
  bool digitalRelayState;
  bool digitalRelayLocked;
  bool digitalSwitchState;

  // Master Lock (global system lock)
  bool masterLock;

  // SD Card stats from Master
  bool sdOk;
  uint64_t sdTotal;
  uint64_t sdUsed;

  // New fields
  bool pzemSensorHealthy;
  int mqttStatus;
};

const int RELAY_PINS[NUM_RELAYS] = {RELAY_1_PIN, RELAY_2_PIN, RELAY_3_PIN,
                                    RELAY_4_PIN, RELAY_5_PIN, RELAY_6_PIN};

const char *const RELAY_NAMES[NUM_RELAYS] = {
    "Light 1", "Light 2", "Light 3", "Light 4", "Light 5", "Power Socket"};

QueueHandle_t relayEventQueue = NULL;
SemaphoreHandle_t stateMutex = NULL;
SystemState sysState;
String wifiPassword = "";
volatile bool isProvisioningMode = false;
volatile bool pendingMasterLockSync = false;
volatile unsigned long pendingMasterLockSyncTime = 0;

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
bool wifiManagerInit();
void wifiManagerLoop();
void resetWiFiCredentials();
void uartCommInit();
void dashboardInit();
void pushWsUpdate();
void setShutdownState(bool state);

void relaySwitchTask(void *pvParameters);
void currentSensorTask(void *pvParameters);
void resetButtonTask(void *pvParameters);
void wifiReconnectTask(void *pvParameters);
void uartCommTask(void *pvParameters);
void ntpSyncTask(void *pvParameters);
void mqttTask(void *pvParameters);

void enqueueUartCmd(const String &cmd) {
  if (UartCmdQueue == NULL)
    return;
  UartCmdItem item;
  strncpy(item.text, cmd.c_str(), sizeof(item.text) - 1);
  item.text[sizeof(item.text) - 1] = '\0';
  xQueueSend(UartCmdQueue, &item, 0);
}

void relaySwitchInit() {
  for (int i = 0; i < NUM_RELAYS; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], HIGH); // Starts OFF (Active LOW)
  }

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    for (int i = 0; i < NUM_RELAYS; i++) {
      sysState.relayStates[i] = false;
      sysState.dashboardStates[i] = false;
      sysState.lockedStates[i] = false;
    }
    sysState.masterShutdown = false;
    sysState.masterLock = false;
    xSemaphoreGive(stateMutex);
  }
}

void setRelayState(int index, bool state) {
  if (index < 0 || index >= NUM_RELAYS)
    return;

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (state && (sysState.masterLock || sysState.masterShutdown ||
                  sysState.lockedStates[index])) {
      Serial.printf("[LOCK] setRelayState(%d, ON) blocked due to active lock "
                    "or shutdown\n",
                    index);
    } else {
      sysState.dashboardStates[index] = state;
    }
    xSemaphoreGive(stateMutex);
  }

  updateRelayHardware();
}

void setShutdownState(bool state) {
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    sysState.masterShutdown = state;
    xSemaphoreGive(stateMutex);
  }
  updateRelayHardware();
  if (state) {
    enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"relay_off\"}");
    Serial.println("[SmartNest] Shutdown ON — sending relay_off command to Digital Board");
  }
  pushWsUpdate();
}

bool getRelayState(int index) {
  if (index < 0 || index >= NUM_RELAYS)
    return false;

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
  bool stateChanged = false;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    for (int i = 0; i < NUM_RELAYS; i++) {
      bool D = sysState.dashboardStates[i];
      bool R = false;

      if (sysState.masterShutdown || sysState.masterLock ||
          sysState.lockedStates[i]) {
        R = false;
      } else {
        R = D;
      }

      if (sysState.relayStates[i] != R) {
        sysState.relayStates[i] = R;
        digitalWrite(RELAY_PINS[i], R ? LOW : HIGH); // LOW = ON, HIGH = OFF
        Serial.printf("[TIMING] SmartNest local relay GPIO written at: %lu\n",
                      millis());
        sysState.lastChangeMs = millis();
        stateChanged = true;

        RelayEvent evt;
        evt.index = i;
        evt.state = R;
        evt.timestamp = millis();
        xQueueSend(relayEventQueue, &evt, 0);
      }
    }
    xSemaphoreGive(stateMutex);
  }
  
  if (stateChanged) {
    pushWsUpdate();
  }
}

void relaySwitchTask(void *pvParameters) {
  relaySwitchInit();
  updateRelayHardware();
  Serial.println("[SmartNest] Relay hardware initialized. Task suspending "
                 "(event-driven mode).");
  // No polling loop — relay changes are applied immediately via
  // setRelayState()/updateRelayHardware() from HTTP API handlers, MQTT
  // callbacks, and UART telemetry processing.
  vTaskSuspend(
      NULL); // Suspend forever; task handle kept for potential future use
}

void currentSensorInit() {
  pinMode(ACS712_PIN, INPUT);
  analogSetAttenuation(ADC_11db);
  analogReadResolution(12);

  Serial.println("[SYSTEM] Calibrating ACS712 zero offset...");
  long sumMilliVolts = 0;
  for (int i = 0; i < 1000; i++) {
    sumMilliVolts += analogReadMilliVolts(ACS712_PIN);
    delay(2);
  }
  acs712ZeroMv = (float)sumMilliVolts / 1000.0f;
  Serial.print("[SYSTEM] Calibration completed. Zero-Offset: ");
  Serial.print(acs712ZeroMv);
  Serial.println(" mV");
}

void currentSensorTask(void *pvParameters) {
  currentSensorInit();

  TickType_t xLastWakeTime = xTaskGetTickCount();
  uint32_t lastProcessTime = millis();
  uint32_t lastWsPushTime = 0;
  
  static float lastSentAmps = 0.0f;
  static uint32_t lastSentTime = 0;
  static bool prevRelayStates[NUM_RELAYS] = {false};

  static double sqSum = 0.0;
  static uint32_t sampleCount = 0;

  while (true) {
    // Check if any local relays are ON and check for changes
    bool anyRelayOn = false;
    bool relayStateChanged = false;
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      for (int i = 0; i < NUM_RELAYS; i++) {
        if (sysState.relayStates[i]) {
          anyRelayOn = true;
        }
        if (sysState.relayStates[i] != prevRelayStates[i]) {
          relayStateChanged = true;
          prevRelayStates[i] = sysState.relayStates[i];
        }
      }
      xSemaphoreGive(stateMutex);
    }

    if (!anyRelayOn) {
      // Force current to 0.00A and clear accumulators
      float oldAmps = 0.0f;
      bool changed = false;
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        oldAmps = sysState.currentAmps;
        sysState.currentAmps = 0.0f;
        changed = (oldAmps != 0.0f);
        xSemaphoreGive(stateMutex);
      }
      sqSum = 0.0;
      sampleCount = 0;

      if (changed || relayStateChanged) {
        pushWsUpdate();
        
        // Immediately send 0.00A to Master
        uint32_t now = millis();
        lastSentTime = now;
        lastSentAmps = 0.0f;
        enqueueUartCmd("{\"t\":\"acs\",\"i\":0.00}");
      }
      // Sleep a bit longer when idle
      vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(10));
      continue;
    }

    // 1 kHz non-blocking sample
    float milliVolts = analogReadMilliVolts(ACS712_PIN);
    float deltaMv = (milliVolts - acs712ZeroMv) * ACS712_DIVIDER_RATIO;
    float instCurrent = deltaMv / 66.0f; // 66 mV/A sensitivity

    sqSum += (double)(instCurrent * instCurrent);
    sampleCount++;

    uint32_t now = millis();
    if (now - lastProcessTime >= 200) {
      lastProcessTime = now;
      if (sampleCount > 0) {
        float meanSquare = (float)(sqSum / (double)sampleCount);
        float rmsCurrent = sqrtf(meanSquare);
        float finalAmps = 0.0f;
        if (rmsCurrent >= 0.15f) { // 0.15A deadband filter
          finalAmps = rmsCurrent;
        }

        bool changedSignificantly = false;
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
          float diff = fabs(sysState.currentAmps - finalAmps);
          changedSignificantly = (diff >= 0.05f) || (sysState.currentAmps == 0.0f && finalAmps > 0.0f);
          sysState.currentAmps = finalAmps;
          xSemaphoreGive(stateMutex);
        }

        sqSum = 0.0;
        sampleCount = 0;

        // Push WebSocket update if changed significantly or at 1s interval
        if (changedSignificantly || (now - lastWsPushTime >= 1000)) {
          lastWsPushTime = now;
          pushWsUpdate();
        }

        // Send immediately to Master if force conditions met, or if changed significantly and rate-limited
        bool forceSend = relayStateChanged || 
                         (lastSentAmps == 0.0f && finalAmps > 0.0f) || 
                         (lastSentAmps > 0.0f && finalAmps == 0.0f);
        
        bool valueChanged = (fabs(finalAmps - lastSentAmps) >= 0.05f);
        
        if (forceSend || (valueChanged && (now - lastSentTime >= 500))) {
          lastSentTime = now;
          lastSentAmps = finalAmps;
          enqueueUartCmd("{\"t\":\"acs\",\"i\":" + String(finalAmps, 2) + "}");
        }
      }
    }

    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1));
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
    if (i > 0)
      json += ",";
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

      xTaskCreatePinnedToCore(wifiConnectAttemptTask, "ConnAttempt", 4096, NULL,
                              1, NULL, 0);

      req->send(200, "application/json", "{\"status\":\"connecting\"}");
    } else {
      req->send(400, "application/json",
                "{\"status\":\"error\",\"msg\":\"Missing SSID or password\"}");
    }
  });

  pProvServer->on("/connect-status", HTTP_GET, [](AsyncWebServerRequest *req) {
    String s;
    switch (connectStatus) {
    case 0:
      s = "idle";
      break;
    case 1:
      s = "connecting";
      break;
    case 2:
      s = "connected";
      break;
    case 3:
      s = "failed";
      break;
    default:
      s = "unknown";
    }
    if (connectStatus == 2) {
      req->send(200, "application/json",
                "{\"status\":\"connected\",\"ip\":\"" +
                    WiFi.localIP().toString() + "\"}");
    } else {
      req->send(200, "application/json", "{\"status\":\"" + s + "\"}");
    }
  });

  pProvServer->on("/generate_204", HTTP_GET,
                  [](AsyncWebServerRequest *r) { r->redirect("/"); });
  pProvServer->on("/hotspot-detect.html", HTTP_GET,
                  [](AsyncWebServerRequest *r) { r->redirect("/"); });
  pProvServer->on("/connecttest.txt", HTTP_GET,
                  [](AsyncWebServerRequest *r) { r->redirect("/"); });
  pProvServer->on("/fwlink", HTTP_GET,
                  [](AsyncWebServerRequest *r) { r->redirect("/"); });

  pProvServer->onNotFound(
      [](AsyncWebServerRequest *req) { req->redirect("/"); });
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
        strncpy(sysState.wifiSSID, savedSSID.c_str(),
                sizeof(sysState.wifiSSID) - 1);
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
    if (isProvisioningMode)
      continue;

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
  if (ws.count() == 0)
    return;
  String payload = buildStatusJSON();
  ws.textAll(payload);
}

// MQTT Callback and publishing helpers
#if MQTT_ENABLED
int getRelayIndexFromTopic(const String &topic) {
  String prefix = String(g_mqttConfig.baseTopic) + "/relay/";
  int prefixLen = prefix.length();
  if (!topic.startsWith(prefix))
    return -1;
  int nextSlash = topic.indexOf('/', prefixLen);
  if (nextSlash < 0)
    return -1;
  String idxStr = topic.substring(prefixLen, nextSlash);
  return idxStr.toInt();
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String t = String(topic);
  String p = String((char *)payload, length);
  p.trim();

  String base = String(g_mqttConfig.baseTopic);
  String relayPrefix = base + "/relay/";

  // Relay set: {baseTopic}/relay/N/set
  if (t.startsWith(relayPrefix) && t.endsWith("/set")) {
    int idx = getRelayIndexFromTopic(t);
    if (idx >= 0 && idx < NUM_RELAYS) {
      setRelayState(idx, p == "true");
    } else if (idx == 6) {
      enqueueUartCmd(String("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"") +
                     (p == "true" ? "relay_on" : "relay_off") + "\"}");
    }
  }
  // Relay lock: {baseTopic}/relay/N/lock
  else if (t.startsWith(relayPrefix) && t.endsWith("/lock")) {
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
  else if (t == base + "/cmd/shutdown") {
    setShutdownState(p == "true");
  }
  // Slave commands
  else if (t == base + "/cmd/slave/d1") {
    enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"" + p + "\"}");
  } else if (t == base + "/cmd/slave/pzem") {
    enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"pzem\",\"cmd\":\"" + p + "\"}");
  }

  pushWsUpdate();

  // Republish changed retained state
  if (t.startsWith(relayPrefix)) {
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
        mqttClient.publish((relayPrefix + String(idx) + "/state").c_str(),
                           stateVal ? "true" : "false", true);
        mqttClient.publish((relayPrefix + String(idx) + "/locked").c_str(),
                           lockedVal ? "true" : "false", true);
      } else if (idx == 6) {
        mqttClient.publish((relayPrefix + "6/state").c_str(),
                           stateVal ? "true" : "false", true);
        mqttClient.publish((relayPrefix + "6/locked").c_str(),
                           lockedVal ? "true" : "false", true);
      }
    }
  } else if (t == base + "/cmd/shutdown") {
    bool shutdownVal = false;
    bool gotMutex = false;
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      shutdownVal = sysState.masterShutdown;
      gotMutex = true;
      xSemaphoreGive(stateMutex);
    }
    if (gotMutex) {
      mqttClient.publish((base + "/shutdown").c_str(),
                         shutdownVal ? "true" : "false", true);
    }
  }
}

void publishAllRetainedStates() {
  String base = String(g_mqttConfig.baseTopic);
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
      mqttClient.publish((base + "/relay/" + String(i) + "/state").c_str(),
                         localRelays[i] ? "true" : "false", true);
      mqttClient.publish((base + "/relay/" + String(i) + "/locked").c_str(),
                         localLocks[i] ? "true" : "false", true);
    }
    mqttClient.publish((base + "/relay/6/state").c_str(),
                       localDigitalRelay ? "true" : "false", true);
    mqttClient.publish((base + "/relay/6/locked").c_str(),
                       localDigitalLocked ? "true" : "false", true);
    mqttClient.publish((base + "/shutdown").c_str(),
                       localShutdown ? "true" : "false", true);
  }
}

void publishTelemetry() {
  String base = String(g_mqttConfig.baseTopic);
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    mqttClient.publish((base + "/sensor/voltage").c_str(),
                       String(sysState.pzemVoltage, 1).c_str(), false);
    mqttClient.publish((base + "/sensor/acs").c_str(),
                       String(sysState.acsCurrentA, 2).c_str(), false);
    mqttClient.publish((base + "/sensor/load").c_str(),
                       String(sysState.currentAmps, 2).c_str(), false);
    mqttClient.publish((base + "/sensor/power").c_str(),
                       String(sysState.pzemPowerW, 1).c_str(), false);

    mqttClient.publish((base + "/energy/daily").c_str(),
                       String(sysState.energyDailyWh / 1000.0, 3).c_str(),
                       false);
    mqttClient.publish((base + "/energy/monthly").c_str(),
                       String(sysState.energyMonthlyWh / 1000.0, 3).c_str(),
                       false);
    mqttClient.publish((base + "/energy/lifetime").c_str(),
                       String(sysState.energyLifetimeWh / 1000.0, 3).c_str(),
                       false);

    mqttClient.publish((base + "/slave/d1/online").c_str(),
                       sysState.digitalSlaveOnline ? "true" : "false", true);
    mqttClient.publish((base + "/slave/pzem/online").c_str(),
                       sysState.pzemSlaveOnline ? "true" : "false", true);

    mqttClient.publish((base + "/switch/6/state").c_str(),
                       sysState.digitalSwitchState ? "true" : "false", false);

    // Heartbeat payload
    String heartbeat = "{\"uptime\":" + String(millis()) + ",\"ssid\":\"" +
                       String(sysState.wifiSSID) +
                       "\",\"rssi\":" + String(sysState.wifiRSSI) + "}";
    mqttClient.publish((base + "/status").c_str(), heartbeat.c_str(), false);

    xSemaphoreGive(stateMutex);
  }
}

void mqttTask(void *pvParameters) {
  mqttClient.setServer(g_mqttConfig.broker, g_mqttConfig.port);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(g_mqttConfig.keepAlive);

  uint32_t lastReconnectAttempt = 0;
  uint32_t lastHeartbeat = 0;
  bool wasMqttConnected = false;

  while (true) {
    // Detect dynamic config changes and re-initialize
    if (g_mqttConfigChanged) {
      g_mqttConfigChanged = false;
      Serial.println("[MQTT] Config changed — disconnecting for re-init");
      if (mqttClient.connected()) {
        mqttClient.disconnect();
      }
      mqttClient.setServer(g_mqttConfig.broker, g_mqttConfig.port);
      mqttClient.setKeepAlive(g_mqttConfig.keepAlive);
      wasMqttConnected = false;
      lastReconnectAttempt = 0;
    }

    // Skip MQTT processing if disabled
    if (!g_mqttConfig.enabled) {
      if (wasMqttConnected) {
        mqttClient.disconnect();
        enqueueUartCmd("{\"t\":\"cloud\",\"up\":false}");
        wasMqttConnected = false;
      }
      // Update MQTT status
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        sysState.mqttStatus = 0; // 0 = Disabled
        xSemaphoreGive(stateMutex);
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (WiFi.status() == WL_CONNECTED) {
      if (!mqttClient.connected()) {
        if (wasMqttConnected) {
          enqueueUartCmd("{\"t\":\"cloud\",\"up\":false}");
          wasMqttConnected = false;
        }
        // Update MQTT status
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
          sysState.mqttStatus = 1; // 1 = Connecting
          xSemaphoreGive(stateMutex);
        }
        if (millis() - lastReconnectAttempt > 5000) {
          lastReconnectAttempt = millis();
          bool ok;
          if (strlen(g_mqttConfig.username) > 0)
            ok =
                mqttClient.connect(g_mqttConfig.clientId, g_mqttConfig.username,
                                   g_mqttConfig.password);
          else
            ok = mqttClient.connect(g_mqttConfig.clientId);
          if (ok) {
            String base = String(g_mqttConfig.baseTopic);
            mqttClient.subscribe((base + "/relay/+/set").c_str());
            mqttClient.subscribe((base + "/relay/+/lock").c_str());
            mqttClient.subscribe((base + "/cmd/shutdown").c_str());
            mqttClient.subscribe((base + "/cmd/slave/d1").c_str());
            mqttClient.subscribe((base + "/cmd/slave/pzem").c_str());

            publishAllRetainedStates();
            enqueueUartCmd("{\"t\":\"cloud\",\"up\":true}");
            wasMqttConnected = true;
            Serial.printf("[MQTT] Connected to %s:%d (topic: %s)\n",
                          g_mqttConfig.broker, g_mqttConfig.port,
                          g_mqttConfig.baseTopic);
            // Update MQTT status
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
              sysState.mqttStatus = 2; // 2 = Connected
              xSemaphoreGive(stateMutex);
            }
          } else {
            // Update MQTT status
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
              sysState.mqttStatus = 3; // 3 = Error
              xSemaphoreGive(stateMutex);
            }
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
    if (pendingMasterLockSync && (millis() - pendingMasterLockSyncTime > 3000)) {
      Serial.println("[LOCK] pendingMasterLockSync timed out. Resetting flag.");
      pendingMasterLockSync = false;
    }

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

                // Parse SD card stats + masterLock from Master
                sysState.sdOk = doc["sd_ok"];
                sysState.sdTotal = doc["sd_total"];
                sysState.sdUsed = doc["sd_used"];
                
                if (doc.containsKey("m_lock")) {
                  bool newMLock = doc["m_lock"];
                  bool currentMLock = sysState.masterLock;
                  Serial.printf("[LOCK] Telemetry m_lock received: %s\n", newMLock ? "ON" : "OFF");
                  if (newMLock != currentMLock) {
                    if (pendingMasterLockSync) {
                      Serial.println("[LOCK] Ignored stale/unexpected lock overwrite");
                    } else {
                      sysState.masterLock = newMLock;
                    }
                  }
                }
                
                sysState.pzemSensorHealthy = doc["p_health"] | false;
                xSemaphoreGive(stateMutex);
              }

              // Non-spamming periodic 5s log prefix [SmartNest]
              static uint32_t lastTelLogTime = 0;
              if (millis() - lastTelLogTime >= 5000) {
                lastTelLogTime = millis();
                Serial.printf("[SmartNest] Telemetry update — V: %.1fV, Load: "
                              "%.2fA, SD: %s, MasterLock: %s\n",
                              sysState.pzemVoltage, sysState.currentAmps,
                              sysState.sdOk ? "OK" : "ERROR",
                              sysState.masterLock ? "LOCKED" : "UNLOCKED");
              }

              pushWsUpdate();
#if MQTT_ENABLED
              if (mqttClient.connected()) {
                publishTelemetry();
              }
#endif
            } else if (type == "sw") {
              int idx = doc["idx"];
              int s = doc["s"];
              Serial.printf("[SmartNest][SW] Manual switch event received: idx=%d, state=%d, dashboard updated\n", idx, s);
              if (idx >= 0 && idx < NUM_RELAYS) {
                setRelayState(idx, s == 1);
                pushWsUpdate();
              }
            } else if (type == "lock_ack") {
              bool val = doc["val"];
              Serial.printf("[LOCK] Master lock ACK received: %s\n", val ? "ON" : "OFF");
              if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                sysState.masterLock = val;
                pendingMasterLockSync = false;
                xSemaphoreGive(stateMutex);
              }
              updateRelayHardware();
              pushWsUpdate();
            } else if (type == "off") {
              String dev = doc["dev"];
              if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                if (dev == "d1")
                  sysState.digitalSlaveOnline = false;
                else if (dev == "pzem")
                  sysState.pzemSlaveOnline = false;
                xSemaphoreGive(stateMutex);
              }
              pushWsUpdate();
#if MQTT_ENABLED
              if (mqttClient.connected()) {
                mqttClient.publish(
                    (MQTT_BASE_TOPIC "/slave/" + dev + "/online").c_str(),
                    "false", true);
              }
#endif
            } else if (type == "on") {
              String dev = doc["dev"];
              if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                if (dev == "d1")
                  sysState.digitalSlaveOnline = true;
                else if (dev == "pzem")
                  sysState.pzemSlaveOnline = true;
                xSemaphoreGive(stateMutex);
              }
              pushWsUpdate();
#if MQTT_ENABLED
              if (mqttClient.connected()) {
                mqttClient.publish(
                    (MQTT_BASE_TOPIC "/slave/" + dev + "/online").c_str(),
                    "true", true);
              }
#endif
            } else if (type == "hist") {
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

                  String recJson = "{\"epoch\":" + String(ep) +
                                   ",\"v\":" + String(v, 1) +
                                   ",\"load\":" + String(load, 2) +
                                   ",\"pi\":" + String(pi, 3) +
                                   ",\"powerVA\":" + String(pva, 1) + "}";

                  bool pubOk = mqttClient.publish(MQTT_BASE_TOPIC "/history",
                                                  recJson.c_str(), false);
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
                enqueueUartCmd(
                    "{\"t\":\"hist_ack\",\"batch\":" + String(batch) +
                    ",\"upto\":" + String(lastEpoch) + "}");
              }
            } else if (type == "files_res" || type == "read_res") {
              Serial.printf("[SmartNest] Proxy-broadcasting %s WebSocket "
                            "update (%d bytes)\n",
                            type.c_str(), rxBuffer.length());
              ws.textAll(rxBuffer);
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
        String msg = "{\"t\":\"ntp\",\"epoch\":" + String((uint32_t)nowSecs) +
                     ",\"tz_h\":5,\"tz_m\":30}";
        enqueueUartCmd(msg);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(300000)); // Every 5 minutes
  }
}

static AsyncWebServer dashServer(80);

#include "dashboard_html.h"

String getFormattedTime() {
  time_t nowSecs;
  if (time(&nowSecs) < 1000) {
    return "N/A";
  }
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) {
    return "N/A";
  }
  char timeBuf[32];
  strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(timeBuf);
}

static String buildStatusJSON() {
  String json = "{";
  json += "\"relays\":[";
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    for (int i = 0; i < NUM_RELAYS; i++) {
      if (i > 0)
        json += ",";
      json += sysState.relayStates[i] ? "true" : "false";
    }
    json += "],";
    json += "\"locks\":[";
    for (int i = 0; i < NUM_RELAYS; i++) {
      if (i > 0)
        json += ",";
      json += sysState.lockedStates[i] ? "true" : "false";
    }
    json += "],";
    json +=
        "\"shutdown\":" + String(sysState.masterShutdown ? "true" : "false") +
        ",";
    json += "\"current\":" + String(sysState.currentAmps, 2) + ",";
    json += "\"rssi\":" + String(sysState.wifiRSSI) + ",";
    json += "\"ssid\":\"" + String(sysState.wifiSSID) + "\",";

    // masters / slaves data fields
    json += "\"acs\":" + String(sysState.acsCurrentA, 2) + ",";
    json += "\"load\":" + String(sysState.currentAmps, 2) + ",";
    json += "\"v\":" + String(sysState.pzemVoltage, 1) + ",";
    json += "\"pw\":" + String(sysState.pzemPowerW, 1) + ",";
    json += "\"daily\":" + String(sysState.energyDailyWh / 1000.0, 3) + ",";
    json += "\"monthly\":" + String(sysState.energyMonthlyWh / 1000.0, 3) + ",";
    json +=
        "\"lifetime\":" + String(sysState.energyLifetimeWh / 1000.0, 3) + ",";
    json +=
        "\"d_on\":" + String(sysState.digitalSlaveOnline ? "true" : "false") +
        ",";
    json +=
        "\"p_on\":" + String(sysState.pzemSlaveOnline ? "true" : "false") + ",";
    json += "\"d_rssi\":" + String(sysState.digitalSlaveRSSI) + ",";
    json += "\"p_rssi\":" + String(sysState.pzemSlaveRSSI) + ",";
    json +=
        "\"d_relay\":" + String(sysState.digitalRelayState ? "true" : "false") +
        ",";
    json +=
        "\"d_lock\":" + String(sysState.digitalRelayLocked ? "true" : "false") +
        ",";
    json +=
        "\"d_sw\":" + String(sysState.digitalSwitchState ? "true" : "false") +
        ",";

    // masterLock + SD stats
    json +=
        "\"m_lock\":" + String(sysState.masterLock ? "true" : "false") + ",";
    json += "\"sd_ok\":" + String(sysState.sdOk ? "true" : "false") + ",";
    json +=
        "\"sd_total\":" + String((unsigned long long)sysState.sdTotal) + ",";
    json += "\"sd_used\":" + String((unsigned long long)sysState.sdUsed) + ",";
    json += "\"p_health\":" +
            String(sysState.pzemSensorHealthy ? "true" : "false") + ",";
    json += "\"mqtt_status\":" + String(sysState.mqttStatus);

    xSemaphoreGive(stateMutex);
  } else {
    json += "],\"locks\":[false,false,false,false,false,false],\"shutdown\":"
            "false,\"current\":0,\"rssi\":0,\"ssid\":\"\",";
    json += "\"acs\":0.00,\"load\":0.00,\"v\":0.0,\"pw\":0.0,\"daily\":0.000,"
            "\"monthly\":0.000,\"lifetime\":0.000,\"d_on\":false,\"p_on\":"
            "false,\"d_rssi\":0,\"p_rssi\":0,\"d_relay\":false,\"d_lock\":"
            "false,\"d_sw\":false,";
    json += "\"m_lock\":false,\"sd_ok\":false,\"sd_total\":0,\"sd_used\":0,\"p_"
            "health\":false,\"mqtt_status\":0";
  }
  json += ",\"uptime\":" + String(millis());
  json += ",\"time\":\"" + getFormattedTime() + "\"";
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
      "/api/relay", HTTP_POST, [](AsyncWebServerRequest *req) {}, NULL,
      [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index,
         size_t total) {
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
          pushWsUpdate();
          bool actualState = getRelayState(relayIdx);
          req->send(200, "application/json",
                    "{\"success\":true,\"relay\":" + String(relayIdx) +
                        ",\"state\":" + (actualState ? "true" : "false") + "}");
        } else if (relayIdx == 6) {
          bool actualState = false;
          bool lockActive = false;
          if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            lockActive = sysState.masterLock || sysState.digitalRelayLocked || sysState.masterShutdown;
            actualState = sysState.digitalRelayState;
            xSemaphoreGive(stateMutex);
          }
          if (state && lockActive) {
            Serial.println("[LOCK] /api/relay (6, ON) blocked locally");
            req->send(200, "application/json", "{\"success\":true,\"relay\":6,\"state\":" + String(actualState ? "true" : "false") + "}");
          } else {
            String cmd = state ? "relay_on" : "relay_off";
            enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"" + cmd + "\"}");
            req->send(200, "application/json",
                      "{\"success\":true,\"relay\":6,\"state\":" +
                          String(state ? "true" : "false") + "}");
          }
        } else {
          req->send(400, "application/json", "{\"error\":\"Invalid index\"}");
        }
      });

  dashServer.on(
      "/api/lock", HTTP_POST, [](AsyncWebServerRequest *req) {}, NULL,
      [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index,
         size_t total) {
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
          pushWsUpdate();
          req->send(200, "application/json",
                    "{\"success\":true,\"relay\":" + String(relayIdx) +
                        ",\"locked\":" + (state ? "true" : "false") + "}");
        } else if (relayIdx == 6) {
          String cmd = state ? "relay_lock" : "relay_unlock";
          enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"" + cmd +
                         "\"}");
          req->send(200, "application/json",
                    "{\"success\":true,\"relay\":6,\"locked\":" +
                        String(state ? "true" : "false") + "}");
        } else {
          req->send(400, "application/json", "{\"error\":\"Invalid index\"}");
        }
      });

  dashServer.on(
      "/api/shutdown", HTTP_POST, [](AsyncWebServerRequest *req) {}, NULL,
      [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index,
         size_t total) {
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

        setShutdownState(state);
        req->send(200, "application/json",
                  "{\"success\":true,\"shutdown\":" +
                      String(state ? "true" : "false") + "}");
      });

  // Post handler for master lock setting
  dashServer.on(
      "/api/master-lock", HTTP_POST, [](AsyncWebServerRequest *req) {}, NULL,
      [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index,
         size_t total) {
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

        Serial.printf("[LOCK] Dashboard requested Global Master Lock: %s\n", state ? "ON" : "OFF");
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
          sysState.masterLock = state;
          pendingMasterLockSync = true;
          pendingMasterLockSyncTime = millis();
          xSemaphoreGive(stateMutex);
        }
        Serial.printf("[LOCK] Local masterLock set: %s\n", state ? "ON" : "OFF");
        updateRelayHardware();

        // Send master lock status change command to Master
        enqueueUartCmd("{\"t\":\"lock\",\"val\":" +
                       String(state ? "true" : "false") + "}");
        Serial.println("[LOCK] UART lock command sent to Master");

        pushWsUpdate();
        req->send(200, "application/json",
                  "{\"success\":true,\"state\":" +
                      String(state ? "true" : "false") + "}");
      });

  // Post handler for factory reset
  dashServer.on(
      "/api/factory-reset", HTTP_POST, [](AsyncWebServerRequest *req) {}, NULL,
      [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index,
         size_t total) {
        Serial.println("[SmartNest] FACTORY RESET requested — wiping WiFi "
                       "credentials and notifying Master...");
        // Send reset command to Master
        enqueueUartCmd("{\"t\":\"factory_reset\"}");

        req->send(200, "application/json",
                  "{\"success\":true,\"msg\":\"Factory reset initiated. Device "
                  "is resetting...\"}");

        // Wipe NVS WiFi credentials and reboot
        delay(1000);
        Preferences prefs;
        prefs.begin(NVS_NAMESPACE, false);
        prefs.clear();
        prefs.end();
        delay(1000);
        ESP.restart();
      });

  // GET handler to request directory listing of SD card logs
  dashServer.on("/api/logs/list", HTTP_GET, [](AsyncWebServerRequest *req) {
    Serial.println(
        "[SmartNest] GET /api/logs/list — requesting directory from Master");
    enqueueUartCmd("{\"t\":\"files_req\"}");
    req->send(200, "application/json",
              "{\"success\":true,\"msg\":\"Log list requested\"}");
  });

  // GET handler to request chunk contents of a specific log file
  dashServer.on("/api/logs/view", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("file") && req->hasParam("chunk")) {
      String fileVal = req->getParam("file")->value();
      int chunkVal = req->getParam("chunk")->value().toInt();
      Serial.printf(
          "[SmartNest] GET /api/logs/view — requesting file: %s chunk: %d\n",
          fileVal.c_str(), chunkVal);

      enqueueUartCmd("{\"t\":\"read_req\",\"file\":\"" + fileVal +
                     "\",\"chunk\":" + String(chunkVal) + "}");
      req->send(200, "application/json",
                "{\"success\":true,\"msg\":\"Log chunk requested\"}");
    } else {
      req->send(400, "application/json",
                "{\"error\":\"Missing file or chunk parameter\"}");
    }
  });

  // Post handler for slave commands from the web UI
  dashServer.on(
      "/api/slave-cmd", HTTP_POST, [](AsyncWebServerRequest *req) {}, NULL,
      [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index,
         size_t total) {
        StaticJsonDocument<256> doc;
        DeserializationError err = deserializeJson(doc, data, len);
        if (err) {
          req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
          return;
        }
        String target = doc["target"];
        String cmd = doc["cmd"];

        if ((target == "d1" &&
             (cmd == "relay_on" || cmd == "relay_off" || cmd == "relay_lock" ||
              cmd == "relay_unlock" || cmd == "reboot")) ||
            (target == "pzem" && (cmd == "reboot" || cmd == "energy_reset"))) {
          enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"" + target +
                         "\",\"cmd\":\"" + cmd + "\"}");
          req->send(200, "application/json", "{\"success\":true}");
        } else {
          req->send(400, "application/json",
                    "{\"error\":\"Invalid target or command\"}");
        }
      });

  // --- MQTT Configuration API Endpoints ---
  dashServer.on("/api/mqtt/config", HTTP_GET, [](AsyncWebServerRequest *req) {
    StaticJsonDocument<512> doc;
    doc["enabled"] = g_mqttConfig.enabled;
    doc["broker"] = g_mqttConfig.broker;
    doc["port"] = g_mqttConfig.port;
    doc["clientId"] = g_mqttConfig.clientId;
    doc["username"] = g_mqttConfig.username;
    doc["password"] = strlen(g_mqttConfig.password) > 0 ? "********" : "";
    doc["baseTopic"] = g_mqttConfig.baseTopic;
    doc["keepAlive"] = g_mqttConfig.keepAlive;
    String output;
    serializeJson(doc, output);
    req->send(200, "application/json", output);
  });

  dashServer.on(
      "/api/mqtt/config", HTTP_POST, [](AsyncWebServerRequest *req) {}, NULL,
      [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index,
         size_t total) {
        StaticJsonDocument<512> doc;
        DeserializationError err = deserializeJson(doc, data, len);
        if (err) {
          req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
          return;
        }
        if (doc.containsKey("enabled"))
          g_mqttConfig.enabled = doc["enabled"];
        if (doc.containsKey("broker")) {
          strncpy(g_mqttConfig.broker, doc["broker"] | "",
                  sizeof(g_mqttConfig.broker) - 1);
          g_mqttConfig.broker[sizeof(g_mqttConfig.broker) - 1] = '\0';
        }
        if (doc.containsKey("port"))
          g_mqttConfig.port = doc["port"];
        if (doc.containsKey("clientId")) {
          strncpy(g_mqttConfig.clientId, doc["clientId"] | "",
                  sizeof(g_mqttConfig.clientId) - 1);
          g_mqttConfig.clientId[sizeof(g_mqttConfig.clientId) - 1] = '\0';
        }
        if (doc.containsKey("username")) {
          strncpy(g_mqttConfig.username, doc["username"] | "",
                  sizeof(g_mqttConfig.username) - 1);
          g_mqttConfig.username[sizeof(g_mqttConfig.username) - 1] = '\0';
        }
        if (doc.containsKey("password") &&
            String(doc["password"] | "") != "********") {
          strncpy(g_mqttConfig.password, doc["password"] | "",
                  sizeof(g_mqttConfig.password) - 1);
          g_mqttConfig.password[sizeof(g_mqttConfig.password) - 1] = '\0';
        }
        if (doc.containsKey("baseTopic")) {
          strncpy(g_mqttConfig.baseTopic, doc["baseTopic"] | "",
                  sizeof(g_mqttConfig.baseTopic) - 1);
          g_mqttConfig.baseTopic[sizeof(g_mqttConfig.baseTopic) - 1] = '\0';
        }
        if (doc.containsKey("keepAlive"))
          g_mqttConfig.keepAlive = doc["keepAlive"];
        saveMqttConfig();
        g_mqttConfigChanged = true;
        Serial.println("[MQTT] Config updated via API");
        req->send(200, "application/json",
                  "{\"success\":true,\"msg\":\"MQTT config saved. "
                  "Reconnecting...\"}");
      });

  dashServer.on(
      "/api/mqtt/test", HTTP_POST, [](AsyncWebServerRequest *req) {}, NULL,
      [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index,
         size_t total) {
        WiFiClient testClient;
        PubSubClient testMqtt(testClient);
        testMqtt.setServer(g_mqttConfig.broker, g_mqttConfig.port);
        bool ok;
        if (strlen(g_mqttConfig.username) > 0)
          ok = testMqtt.connect("SmartNest_test", g_mqttConfig.username,
                                g_mqttConfig.password);
        else
          ok = testMqtt.connect("SmartNest_test");
        if (ok) {
          testMqtt.disconnect();
          req->send(200, "application/json",
                    "{\"success\":true,\"msg\":\"Connection test passed!\"}");
        } else {
          req->send(200, "application/json",
                    "{\"success\":false,\"msg\":\"Connection failed. Check "
                    "broker/credentials.\"}");
        }
      });

  dashServer.on(
      "/api/mqtt/reset-default", HTTP_POST, [](AsyncWebServerRequest *req) {
        resetMqttConfigToDefault();
        g_mqttConfigChanged = true;
        req->send(
            200, "application/json",
            "{\"success\":true,\"msg\":\"MQTT config reset to defaults\"}");
      });

  // --- Modular Reset API Endpoints ---
  dashServer.on("/api/reset/wifi", HTTP_POST, [](AsyncWebServerRequest *req) {
    Serial.println("[SmartNest] WiFi reset requested");
    req->send(200, "application/json",
              "{\"success\":true,\"msg\":\"WiFi credentials cleared. "
              "Rebooting...\"}");
    delay(500);
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.clear();
    prefs.end();
    delay(500);
    ESP.restart();
  });

  dashServer.on("/api/reset/mqtt", HTTP_POST, [](AsyncWebServerRequest *req) {
    Serial.println("[SmartNest] MQTT config reset requested");
    resetMqttConfigToDefault();
    g_mqttConfigChanged = true;
    req->send(200, "application/json",
              "{\"success\":true,\"msg\":\"MQTT config reset to defaults\"}");
  });

  dashServer.on("/api/reset/energy", HTTP_POST, [](AsyncWebServerRequest *req) {
    Serial.println("[SmartNest] Energy counter reset requested");
    enqueueUartCmd("{\"t\":\"factory_reset\"}");
    req->send(
        200, "application/json",
        "{\"success\":true,\"msg\":\"Energy counters reset sent to Master\"}");
  });

  dashServer.on("/api/reset/logs", HTTP_POST, [](AsyncWebServerRequest *req) {
    Serial.println("[SmartNest] SD log clear requested");
    enqueueUartCmd("{\"t\":\"clear_logs\"}");
    req->send(200, "application/json",
              "{\"success\":true,\"msg\":\"SD log clear sent to Master\"}");
  });

  dashServer.on("/api/reset/full", HTTP_POST, [](AsyncWebServerRequest *req) {
    Serial.println("[SmartNest] FULL factory reset requested");
    enqueueUartCmd("{\"t\":\"factory_reset\"}");
    enqueueUartCmd("{\"t\":\"clear_logs\"}");
    resetMqttConfigToDefault();
    req->send(
        200, "application/json",
        "{\"success\":true,\"msg\":\"Full reset initiated. Rebooting...\"}");
    delay(1000);
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.clear();
    prefs.end();
    delay(500);
    ESP.restart();
  });

  dashServer.onNotFound([](AsyncWebServerRequest *req) { req->redirect("/"); });

  // Register WebSocket server
  ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client,
                AwsEventType type, void *arg, uint8_t *data, size_t len) {});
  dashServer.addHandler(&ws);

  dashServer.begin();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  memset(&sysState, 0, sizeof(SystemState));
  loadMqttConfig(); // Load MQTT config from NVS Preferences

  relayEventQueue = xQueueCreate(10, sizeof(RelayEvent));
  stateMutex = xSemaphoreCreateMutex();
  UartCmdQueue = xQueueCreate(5, sizeof(UartCmdItem));

  if (!relayEventQueue || !stateMutex || !UartCmdQueue) {
    delay(1000);
    ESP.restart();
  }

  xTaskCreatePinnedToCore(relaySwitchTask, "RelaySwitch", 4096, NULL, 2,
                          &hRelaySwitch, 1);

  xTaskCreatePinnedToCore(currentSensorTask, "CurrentSensor", 4096, NULL, 1,
                          &hCurrentSensor, 1);

  xTaskCreatePinnedToCore(resetButtonTask, "ResetBtn", 2048, NULL, 1,
                          &hResetButton, 0);

  bool wifiConnected = wifiManagerInit();

  if (wifiConnected) {
    dashboardInit();

    xTaskCreatePinnedToCore(uartCommTask, "UartComm", 4096, NULL, 1, &hUartComm,
                            0);

    xTaskCreatePinnedToCore(wifiReconnectTask, "WifiRecon", 4096, NULL, 1,
                            &hWifiReconnect, 0);

    xTaskCreatePinnedToCore(ntpSyncTask, "NtpSync", 4096, NULL, 1, &hNtpSync,
                            0);

#if MQTT_ENABLED
    xTaskCreatePinnedToCore(mqttTask, "MqttTask", 8192, NULL, 1, &hMqttTask, 0);
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
