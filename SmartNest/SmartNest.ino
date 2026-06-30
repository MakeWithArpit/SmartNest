#include <Arduino.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <HardwareSerial.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "dashboard_html.h"

#define NUM_RELAYS 6

#define RELAY_1_PIN 2
#define RELAY_2_PIN 4
#define RELAY_3_PIN 5
#define RELAY_4_PIN 19
#define RELAY_5_PIN 18
#define RELAY_6_PIN 15

#define ACS712_PIN 34
static float acs712ZeroMv = 2500.0f;

#define DHT_PIN 23
#define DHT_TYPE DHT11

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
#define CTRL_NVS_NAMESPACE "relay_ctrl"
#define UART_RX_BUFFER_MAX 6144

#define MQTT_ENABLED true
#define MQTT_BROKER_HOST "broker.hivemq.com"
#define MQTT_BROKER_PORT 1883
#define MQTT_CLIENT_ID "SmartNest_001"
#define MQTT_USERNAME ""
#define MQTT_PASSWORD ""
#define MQTT_KEEPALIVE_S 60
#define MQTT_BASE_TOPIC "smartnest/SmartNest_001"
#define MQTT_HISTORY_BATCH_LIMIT 6
#define MQTT_HISTORY_RETRY_MS 15000
#define MQTT_HISTORY_MAX_PAYLOAD_BYTES 2800
#define RESTORE_RELAYS_ON_BOOT true

// Set to 1 to enable verbose debug output on Serial Monitor
#define DEBUG_VERBOSE 0

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
static String g_pendingD1CmdId = "";
static String g_pendingD1CmdType = "";
static uint32_t g_pendingD1CmdMs = 0;
static bool g_historyPending = false;
static String g_historyPendingBatchId = "";
static uint32_t g_historyPendingLastId = 0;
static uint32_t g_historyLastAckedId = 0;
static uint32_t g_historyLastRequestMs = 0;
static uint8_t g_historyBatchLimit = MQTT_HISTORY_BATCH_LIMIT;
static String g_resetReason = "";

// CLEAR ENERGY LOGS command tracking (Serial Monitor only)
static volatile bool g_clearEnergyLogsSerialPending = false;

// ===== Dashboard globals =====
static AsyncWebServer* pDashServer = nullptr;
static String g_dashSessionToken = "";

// Credentials stored in NVS under namespace "dash_auth"
// keys: "dash_user" (default "admin"), "dash_pass" (default "smartnest")

// HTTP pending flags for async UART responses
static volatile bool g_sdLastRecordHttpPending = false;
static AsyncWebServerRequest* g_sdLastRecordHttpReq = nullptr;
static uint32_t g_sdLastRecordHttpStartMs = 0;

static volatile bool g_clearLogsHttpPending = false;
static AsyncWebServerRequest* g_clearLogsHttpReq = nullptr;
static uint32_t g_clearLogsHttpStartMs = 0;

// Slave status tracking for change-only serial and periodic MQTT
static bool g_prevDigitalOnline = false;
static bool g_prevPzemOnline = false;
static uint32_t g_lastSlavesMqttPublishMs = 0;
static uint32_t g_lastSlaveStatusRxMs = 0;
static uint32_t g_lastDigitalAgeSec = 0;
static uint32_t g_lastPzemAgeSec = 0;

void saveMqttConfig() {
  Preferences prefs;
  prefs.begin("mqtt_cfg", false);
  prefs.putBool("enabled", g_mqttConfig.enabled);
  prefs.putString("broker", String(g_mqttConfig.broker));
  prefs.putInt("port", g_mqttConfig.port);
  prefs.putString("username", String(g_mqttConfig.username));
  prefs.putString("password", String(g_mqttConfig.password));
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

  strncpy(g_mqttConfig.clientId, MQTT_CLIENT_ID,
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

  strncpy(g_mqttConfig.baseTopic, MQTT_BASE_TOPIC,
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

struct SystemState {
  bool relayStates[NUM_RELAYS];
  bool requestedRelayStates[NUM_RELAYS];
  bool lockedStates[NUM_RELAYS];
  bool wifiConnected;
  int wifiRSSI;
  char wifiSSID[33];
  unsigned long lastChangeMs;
  float currentAmps;

  float acsCurrentA;
  float pzemVoltage;
  float energyVoltage;
  float pzemCurrentA;
  float pzemPowerW;
  double acEnergyKWh;
  double pzemRawEnergyKWh;
  double acDayStartKWh;
  double mainEnergyKWh;
  double digitalEnergyKWh;
  uint32_t relayRuntimeSec[7];
  bool digitalSlaveOnline;
  bool pzemSlaveOnline;
  int digitalSlaveRSSI;
  int pzemSlaveRSSI;
  uint32_t digitalLastSeenAgeSec;
  uint32_t pzemLastSeenAgeSec;
  uint8_t digitalMissedHeartbeats;
  uint8_t pzemMissedHeartbeats;
  bool pzemHealthFromMaster;

  bool digitalRelayState;
  bool digitalRelayLocked;
  bool digitalSwitchState;

  bool sdOk;
  uint64_t sdTotal;
  uint64_t sdUsed;
  bool acPower;
  int acTempC;
  char acFan[8];

  bool pzemSensorHealthy;
  bool voltageEstimated;
  int mqttStatus;
  float temperatureC;
  float humidityPct;
  bool dhtHealthy;
  char resetReason[24];
  char timeSource[12];
};

const int RELAY_PINS[NUM_RELAYS] = {RELAY_1_PIN, RELAY_2_PIN, RELAY_3_PIN,
                                    RELAY_4_PIN, RELAY_5_PIN, RELAY_6_PIN};

SemaphoreHandle_t stateMutex = NULL;
SystemState sysState;
String wifiPassword = "";
volatile bool isProvisioningMode = false;

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
// Flag set by "SD LAST-RECORD" serial command; cleared when response arrives
static volatile bool g_sdLastRecordSerialPending = false;

#if MQTT_ENABLED
WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);
#endif

DHT dht(DHT_PIN, DHT_TYPE);

void setRelayState(int index, bool state);
void updateRelayHardware();
void loadLocalControlState();
void saveLocalControlState();
static void setLocalRelayLock(int index, bool locked);
void relaySwitchInit();
void currentSensorInit();
bool wifiManagerInit();
void wifiManagerLoop();
void resetWiFiCredentials();
void uartCommInit();
void serialCommandInit();
void serialCommandLoop();
static void handleSerialCommand(String cmd);
static String buildStatusJSON();
static void sendJson(AsyncWebServerRequest* request, int code,
                     const String& json);

void relaySwitchTask(void *pvParameters);
void currentSensorTask(void *pvParameters);
void resetButtonTask(void *pvParameters);
void wifiReconnectTask(void *pvParameters);
void uartCommTask(void *pvParameters);
void ntpSyncTask(void *pvParameters);
void mqttTask(void *pvParameters);
void dhtTask(void *pvParameters);
void publishTelemetry();
void publishLiveData();
void publishSlavesStatus();
void requestHistoryBatch(bool force);
static String jsonEscape(const char *value);
static String jsonEscape(const String &value);
static bool setMasterLock(bool state);
static const char *resetReasonToString(esp_reset_reason_t reason);
static void sendRelayMaskIfChanged(bool force = false);

void enqueueUartCmd(const String &cmd) {
  if (UartCmdQueue == NULL)
    return;
  UartCmdItem item;
  strncpy(item.text, cmd.c_str(), sizeof(item.text) - 1);
  item.text[sizeof(item.text) - 1] = '\0';
  xQueueSend(UartCmdQueue, &item, 0);
}

static const char *resetReasonToString(esp_reset_reason_t reason) {
  switch (reason) {
  case ESP_RST_POWERON:
    return "POWERON";
  case ESP_RST_EXT:
    return "EXT";
  case ESP_RST_SW:
    return "SW";
  case ESP_RST_PANIC:
    return "PANIC";
  case ESP_RST_INT_WDT:
    return "INT_WDT";
  case ESP_RST_TASK_WDT:
    return "TASK_WDT";
  case ESP_RST_WDT:
    return "WDT";
  case ESP_RST_DEEPSLEEP:
    return "DEEPSLEEP";
  case ESP_RST_BROWNOUT:
    return "BROWNOUT";
  case ESP_RST_SDIO:
    return "SDIO";
  default:
    return "UNKNOWN";
  }
}

void relaySwitchInit() {
  for (int i = 0; i < NUM_RELAYS; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], HIGH);
  }

  loadLocalControlState();
}

void loadLocalControlState() {
  bool savedRelays[NUM_RELAYS] = {false};
  bool savedLocks[NUM_RELAYS] = {false};
  bool hadLegacyMasterLock = false;
  bool brownoutReset = esp_reset_reason() == ESP_RST_BROWNOUT;

  Preferences prefs;
  prefs.begin(CTRL_NVS_NAMESPACE, true);
  for (int i = 0; i < NUM_RELAYS; i++) {
    savedRelays[i] = prefs.getBool((String("r") + String(i)).c_str(), false);
    savedLocks[i] = prefs.getBool((String("l") + String(i)).c_str(), false);
  }
  hadLegacyMasterLock = prefs.isKey("mLock") && prefs.getBool("mLock", false);
  prefs.end();

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    for (int i = 0; i < NUM_RELAYS; i++) {
      sysState.relayStates[i] = false;
      sysState.requestedRelayStates[i] =
          (!RESTORE_RELAYS_ON_BOOT || brownoutReset || savedLocks[i]) ? false : savedRelays[i];
      sysState.lockedStates[i] = savedLocks[i];
    }
    xSemaphoreGive(stateMutex);
  }

  uint8_t loadedMask = 0;
  for (int i = 0; i < NUM_RELAYS; i++) {
    if (RESTORE_RELAYS_ON_BOOT && !brownoutReset && !savedLocks[i] && savedRelays[i])
      loadedMask |= (1 << i);
  }
  if (brownoutReset) {
    Preferences safePrefs;
    safePrefs.begin(CTRL_NVS_NAMESPACE, false);
    for (int i = 0; i < NUM_RELAYS; i++) {
      safePrefs.putBool((String("r") + String(i)).c_str(), false);
    }
    safePrefs.end();
    Serial.println("[SAFEBOOT] Brownout detected, relay requested states cleared");
  }
  if (hadLegacyMasterLock) {
    Preferences cleanup;
    cleanup.begin(CTRL_NVS_NAMESPACE, false);
    cleanup.remove("mLock");
    cleanup.end();
    Serial.println("[SmartNest] Ignored and cleared legacy mLock NVS key");
  }
  Serial.printf("[SmartNest] Loaded local relay state from NVS. requestedMask=0x%02X\n",
                loadedMask);
}

void saveLocalControlState() {
  bool relays[NUM_RELAYS] = {false};
  bool locks[NUM_RELAYS] = {false};

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    for (int i = 0; i < NUM_RELAYS; i++) {
      relays[i] = sysState.requestedRelayStates[i];
      locks[i] = sysState.lockedStates[i];
    }
    xSemaphoreGive(stateMutex);
  } else {
    return;
  }

  Preferences prefs;
  prefs.begin(CTRL_NVS_NAMESPACE, false);
  for (int i = 0; i < NUM_RELAYS; i++) {
    prefs.putBool((String("r") + String(i)).c_str(), relays[i]);
    prefs.putBool((String("l") + String(i)).c_str(), locks[i]);
  }
  prefs.remove("mLock");
  prefs.end();
}

static void clearLocalControlState() {
  Preferences prefs;
  prefs.begin(CTRL_NVS_NAMESPACE, false);
  prefs.clear();
  prefs.end();
}

void setRelayState(int index, bool state) {
  if (index < 0 || index >= NUM_RELAYS)
    return;

  bool doUpdate = false;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (state && sysState.lockedStates[index]) {
      Serial.printf("[LOCK] Relay %d blocked\n", index + 1);
    } else {
      sysState.requestedRelayStates[index] = state;
      doUpdate = true;
    }
    xSemaphoreGive(stateMutex);
  }

  if (doUpdate) {
    saveLocalControlState();
    updateRelayHardware();
  }
}

static void setLocalRelayLock(int index, bool locked) {
  if (index < 0 || index >= NUM_RELAYS)
    return;

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    sysState.lockedStates[index] = locked;
    if (locked) {
      sysState.requestedRelayStates[index] = false;
    }
    xSemaphoreGive(stateMutex);
  }
}


static uint8_t currentRelayMaskSnapshot() {
  uint8_t relayMask = 0;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    for (int i = 0; i < NUM_RELAYS; i++) {
      if (sysState.relayStates[i])
        relayMask |= (1 << i);
    }
    xSemaphoreGive(stateMutex);
  }
  return relayMask;
}

static void sendRelayMaskIfChanged(bool force) {
  static bool initialized = false;
  static uint8_t lastMask = 0xFF;
  uint8_t relayMask = currentRelayMaskSnapshot();
  if (force || !initialized || relayMask != lastMask) {
    initialized = true;
    lastMask = relayMask;
    enqueueUartCmd("{\"t\":\"rel\",\"mask\":" + String(relayMask) + "}");
  }
}

void updateRelayHardware() {
  bool changed = false;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    for (int i = 0; i < NUM_RELAYS; i++) {
      bool requested = sysState.requestedRelayStates[i];
      bool actual = requested;

      if (sysState.lockedStates[i]) {
        actual = false;
      }

      if (sysState.relayStates[i] != actual) {
        sysState.relayStates[i] = actual;
        digitalWrite(RELAY_PINS[i], actual ? LOW : HIGH);
        Serial.printf("[SmartNest] Relay %d set %s\n", i + 1,
                      actual ? "ON" : "OFF");
        sysState.lastChangeMs = millis();
        changed = true;

      }
    }
    xSemaphoreGive(stateMutex);
  }
  if (changed) {
    sendRelayMaskIfChanged(false);
  }
}
void relaySwitchTask(void *pvParameters) {
  relaySwitchInit();
  updateRelayHardware();
  sendRelayMaskIfChanged(true);
  Serial.println("[SmartNest] Relay hardware initialized.");
  vTaskSuspend(NULL);
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
  
  static float lastSentAmps = 0.0f;
  static uint32_t lastSentTime = 0;
  static bool prevRelayStates[NUM_RELAYS] = {false};

  static double sqSum = 0.0;
  static uint32_t sampleCount = 0;

  while (true) {
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
        uint32_t now = millis();
        lastSentTime = now;
        lastSentAmps = 0.0f;
        enqueueUartCmd("{\"t\":\"acs\",\"i\":0.00}");
      }
      vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(10));
      continue;
    }

    float milliVolts = analogReadMilliVolts(ACS712_PIN);
    float deltaMv = (milliVolts - acs712ZeroMv);
    float instCurrent = deltaMv / 66.0f;

    sqSum += (double)(instCurrent * instCurrent);
    sampleCount++;

    uint32_t now = millis();
    if (now - lastProcessTime >= 200) {
      lastProcessTime = now;
      if (sampleCount > 0) {
        float meanSquare = (float)(sqSum / (double)sampleCount);
        float rmsCurrent = sqrtf(meanSquare);
        float finalAmps = 0.0f;
        if (rmsCurrent >= 0.15f) {
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

void dhtTask(void *pvParameters) {
  dht.begin();
  vTaskDelay(pdMS_TO_TICKS(2000));

  float lastTemperatureC = NAN;
  float lastHumidityPct = NAN;
  uint32_t lastPublishMs = 0;

  while (true) {
    float humidity = dht.readHumidity();
    float temperature = dht.readTemperature();
    bool healthy = !isnan(humidity) && !isnan(temperature);

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      sysState.dhtHealthy = healthy;
      if (healthy) {
        sysState.temperatureC = temperature;
        sysState.humidityPct = humidity;
      }
      xSemaphoreGive(stateMutex);
    }

#if MQTT_ENABLED
    if (healthy && mqttClient.connected()) {
      bool changed = isnan(lastTemperatureC) || isnan(lastHumidityPct) ||
                     fabs(temperature - lastTemperatureC) >= 0.5f ||
                     fabs(humidity - lastHumidityPct) >= 2.0f;
      uint32_t now = millis();
      if (changed || now - lastPublishMs >= HEARTBEAT_MS) {
        publishTelemetry();
        lastTemperatureC = temperature;
        lastHumidityPct = humidity;
        lastPublishMs = now;
      }
    }
#endif

    vTaskDelay(pdMS_TO_TICKS(2000));
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

static String formatSdInfoText() {
  bool sdOk = false;
  uint64_t sdTotal = 0;
  uint64_t sdUsed = 0;

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    sdOk = sysState.sdOk;
    sdTotal = sysState.sdTotal;
    sdUsed = sysState.sdUsed;
    xSemaphoreGive(stateMutex);
  }

  uint64_t sdFree = sdTotal > sdUsed ? sdTotal - sdUsed : 0;
  float usedPct = sdTotal > 0 ? (100.0f * (float)sdUsed / (float)sdTotal) : 0.0f;

  String out;
  out += "SD status: " + String(sdOk ? "OK" : "ERROR") + "\n";
  out += "SD total: " + String((unsigned long long)sdTotal) + " bytes\n";
  out += "SD used: " + String((unsigned long long)sdUsed) + " bytes\n";
  out += "SD free: " + String((unsigned long long)sdFree) + " bytes\n";
  out += "SD used percent: " + String(usedPct, 2) + "%";
  return out;
}

static String jsonEscape(const char *value) {
  String out = value ? String(value) : "";
  out.replace("\\", "\\\\");
  out.replace("\"", "\\\"");
  return out;
}

static String jsonEscape(const String &value) {
  return jsonEscape(value.c_str());
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
    isProvisioningMode = false;
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(MDNS_HOSTNAME);
    WiFi.begin(savedSSID.c_str(), wifiPassword.c_str());
    Serial.printf("[WIFI] Connecting to saved SSID: %s...\n", savedSSID.c_str());

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
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
      MDNS.begin(MDNS_HOSTNAME);
      Serial.printf("[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
      Serial.println("[WIFI] Connection failed. Entering reconnecting mode.");
    }
    // Always return true if credentials exist so runtime tasks start
    return true;
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

  Serial.println("[WIFI] No credentials. Provisioning AP 'SmartNest' started.");
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
        strncpy(sysState.wifiSSID, WiFi.SSID().c_str(),
                sizeof(sysState.wifiSSID) - 1);
      }
      xSemaphoreGive(stateMutex);
    }

    if (!connected) {
      Serial.println("[WIFI] Disconnected. Reconnecting...");
      WiFi.reconnect();
      int retries = 0;
      while (WiFi.status() != WL_CONNECTED && retries < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        retries++;
      }

      if (WiFi.status() == WL_CONNECTED) {
        MDNS.end();
        MDNS.begin(MDNS_HOSTNAME);

        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
          sysState.wifiConnected = true;
          sysState.wifiRSSI = WiFi.RSSI();
          strncpy(sysState.wifiSSID, WiFi.SSID().c_str(),
                  sizeof(sysState.wifiSSID) - 1);
          xSemaphoreGive(stateMutex);
        }
        Serial.printf("[WIFI] Reconnected! IP: %s\n", WiFi.localIP().toString().c_str());
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
#if MQTT_ENABLED
static String mqttBool(bool value) { return value ? "true" : "false"; }

static void publishAck(const String &cmdId, const String &command, bool ok, const String &message) {
  if (!mqttClient.connected())
    return;
  String base = String(g_mqttConfig.baseTopic);
  time_t nowSecs = time(NULL);
  uint32_t ts = (nowSecs > 1000000000) ? (uint32_t)nowSecs : 0;

  StaticJsonDocument<512> doc;
  doc["cmd_id"] = cmdId.length() ? cmdId : "unknown";
  doc["command"] = command.length() ? command : "unknown";
  doc["ok"] = ok;
  doc["message"] = message;
  doc["timestamp"] = ts;

  String payload;
  serializeJson(doc, payload);
  mqttClient.publish((base + "/cmd/ack").c_str(), payload.c_str(), false);
}

static bool getLocalRelaySnapshot(int relay, bool &stateVal, bool &lockedVal) {
  if (relay < 1 || relay > NUM_RELAYS)
    return false;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) != pdTRUE)
    return false;
  stateVal = sysState.relayStates[relay - 1];
  lockedVal = sysState.lockedStates[relay - 1];
  xSemaphoreGive(stateMutex);
  return true;
}

static void startSystemRebootTask(void *pvParameters) {
  vTaskDelay(pdMS_TO_TICKS(150));
  enqueueUartCmd("{\"t\":\"system_reboot_req\"}");
  vTaskDelay(pdMS_TO_TICKS(1500));
  ESP.restart();
}

static void handleCloudCommand(const String &payload, const String &legacyType,
                               int legacyRelay = 0, bool legacyBool = false,
                               const String &legacyTarget = "") {
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    publishAck("unknown", "unknown", false, "invalid JSON payload");
    return;
  }

  String cmdId = doc["cmd_id"] | "";
  if (cmdId.length() == 0) {
    cmdId = "cmd-" + String(millis());
  }

  if (!doc.containsKey("command")) {
    publishAck(cmdId, "unknown", false, "missing command field");
    return;
  }

  String command = doc["command"] | "";
  if (command.length() == 0) {
    publishAck(cmdId, "unknown", false, "missing command field");
    return;
  }

  // clear_energy_logs is a destructive action; reject over MQTT, allow only via Serial Monitor
  if (command == "clear_energy_logs") {
    publishAck(cmdId, command, false, "clear_energy_logs is not allowed over MQTT");
    return;
  }

  // Allowed commands: relay_set, relay_lock, lights_set, ac_set, all_relays_off, unlock_all_relays, system_reboot
  if (command != "relay_set" && command != "relay_lock" && command != "lights_set" &&
      command != "ac_set" && command != "all_relays_off" && command != "unlock_all_relays" &&
      command != "system_reboot") {
    publishAck(cmdId, command, false, "unknown MQTT command");
    return;
  }

  if (command == "relay_set") {
    if (!doc.containsKey("relay") || !doc["relay"].is<int>()) {
      publishAck(cmdId, command, false, "invalid relay number");
      return;
    }
    int relay = doc["relay"].as<int>();
    if (relay < 1 || relay > 7) {
      publishAck(cmdId, command, false, "invalid relay number");
      return;
    }
    if (!doc.containsKey("state") || !doc["state"].is<bool>()) {
      publishAck(cmdId, command, false, "state field must be boolean");
      return;
    }
    bool state = doc["state"].as<bool>();

    if (relay <= NUM_RELAYS) {
      bool isLocked = false;
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        isLocked = sysState.lockedStates[relay - 1];
        xSemaphoreGive(stateMutex);
      }
      if (state && isLocked) {
        publishAck(cmdId, command, false, "relay " + String(relay) + " is locked");
        return;
      }
      setRelayState(relay - 1, state);
      publishAck(cmdId, command, true, "relay " + String(relay) + " set to " + String(state ? "ON" : "OFF"));
      publishLiveData();
    } else {
      if (g_pendingD1CmdId.length() > 0) {
        publishAck(cmdId, command, false, "relay 7 is busy");
        return;
      }
      g_pendingD1CmdId = cmdId;
      g_pendingD1CmdType = command;
      g_pendingD1CmdMs = millis();
      String d1Cmd = state ? "relay_on" : "relay_off";
      enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"" + d1Cmd + "\"}");
    }
  }
  else if (command == "relay_lock") {
    if (!doc.containsKey("relay") || !doc["relay"].is<int>()) {
      publishAck(cmdId, command, false, "invalid relay number");
      return;
    }
    int relay = doc["relay"].as<int>();
    if (relay < 1 || relay > 7) {
      publishAck(cmdId, command, false, "invalid relay number");
      return;
    }
    if (!doc.containsKey("locked") || !doc["locked"].is<bool>()) {
      publishAck(cmdId, command, false, "locked field must be boolean");
      return;
    }
    bool locked = doc["locked"].as<bool>();

    if (relay <= NUM_RELAYS) {
      setLocalRelayLock(relay - 1, locked);
      saveLocalControlState();
      updateRelayHardware();
      publishAck(cmdId, command, true, "relay " + String(relay) + (locked ? " locked" : " unlocked"));
      publishLiveData();
    } else {
      if (g_pendingD1CmdId.length() > 0) {
        publishAck(cmdId, command, false, "relay 7 is busy");
        return;
      }
      g_pendingD1CmdId = cmdId;
      g_pendingD1CmdType = command;
      g_pendingD1CmdMs = millis();
      String d1Cmd = locked ? "relay_lock" : "relay_unlock";
      enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"" + d1Cmd + "\"}");
    }
  }
  else if (command == "lights_set") {
    if (!doc.containsKey("state") || !doc["state"].is<bool>()) {
      publishAck(cmdId, command, false, "state field must be boolean");
      return;
    }
    bool state = doc["state"].as<bool>();
    for (int i = 0; i < 5; i++) {
      setRelayState(i, state);
    }
    publishAck(cmdId, command, true, "relay 1 to relay 5 set to " + String(state ? "ON" : "OFF"));
    publishLiveData();
  }
  else if (command == "ac_set") {
    int fieldCount = 0;
    if (doc.containsKey("power")) fieldCount++;
    if (doc.containsKey("temp")) fieldCount++;
    if (doc.containsKey("temp_step")) fieldCount++;
    if (doc.containsKey("fan")) fieldCount++;
    if (fieldCount != 1) {
      publishAck(cmdId, command, false, "provide exactly one AC field");
      return;
    }

    if (doc.containsKey("power")) {
      if (!doc["power"].is<bool>()) {
        publishAck(cmdId, command, false, "power field must be boolean");
        return;
      }
      bool power = doc["power"].as<bool>();
      enqueueUartCmd(String("{\"t\":\"ac_cmd\",\"cmd\":\"power\",\"val\":\"") +
                     (power ? "on" : "off") + "\"}");
      publishAck(cmdId, command, true, String("AC power ") + (power ? "ON" : "OFF"));
    } else if (doc.containsKey("temp")) {
      if (!doc["temp"].is<int>()) {
        publishAck(cmdId, command, false, "temp field must be integer");
        return;
      }
      int temp = doc["temp"].as<int>();
      if (temp < 16 || temp > 30) {
        publishAck(cmdId, command, false, "temp must be 16-30");
        return;
      }
      enqueueUartCmd("{\"t\":\"ac_cmd\",\"cmd\":\"temp\",\"val\":" + String(temp) + "}");
      publishAck(cmdId, command, true, "AC temp set to " + String(temp));
    } else if (doc.containsKey("temp_step")) {
      if (!doc["temp_step"].is<const char *>()) {
        publishAck(cmdId, command, false, "temp_step field must be string");
        return;
      }
      String step = doc["temp_step"].as<const char *>();
      step.toLowerCase();
      if (step != "up" && step != "down") {
        publishAck(cmdId, command, false, "temp_step must be up/down");
        return;
      }
      enqueueUartCmd("{\"t\":\"ac_cmd\",\"cmd\":\"temp_step\",\"val\":\"" + step + "\"}");
      publishAck(cmdId, command, true, "AC temp step " + step);
    } else if (doc.containsKey("fan")) {
      if (!doc["fan"].is<const char *>()) {
        publishAck(cmdId, command, false, "fan field must be string");
        return;
      }
      String fan = doc["fan"].as<const char *>();
      fan.toLowerCase();
      if (fan != "auto" && fan != "min" && fan != "low" && fan != "med" &&
          fan != "high" && fan != "max") {
        publishAck(cmdId, command, false, "invalid fan speed");
        return;
      }
      enqueueUartCmd("{\"t\":\"ac_cmd\",\"cmd\":\"fan\",\"val\":\"" + fan + "\"}");
      publishAck(cmdId, command, true, "AC fan set to " + fan);
    }
  }
  else if (command == "all_relays_off") {
    if (g_pendingD1CmdId.length() > 0) {
      publishAck(cmdId, command, false, "relay 7 is busy");
      return;
    }
    for (int i = 0; i < NUM_RELAYS; i++) {
      setRelayState(i, false);
    }
    g_pendingD1CmdId = cmdId;
    g_pendingD1CmdType = command;
    g_pendingD1CmdMs = millis();
    enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"relay_off\"}");
    enqueueUartCmd("{\"t\":\"ac_cmd\",\"cmd\":\"power\",\"val\":\"off\"}");
    publishLiveData();
  }
  else if (command == "unlock_all_relays") {
    if (g_pendingD1CmdId.length() > 0) {
      publishAck(cmdId, command, false, "relay 7 is busy");
      return;
    }
    for (int i = 0; i < NUM_RELAYS; i++) {
      setLocalRelayLock(i, false);
    }
    saveLocalControlState();
    updateRelayHardware();
    g_pendingD1CmdId = cmdId;
    g_pendingD1CmdType = command;
    g_pendingD1CmdMs = millis();
    enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"relay_unlock\"}");
    publishLiveData();
  }
  else if (command == "system_reboot") {
    publishAck(cmdId, command, true, "system reboot accepted; reboot sequence started");
    delay(200);
    xTaskCreatePinnedToCore(startSystemRebootTask, "SysReboot", 2048, NULL, 1,
                            NULL, 0);
  }

}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String t = String(topic);
  String p = String((char *)payload, length);
  p.trim();

  String base = String(g_mqttConfig.baseTopic);
  if (t == base + "/cmd/request") {
    handleCloudCommand(p, "");
  } else if (t == base + "/history/ack") {
    StaticJsonDocument<256> doc;
    if (!deserializeJson(doc, p)) {
      String batchId = doc["batch_id"] | "";
      bool ok = doc["ok"] | false;
      uint32_t lastId = doc["last_id"] | 0;
      if (ok && g_historyPending && batchId == g_historyPendingBatchId) {
        if (lastId == 0)
          lastId = g_historyPendingLastId;
        if (lastId > g_historyPendingLastId) {
#if DEBUG_VERBOSE
          Serial.printf("[HISTORY] ACK rejected batch_id=%s last_id=%u pending_last=%u\n",
                        batchId.c_str(), lastId, g_historyPendingLastId);
#endif
          return;
        }
        enqueueUartCmd("{\"t\":\"hist_ack\",\"last\":" + String(lastId) + "}");
        g_historyLastAckedId = lastId;
        g_historyPending = false;
        g_historyPendingBatchId = "";
        g_historyPendingLastId = 0;
#if DEBUG_VERBOSE
        Serial.printf("[HISTORY] ACK received batch_id=%s last_id=%u\n",
                      batchId.c_str(), lastId);
#endif
        requestHistoryBatch(true);
      }
    }
  }
}

void publishTelemetry() {
  publishLiveData();
}

void publishLiveData() {
  if (!mqttClient.connected())
    return;

  String base = String(g_mqttConfig.baseTopic);
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    String sensors = "{";
    sensors += "\"voltage\":" + String(sysState.pzemVoltage, 1) + ",";
    sensors += "\"energy_voltage\":" + String(sysState.energyVoltage, 1) + ",";
    sensors += "\"main_current\":" + String(sysState.currentAmps, 2) + ",";
    sensors += "\"digital_current\":" + String(sysState.acsCurrentA, 2) + ",";
    sensors += "\"ac_current\":" + String(sysState.pzemCurrentA, 3) + ",";
    sensors += "\"ac_power\":" + String(sysState.pzemPowerW, 1) + ",";
    sensors += "\"ac_energy_kwh\":" + String(sysState.acEnergyKWh, 3) + ",";
    sensors += "\"pzem_cumulative_energy_kwh\":" + String(sysState.pzemRawEnergyKWh, 3) + ",";
    sensors += "\"ac_day_start_kwh\":" + String(sysState.acDayStartKWh, 3) + ",";
    sensors += "\"main_energy_kwh\":" + String(sysState.mainEnergyKWh, 3) + ",";
    sensors += "\"digital_energy_kwh\":" + String(sysState.digitalEnergyKWh, 3) + ",";
    sensors += "\"temperature_c\":" + String(sysState.temperatureC, 1) + ",";
    sensors += "\"humidity_pct\":" + String(sysState.humidityPct, 1);
    sensors += "}";

    String relays = "{\"states\":[";
    for (int i = 0; i < NUM_RELAYS; i++) {
      if (i > 0)
        relays += ",";
      relays += mqttBool(sysState.relayStates[i]);
    }
    relays += "," + mqttBool(sysState.digitalRelayState) + "],\"locks\":[";
    for (int i = 0; i < NUM_RELAYS; i++) {
      if (i > 0)
        relays += ",";
      relays += mqttBool(sysState.lockedStates[i]);
    }
    relays += "," + mqttBool(sysState.digitalRelayLocked) + "],\"master_lock\":";
    bool allLocked = sysState.digitalRelayLocked;
    for (int i = 0; i < NUM_RELAYS; i++) {
      allLocked = allLocked && sysState.lockedStates[i];
    }
    relays += mqttBool(allLocked);
    relays += ",\"digital_switch\":" + mqttBool(sysState.digitalSwitchState);
    relays += ",\"runtime_sec\":[";
    for (int i = 0; i < 7; i++) {
      if (i > 0)
        relays += ",";
      relays += String(sysState.relayRuntimeSec[i]);
    }
    relays += "]}";

    String status = "{";
    status += "\"uptime\":" + String(millis()) + ",";
    status += "\"ssid\":\"" + jsonEscape(sysState.wifiSSID) + "\",";
    status += "\"rssi\":" + String(sysState.wifiRSSI) + ",";
    status += "\"mqtt_status\":" + String(sysState.mqttStatus) + ",";
    status += "\"sd_ok\":" + mqttBool(sysState.sdOk) + ",";
    status += "\"sd_total\":" + String((unsigned long long)sysState.sdTotal) + ",";
    status += "\"sd_used\":" + String((unsigned long long)sysState.sdUsed) + ",";
    status += "\"dht_ok\":" + mqttBool(sysState.dhtHealthy) + ",";
    status += "\"voltage_estimated\":" + mqttBool(sysState.voltageEstimated) + ",";
    status += "\"time_source\":\"" + jsonEscape(sysState.timeSource) + "\",";
    status += "\"reset_reason\":\"" + jsonEscape(sysState.resetReason) + "\"";
    status += "}";

    xSemaphoreGive(stateMutex);
    mqttClient.publish((base + "/live/sensors").c_str(), sensors.c_str(),
                       false);
    mqttClient.publish((base + "/live/relays").c_str(), relays.c_str(), true);
    mqttClient.publish((base + "/live/status").c_str(), status.c_str(), true);
  }
}

void publishSlavesStatus() {
#if MQTT_ENABLED
  if (!mqttClient.connected())
    return;

  bool dOnline = false;
  int dRssi = 0;
  uint32_t dAge = 0;
  bool pOnline = false;
  int pRssi = 0;
  uint32_t pAge = 0;

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    dOnline = sysState.digitalSlaveOnline;
    dRssi = sysState.digitalSlaveRSSI;
    dAge = sysState.digitalLastSeenAgeSec;
    pOnline = sysState.pzemSlaveOnline;
    pRssi = sysState.pzemSlaveRSSI;
    pAge = sysState.pzemLastSeenAgeSec;
    xSemaphoreGive(stateMutex);
  }

  uint32_t elapsedSec = 0;
  if (g_lastSlaveStatusRxMs > 0) {
    elapsedSec = (millis() - g_lastSlaveStatusRxMs) / 1000;
  }
  uint32_t dCurrentAge = dAge + elapsedSec;
  uint32_t pCurrentAge = pAge + elapsedSec;

  StaticJsonDocument<256> doc;
  JsonObject dbObj = doc.createNestedObject("digital_board");
  dbObj["online"] = dOnline;
  dbObj["rssi"] = dRssi;
  dbObj["last_seen_sec_ago"] = dCurrentAge;

  JsonObject pzemObj = doc.createNestedObject("pzem");
  pzemObj["online"] = pOnline;
  pzemObj["rssi"] = pRssi;
  pzemObj["last_seen_sec_ago"] = pCurrentAge;

  String payload;
  serializeJson(doc, payload);

  String base = String(g_mqttConfig.baseTopic);
  mqttClient.publish((base + "/live/slaves").c_str(), payload.c_str(), true);
  g_lastSlavesMqttPublishMs = millis();
#endif
}

void requestHistoryBatch(bool force) {
  if (!mqttClient.connected() || g_historyPending)
    return;
  uint32_t now = millis();
  if (!force && now - g_historyLastRequestMs < MQTT_HISTORY_RETRY_MS)
    return;
  g_historyLastRequestMs = now;
#if DEBUG_VERBOSE
  Serial.printf("[HISTORY] Requesting batch after=%u limit=%u\n",
                g_historyLastAckedId, g_historyBatchLimit);
#endif
  enqueueUartCmd("{\"t\":\"hist_req\",\"after\":" +
                 String(g_historyLastAckedId) + ",\"limit\":" +
                 String(g_historyBatchLimit) + "}");
}

void mqttTask(void *pvParameters) {
  mqttClient.setServer(g_mqttConfig.broker, g_mqttConfig.port);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(g_mqttConfig.keepAlive);
  mqttClient.setBufferSize(3072);

  uint32_t lastReconnectAttempt = 0;
  uint32_t lastHeartbeat = 0;
  bool wasMqttConnected = false;

  while (true) {
    if (g_mqttConfigChanged) {
      g_mqttConfigChanged = false;
      Serial.println("[MQTT] Config changed - disconnecting for re-init");
      if (mqttClient.connected()) {
        mqttClient.disconnect();
      }
      mqttClient.setServer(g_mqttConfig.broker, g_mqttConfig.port);
      mqttClient.setKeepAlive(g_mqttConfig.keepAlive);
      mqttClient.setBufferSize(3072);
      wasMqttConnected = false;
      lastReconnectAttempt = 0;
    }
    if (!g_mqttConfig.enabled) {
      if (wasMqttConnected) {
        mqttClient.disconnect();
        enqueueUartCmd("{\"t\":\"cloud\",\"up\":false}");
        wasMqttConnected = false;
      }
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        sysState.mqttStatus = 0;
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
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
          sysState.mqttStatus = 1;
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
            mqttClient.subscribe((base + "/cmd/request").c_str());
            mqttClient.subscribe((base + "/history/ack").c_str());

            publishLiveData();
            requestHistoryBatch(true);
            enqueueUartCmd("{\"t\":\"cloud\",\"up\":true}");
            wasMqttConnected = true;
            Serial.printf("[MQTT] Connected to %s:%d (topic: %s)\n",
                          g_mqttConfig.broker, g_mqttConfig.port,
                          g_mqttConfig.baseTopic);
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
              sysState.mqttStatus = 2;
              xSemaphoreGive(stateMutex);
            }
          } else {
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
              sysState.mqttStatus = 3;
              xSemaphoreGive(stateMutex);
            }
          }
        }
      } else {
        mqttClient.loop();
        if (g_pendingD1CmdId.length() > 0 &&
            millis() - g_pendingD1CmdMs > MQTT_HISTORY_RETRY_MS) {
          publishAck(g_pendingD1CmdId, g_pendingD1CmdType, false, "timeout");
          g_pendingD1CmdId = "";
          g_pendingD1CmdType = "";
        }

        if (g_historyPending &&
            millis() - g_historyLastRequestMs > MQTT_HISTORY_RETRY_MS) {
          g_historyPending = false;
          g_historyPendingBatchId = "";
          g_historyPendingLastId = 0;
#if DEBUG_VERBOSE
          Serial.printf("[HISTORY] ACK timeout; retry from lastAckedId=%u\n",
                        g_historyLastAckedId);
#endif
        }
        if (millis() - lastHeartbeat > HEARTBEAT_MS) {
          lastHeartbeat = millis();
          publishTelemetry();
        }
        if (millis() - g_lastSlavesMqttPublishMs >= 60000) {
          publishSlavesStatus();
        }
        requestHistoryBatch(false);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
#endif
void uartCommInit() {
  Serial2.begin(UART2_BAUD, SERIAL_8N1, UART2_RX_PIN, UART2_TX_PIN);
}

void uartCommTask(void *pvParameters) {
  uartCommInit();

  String rxBuffer = "";
  uint32_t lastAcsSentTime = 0;

  while (true) {
    UartCmdItem txItem;
    if (xQueueReceive(UartCmdQueue, &txItem, 0) == pdTRUE) {
      Serial2.println(txItem.text);
    }

    uint32_t now = millis();
    if (now - lastAcsSentTime >= 10000) {
      lastAcsSentTime = now;
      float ampsVal = 0.0f;
      uint8_t relayMask = 0;
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        ampsVal = sysState.currentAmps;
        for (int i = 0; i < NUM_RELAYS; i++) {
          if (sysState.relayStates[i])
            relayMask |= (1 << i);
        }
        xSemaphoreGive(stateMutex);
      }
      String acsMsg = "{\"t\":\"acs\",\"i\":" + String(ampsVal, 2) + "}";
      Serial2.println(acsMsg);
      Serial2.println("{\"t\":\"rel\",\"mask\":" + String(relayMask) + "}");
    }
    while (Serial2.available() > 0) {
      char c = Serial2.read();
      if (c == '\n') {
        rxBuffer.trim();
        if (rxBuffer.length() > 0) {
          StaticJsonDocument<6144> doc;
          DeserializationError err = deserializeJson(doc, rxBuffer);
          if (!err) {
            String type = doc["t"];
            if (type == "tel") {
              if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                sysState.acsCurrentA = doc["acs"];
                sysState.pzemVoltage = doc["v"];
                sysState.energyVoltage = doc["ev"] | sysState.pzemVoltage;
                sysState.voltageEstimated = doc["ve"] | false;
                sysState.pzemCurrentA = doc["pi"];
                sysState.pzemPowerW = doc["pp"];
                sysState.acEnergyKWh = doc["pe"] | 0.0;
                sysState.pzemRawEnergyKWh = doc["pe_raw"] | sysState.pzemRawEnergyKWh;
                sysState.acDayStartKWh = doc["pe_start"] | sysState.acDayStartKWh;
                sysState.mainEnergyKWh = doc["me"] | 0.0;
                sysState.digitalEnergyKWh = doc["de"] | 0.0;
                JsonArray runtime = doc["rt"].as<JsonArray>();
                if (runtime.size() >= 7) {
                  for (int i = 0; i < 7; i++) {
                    sysState.relayRuntimeSec[i] = runtime[i] | 0;
                  }
                }
                sysState.digitalSlaveOnline = doc["d_on"];
                sysState.pzemSlaveOnline = doc["p_on"];
                sysState.digitalSlaveRSSI = doc["d_rssi"];
                sysState.pzemSlaveRSSI = doc["p_rssi"];
                sysState.digitalRelayState = doc["d_relay"];
                sysState.digitalSwitchState = doc["d_sw"];
                sysState.digitalRelayLocked = doc["d_lock"];
                sysState.sdOk = doc["sd_ok"];
                sysState.sdTotal = doc["sd_total"];
                sysState.sdUsed = doc["sd_used"];
                sysState.pzemSensorHealthy = doc["p_health"] | false;
                const char *masterResetReason = doc["reset_reason"] | "";
                if (strlen(masterResetReason) > 0) {
                  strncpy(sysState.resetReason, masterResetReason, sizeof(sysState.resetReason) - 1);
                  sysState.resetReason[sizeof(sysState.resetReason) - 1] = '\0';
                }
                const char *timeSource = doc["tsrc"] | "";
                if (strlen(timeSource) > 0) {
                  strncpy(sysState.timeSource, timeSource, sizeof(sysState.timeSource) - 1);
                  sysState.timeSource[sizeof(sysState.timeSource) - 1] = '\0';
                }
                xSemaphoreGive(stateMutex);
              }
#if DEBUG_VERBOSE
              static uint32_t lastTelLogTime = 0;
              if (millis() - lastTelLogTime >= 5000) {
                lastTelLogTime = millis();
                Serial.printf("[SmartNest] Telemetry update - V: %.1fV, Load: "
                              "%.2fA, SD: %s\n",
                              sysState.pzemVoltage, sysState.currentAmps,
                              sysState.sdOk ? "OK" : "ERROR");
              }
#endif
#if MQTT_ENABLED
              if (mqttClient.connected()) {
                publishTelemetry();
              }
#endif
            } else if (type == "sw") {
              int idx = doc["idx"];
              int s = doc["s"];
              Serial.printf("[SmartNest][SW] Manual switch event received: idx=%d, state=%d\n", idx, s);
              if (idx >= 0 && idx < NUM_RELAYS) {
                setRelayState(idx, s == 1);
              }
            } else if (type == "lock_ack") {
              bool val = doc["val"];
#if DEBUG_VERBOSE
              Serial.printf("[LOCK] Legacy lock_ack received: %s\n", val ? "ON" : "OFF");
#endif
            } else if (type == "ac_ack") {
              bool ok = doc["ok"] | false;
              const char *cmd = doc["cmd"] | "";
              if (ok && xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                if (strcmp(cmd, "power") == 0) {
                  sysState.acPower = doc["power"] | false;
                } else if (strcmp(cmd, "temp") == 0) {
                  sysState.acTempC = doc["degrees"] | sysState.acTempC;
                } else if (strcmp(cmd, "temp_step") == 0) {
                  sysState.acTempC = doc["degrees"] | sysState.acTempC;
                } else if (strcmp(cmd, "fan") == 0) {
                  strncpy(sysState.acFan, doc["fan"] | "", 7);
                  sysState.acFan[7] = '\0';
                }
                xSemaphoreGive(stateMutex);
              }
#if DEBUG_VERBOSE
              if (ok) {
                bool power = doc["power"] | false;
                int degrees = doc["degrees"] | 0;
                const char *fan = doc["fan"] | "";
                Serial.printf("[AC] ac_ack cmd=%s ok=YES power=%s temp=%d fan=%s\n",
                              cmd, power ? "ON" : "OFF", degrees, fan);
              } else {
                const char *msg = doc["msg"] | "";
                Serial.printf("[AC] ac_ack cmd=%s ok=NO msg=%s\n", cmd, msg);
              }
#endif
            } else if (type == "cmd_ack") {
              String tgt = doc["tgt"] | "";
              int cmdType = doc["cmd_type"] | 0;
              bool ok = doc["ok"] | false;
              int reason = doc["reason"] | 0;
              int relay = doc["relay"] | 0;
              int relayLock = doc["relay_lock"] | 0;
              int masterLock = doc["master_lock"] | 0;
              int overcurrentLock = doc["oc_lock"] | 0;

              if (tgt == "d1") {
                if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                  sysState.digitalRelayState = relay != 0;
                  sysState.digitalRelayLocked = relayLock != 0;
                  xSemaphoreGive(stateMutex);
                }
#if DEBUG_VERBOSE
                Serial.printf("[CMD_ACK] d1 cmd=0x%02X ok=%s reason=%d relay=%d relayLock=%d masterLock=%d ocLock=%d\n",
                              cmdType, ok ? "YES" : "NO", reason, relay,
                              relayLock, masterLock, overcurrentLock);
#endif
#if MQTT_ENABLED
                if (mqttClient.connected()) {
                  if (g_pendingD1CmdId.length() > 0) {
                    String msg = "";
                    bool finalOk = ok;
                    if (g_pendingD1CmdType == "relay_set") {
                      if (ok) {
                        msg = "relay 7 set to " + String(relay != 0 ? "ON" : "OFF");
                      } else {
                        if (reason == 1)
                          msg = "relay 7 is locked";
                        else if (reason == 3)
                          msg = "relay 7 overcurrent locked";
                        else
                          msg = "relay 7 set failed";
                      }
                    }
                    else if (g_pendingD1CmdType == "relay_lock") {
                      if (ok) {
                        msg = "relay 7 " + String(relayLock != 0 ? "locked" : "unlocked");
                      } else {
                        msg = "relay 7 lock failed";
                      }
                    }
                    else if (g_pendingD1CmdType == "all_relays_off") {
                      bool anyLocked = false;
                      for (int i = 0; i < NUM_RELAYS; i++) {
                        if (sysState.lockedStates[i]) anyLocked = true;
                      }
                      if (sysState.digitalRelayLocked) anyLocked = true;
                      if (anyLocked) {
                        finalOk = false;
                        msg = "some relays could not be turned OFF because they are locked";
                      } else {
                        finalOk = true;
                        msg = "all controllable relays turned OFF";
                      }
                    }
                    else if (g_pendingD1CmdType == "unlock_all_relays") {
                      if (ok) {
                        msg = "all relay locks cleared";
                      } else {
                        msg = "unlock_all_relays failed";
                      }
                    }

                    publishAck(g_pendingD1CmdId, g_pendingD1CmdType, finalOk, msg);
                    g_pendingD1CmdId = "";
                    g_pendingD1CmdType = "";
                  }
                }
#endif
              }
            } else if (type == "hist_res") {
#if MQTT_ENABLED
              if (mqttClient.connected() && !g_historyPending) {
                JsonArray records = doc["records"].as<JsonArray>();
                uint32_t lastId = doc["last"] | 0;
                if (records.size() > 0 && lastId > 0) {
                  String batchId = String(g_mqttConfig.clientId) + "-" +
                                   String(lastId) + "-" + String(millis());
                  String payload = "{\"batch_id\":\"" + jsonEscape(batchId) +
                                   "\",\"device\":\"" +
                                   jsonEscape(g_mqttConfig.clientId) +
                                   "\",\"records\":";
                  String recordsJson;
                  serializeJson(records, recordsJson);
                  payload += recordsJson;
                  payload += "}";
                  String base = String(g_mqttConfig.baseTopic);
                  if (payload.length() > MQTT_HISTORY_MAX_PAYLOAD_BYTES) {
#if DEBUG_VERBOSE
                    Serial.printf("[HISTORY] Oversized batch skipped bytes=%u max=%u last_id=%u\n",
                                  payload.length(), MQTT_HISTORY_MAX_PAYLOAD_BYTES, lastId);
#endif
                    g_historyPending = false;
                    g_historyPendingBatchId = "";
                    g_historyPendingLastId = 0;
                    if (g_historyBatchLimit > 1) {
                      g_historyBatchLimit = (g_historyBatchLimit + 1) / 2;
                      g_historyLastRequestMs = 0;
                      requestHistoryBatch(true);
                    }
                  } else if (mqttClient.publish((base + "/history/batch").c_str(),
                                                payload.c_str(), false)) {
                    g_historyPending = true;
                    g_historyPendingBatchId = batchId;
                    g_historyPendingLastId = lastId;
                    g_historyLastRequestMs = millis();
                    g_historyBatchLimit = MQTT_HISTORY_BATCH_LIMIT;
#if DEBUG_VERBOSE
                    Serial.printf("[HISTORY] Published batch_id=%s count=%u last_id=%u bytes=%u\n",
                                  batchId.c_str(), records.size(), lastId, payload.length());
#endif
                  } else {
#if DEBUG_VERBOSE
                    Serial.printf("[HISTORY] Publish failed batch_id=%s count=%u last_id=%u bytes=%u\n",
                                  batchId.c_str(), records.size(), lastId, payload.length());
#endif
                  }
                }
              }
#endif
            } else if (type == "last_record_res") {
              if (g_sdLastRecordSerialPending) {
                g_sdLastRecordSerialPending = false;
                bool ok = doc["ok"] | false;
                if (ok) {
                  uint32_t recordId = doc["record_id"] | 0;
                  const char *date = doc["date"] | "";
                  Serial.println();
                  Serial.println("--- SD Last Record ---");
                  Serial.printf("Record ID: %u\n", recordId);
                  Serial.printf("Date:      %s\n", date);
                  Serial.println("----------------------");
                } else {
                  const char *msg = doc["message"] | "Unknown error";
                  Serial.printf("\nERR: %s\n", msg);
                }
              }
              if (g_sdLastRecordHttpPending && g_sdLastRecordHttpReq != nullptr) {
                g_sdLastRecordHttpPending = false;
                bool ok = doc["ok"] | false;
                String resp;
                if (ok) {
                  uint32_t recId = doc["record_id"] | 0;
                  const char* date = doc["date"] | "";
                  resp = "{\"ok\":true,\"record_id\":" + String(recId) +
                         ",\"date\":\"" + String(date) + "\"}";
                } else {
                  const char* msg = doc["message"] | "Unknown error";
                  resp = "{\"ok\":false,\"message\":\"" + String(msg) + "\"}";
                }
                sendJson(g_sdLastRecordHttpReq, ok ? 200 : 500, resp);
                g_sdLastRecordHttpReq = nullptr;
              }
            } else if (type == "clear_energy_logs_res") {
              bool ok = doc["ok"] | false;
              const char *msg = doc["message"] | "";
              if (g_clearEnergyLogsSerialPending) {
                g_clearEnergyLogsSerialPending = false;
                if (ok) {
                  Serial.printf("\nOK: %s\n", msg);
                } else {
                  Serial.printf("\nERR: %s\n", msg);
                }
              }
              if (g_clearLogsHttpPending && g_clearLogsHttpReq != nullptr) {
                g_clearLogsHttpPending = false;
                bool ok = doc["ok"] | false;
                const char* msg = doc["message"] | "";
                sendJson(g_clearLogsHttpReq, ok ? 200 : 500,
                         "{\"ok\":" + String(ok ? "true" : "false") +
                         ",\"message\":\"" + String(msg) + "\"}");
                g_clearLogsHttpReq = nullptr;
              }

            } else if (type == "slave_status") {
              bool dOnline = doc["digital_board"]["online"] | false;
              int dRssi = doc["digital_board"]["rssi"] | 0;
              uint32_t dAge = doc["digital_board"]["last_seen_age_sec"] | 0;
              uint32_t dMissed = doc["digital_board"]["missed_heartbeats"] | 0;
              const char* dReason = doc["digital_board"]["reason"] | "";

              bool pOnline = doc["pzem"]["online"] | false;
              int pRssi = doc["pzem"]["rssi"] | 0;
              uint32_t pAge = doc["pzem"]["last_seen_age_sec"] | 0;
              uint32_t pMissed = doc["pzem"]["missed_heartbeats"] | 0;
              const char* pReason = doc["pzem"]["reason"] | "";
              bool pHealthy = doc["pzem"]["pzem_health"] | false;

              bool dChanged = false;
              bool pChanged = false;

              if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                if (sysState.digitalSlaveOnline != dOnline) {
                  dChanged = true;
                  sysState.digitalSlaveOnline = dOnline;
                }
                sysState.digitalSlaveRSSI = dRssi;
                sysState.digitalLastSeenAgeSec = dAge;
                sysState.digitalMissedHeartbeats = dMissed;

                if (sysState.pzemSlaveOnline != pOnline) {
                  pChanged = true;
                  sysState.pzemSlaveOnline = pOnline;
                }
                sysState.pzemSlaveRSSI = pRssi;
                sysState.pzemLastSeenAgeSec = pAge;
                sysState.pzemMissedHeartbeats = pMissed;
                sysState.pzemHealthFromMaster = pHealthy;
                sysState.pzemSensorHealthy = pHealthy;
                xSemaphoreGive(stateMutex);
              }

              g_lastSlaveStatusRxMs = millis();
              g_lastDigitalAgeSec = dAge;
              g_lastPzemAgeSec = pAge;

              auto formatAge = [](uint32_t sec) -> String {
                if (sec < 60) {
                  return String(sec) + "s ago";
                } else if (sec < 3600) {
                  uint32_t m = sec / 60;
                  uint32_t s = sec % 60;
                  return String(m) + "m " + String(s) + "s ago";
                } else {
                  uint32_t h = sec / 3600;
                  uint32_t m = (sec % 3600) / 60;
                  uint32_t s = sec % 60;
                  return String(h) + "h " + String(m) + "m " + String(s) + "s ago";
                }
              };

              if (dChanged) {
                if (dOnline) {
                  Serial.printf("\n[SLAVE] Digital Board: ONLINE | RSSI: %d dBm | Last seen: %s\n",
                                dRssi, formatAge(dAge).c_str());
                } else {
                  Serial.printf("\n[SLAVE] Digital Board: OFFLINE | RSSI: %d dBm | Reason: %s | Last seen: %s\n",
                                dRssi, dReason, formatAge(dAge).c_str());
                }
              }

              if (pChanged) {
                if (pOnline) {
                  Serial.printf("\n[SLAVE] PZEM: ONLINE | RSSI: %d dBm | Last seen: %s | PZEM Health: OK\n",
                                pRssi, formatAge(pAge).c_str());
                } else {
                  Serial.printf("\n[SLAVE] PZEM: OFFLINE | RSSI: %d dBm | Reason: %s | Last seen: %s | PZEM Health: FAIL\n",
                                pRssi, pReason, formatAge(pAge).c_str());
                }
              }

              if (dChanged || pChanged) {
                publishSlavesStatus();
              }
            }
          }
        }
        rxBuffer = "";
      } else if (c != '\r') {
        rxBuffer += c;
        if (rxBuffer.length() > UART_RX_BUFFER_MAX) {
          Serial.println("[UART] RX line too long, dropping");
          rxBuffer = "";
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
void ntpSyncTask(void *pvParameters) {
  while (true) {
    if (WiFi.status() == WL_CONNECTED) {
      configTime(19800, 0, "pool.ntp.org");
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
    vTaskDelay(pdMS_TO_TICKS(300000));
  }
}

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
  bool relays[NUM_RELAYS] = {false};
  bool locks[NUM_RELAYS] = {false};
  bool dOn = false, pOn = false, dRelay = false, dLock = false, dSw = false;
  bool mLock = false, sdOk = false, pHealth = false, dhtOk = false;
  bool voltageEstimated = false;
  int rssi = 0, dRssi = 0, pRssi = 0, mqttStatus = 0;
  char ssid[33] = "";
  char timeSource[12] = "";
  char acFan[8] = "";
  float current = 0.0f, acs = 0.0f, voltage = 0.0f, energyVoltage = 0.0f, acCurrent = 0.0f;
  float acPower = 0.0f, tempC = 0.0f, humidity = 0.0f;
  bool acPowerState = false;
  int acTempC = 0;
  double acEnergy = 0.0, rawPzemEnergy = 0.0, acDayStart = 0.0;
  double mainEnergy = 0.0, digitalEnergy = 0.0;
  uint32_t runtime[7] = {0};
  uint64_t sdTotal = 0, sdUsed = 0;

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    for (int i = 0; i < NUM_RELAYS; i++) {
      relays[i] = sysState.relayStates[i];
      locks[i] = sysState.lockedStates[i];
    }
    for (int i = 0; i < 7; i++) {
      runtime[i] = sysState.relayRuntimeSec[i];
    }
    current = sysState.currentAmps;
    rssi = sysState.wifiRSSI;
    strncpy(ssid, sysState.wifiSSID, sizeof(ssid) - 1);
    acs = sysState.acsCurrentA;
    voltage = sysState.pzemVoltage;
    energyVoltage = sysState.energyVoltage;
    voltageEstimated = sysState.voltageEstimated;
    acCurrent = sysState.pzemCurrentA;
    acPower = sysState.pzemPowerW;
    acEnergy = sysState.acEnergyKWh;
    rawPzemEnergy = sysState.pzemRawEnergyKWh;
    acDayStart = sysState.acDayStartKWh;
    mainEnergy = sysState.mainEnergyKWh;
    digitalEnergy = sysState.digitalEnergyKWh;
    dOn = sysState.digitalSlaveOnline;
    pOn = sysState.pzemSlaveOnline;
    dRssi = sysState.digitalSlaveRSSI;
    pRssi = sysState.pzemSlaveRSSI;
    dRelay = sysState.digitalRelayState;
    dLock = sysState.digitalRelayLocked;
    dSw = sysState.digitalSwitchState;
    mLock = dLock;
    for (int i = 0; i < NUM_RELAYS; i++) {
      mLock = mLock && locks[i];
    }
    sdOk = sysState.sdOk;
    sdTotal = sysState.sdTotal;
    sdUsed = sysState.sdUsed;
    pHealth = sysState.pzemSensorHealthy;
    tempC = sysState.temperatureC;
    humidity = sysState.humidityPct;
    dhtOk = sysState.dhtHealthy;
    acPowerState = sysState.acPower;
    acTempC = sysState.acTempC;
    strncpy(acFan, sysState.acFan, sizeof(acFan) - 1);
    mqttStatus = sysState.mqttStatus;
    strncpy(timeSource, sysState.timeSource, sizeof(timeSource) - 1);
    xSemaphoreGive(stateMutex);
  } else {
    Serial.println("[STATUS] state mutex timeout, using default snapshot");
  }

  String json = "{";
  json += "\"relays\":[";
  for (int i = 0; i < NUM_RELAYS; i++) {
    if (i > 0)
      json += ",";
    json += relays[i] ? "true" : "false";
  }
  json += "," + String(dRelay ? "true" : "false") + "],\"locks\":[";
  for (int i = 0; i < NUM_RELAYS; i++) {
    if (i > 0)
      json += ",";
    json += locks[i] ? "true" : "false";
  }
  json += "," + String(dLock ? "true" : "false") + "],";
  json += "\"current\":" + String(current, 2) + ",";
  json += "\"rssi\":" + String(rssi) + ",";
  json += "\"ssid\":\"" + jsonEscape(ssid) + "\",";
  json += "\"acs\":" + String(acs, 2) + ",";
  json += "\"load\":" + String(current, 2) + ",";
  json += "\"voltage\":" + String(voltage, 1) + ",";
  json += "\"energy_voltage\":" + String(energyVoltage, 1) + ",";
  json += "\"voltage_estimated\":" + String(voltageEstimated ? "true" : "false") + ",";
  json += "\"ac_current\":" + String(acCurrent, 3) + ",";
  json += "\"ac_power\":" + String(acPower, 1) + ",";
  json += "\"ac_energy\":" + String(acEnergy, 3) + ",";
  json += "\"pzem_energy_cumulative\":" + String(rawPzemEnergy, 3) + ",";
  json += "\"ac_day_start_energy\":" + String(acDayStart, 3) + ",";
  json += "\"main_energy\":" + String(mainEnergy, 3) + ",";
  json += "\"digital_energy\":" + String(digitalEnergy, 3) + ",";
  json += "\"relay_runtime\":[";
  for (int i = 0; i < 7; i++) {
    if (i > 0)
      json += ",";
    json += String(runtime[i]);
  }
  json += "],";
  json += "\"d_on\":" + String(dOn ? "true" : "false") + ",";
  json += "\"p_on\":" + String(pOn ? "true" : "false") + ",";
  json += "\"d_rssi\":" + String(dRssi) + ",";
  json += "\"p_rssi\":" + String(pRssi) + ",";
  json += "\"d_relay\":" + String(dRelay ? "true" : "false") + ",";
  json += "\"d_lock\":" + String(dLock ? "true" : "false") + ",";
  json += "\"d_sw\":" + String(dSw ? "true" : "false") + ",";
  json += "\"m_lock\":" + String(mLock ? "true" : "false") + ",";
  json += "\"sd_ok\":" + String(sdOk ? "true" : "false") + ",";
  json += "\"sd_total\":" + String((unsigned long long)sdTotal) + ",";
  json += "\"sd_used\":" + String((unsigned long long)sdUsed) + ",";
  json += "\"p_health\":" + String(pHealth ? "true" : "false") + ",";
  json += "\"temp_c\":" + (dhtOk ? String(tempC, 1) : "null") + ",";
  json += "\"humidity\":" + (dhtOk ? String(humidity, 1) : "null") + ",";
  json += "\"dht_ok\":" + String(dhtOk ? "true" : "false") + ",";
  json += "\"ac\":{\"power\":" + String(acPowerState ? "true" : "false")
          + ",\"temp\":" + String(acTempC)
          + ",\"fan\":\"" + jsonEscape(acFan) + "\"},";
  json += "\"mqtt_status\":" + String(mqttStatus) + ",";
  json += "\"time_source\":\"" + jsonEscape(timeSource) + "\",";
  json += "\"reset_reason\":\"" + jsonEscape(g_resetReason) + "\"";
  json += ",\"uptime\":" + String(millis());
  json += ",\"time\":\"" + getFormattedTime() + "\"}";
  return json;
}

static bool parseBoolValue(const String &value, bool &out) {
  if (value == "ON" || value == "1" || value == "TRUE") {
    out = true;
    return true;
  }
  if (value == "OFF" || value == "0" || value == "FALSE") {
    out = false;
    return true;
  }
  return false;
}

static void printSdInfo() {
  Serial.println(formatSdInfoText());
}

static bool setMasterLock(bool state) {
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    Serial.println("[LOCK] Lock-all alias rejected: state mutex busy");
    return false;
  }

  uint8_t requestedMaskBefore = 0;
  uint8_t actualMaskBefore = 0;
  for (int i = 0; i < NUM_RELAYS; i++) {
    if (sysState.requestedRelayStates[i])
      requestedMaskBefore |= (1 << i);
    if (sysState.relayStates[i])
      actualMaskBefore |= (1 << i);
    sysState.lockedStates[i] = state;
    if (state)
      sysState.requestedRelayStates[i] = false;
  }
  Serial.printf("[LOCK] Lock-all alias request=%s requestedBefore=0x%02X actualBefore=0x%02X requestedAfter=0x%02X\n",
                state ? "ON" : "OFF", requestedMaskBefore, actualMaskBefore,
                state ? 0 : requestedMaskBefore);
  xSemaphoreGive(stateMutex);

  saveLocalControlState();
  updateRelayHardware();
  enqueueUartCmd(String("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"") +
                 (state ? "relay_lock" : "relay_unlock") + "\"}");
  return true;
}

static void printHelp() {
  Serial.println();
  Serial.println("SmartNest serial commands:");
  Serial.println("HELP");
  Serial.println("STATUS");
  Serial.println("RELAY <1-7> ON|OFF");
  Serial.println("LOCK <1-7> ON|OFF");
  Serial.println("AC ON|OFF");
  Serial.println("AC TEMP <16-30>");
  Serial.println("AC TEMP+|TEMP-");
  Serial.println("AC FAN auto|min|low|med|high|max");
  Serial.println("LIGHTS ON|OFF");
  Serial.println("SHUTDOWN ON");
  Serial.println("UNLOCK-ALL");
  Serial.println("SD INFO");
  Serial.println("SD LAST-RECORD");
  Serial.println("CLEAR ENERGY LOGS");
  Serial.println("WIFI STATUS");
  Serial.println("REBOOT");
  Serial.println("MQTT SHOW");
  Serial.println("MQTT ENABLE ON|OFF");
  Serial.println("MQTT SET BROKER <host>");
  Serial.println("MQTT SET PORT <port>");
  Serial.println("MQTT SET USER <username>");
  Serial.println("MQTT SET PASS <password>");
  Serial.println("MQTT SET KEEPALIVE <seconds>");
  Serial.println();
}

static void resetWifiAndRestart() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.clear();
  prefs.end();
  delay(500);
  ESP.restart();
}


static void handleRelayCommand(int relayIdx, const String &action) {
  if (relayIdx < 1 || relayIdx > 7) {
    Serial.println("ERR: relay must be 1-7");
    return;
  }

  bool state = false;
  if (!parseBoolValue(action, state)) {
    Serial.println("ERR: use ON or OFF");
    return;
  }

  if (relayIdx <= NUM_RELAYS) {
    setRelayState(relayIdx - 1, state);
  } else {
    bool lockActive = false;
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      lockActive = sysState.digitalRelayLocked;
      xSemaphoreGive(stateMutex);
    }
    if (state && lockActive) {
      Serial.println("ERR: relay 7 is locked");
      return;
    }
    enqueueUartCmd(String("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"") +
                   (state ? "relay_on" : "relay_off") + "\"}");
  }
  Serial.println("OK");
}

static void handleLockCommand(int relayIdx, const String &value) {
  bool state = false;
  if (relayIdx < 1 || relayIdx > 7 || !parseBoolValue(value, state)) {
    Serial.println("ERR: use LOCK <1-7> ON|OFF");
    return;
  }

  if (relayIdx <= NUM_RELAYS) {
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      sysState.lockedStates[relayIdx - 1] = state;
      if (state) {
        sysState.requestedRelayStates[relayIdx - 1] = false;
      }
      xSemaphoreGive(stateMutex);
    }
    saveLocalControlState();
    updateRelayHardware();
  } else {
    enqueueUartCmd(String("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"") +
                   (state ? "relay_lock" : "relay_unlock") + "\"}");
  }
  Serial.println("OK");
}

static bool isValidAcFanValue(const String &fan) {
  return fan == "auto" || fan == "min" || fan == "low" || fan == "med" ||
         fan == "high" || fan == "max";
}

static bool parseAcTempValue(const String &value, int &temp) {
  String s = value;
  s.trim();
  if (s.length() == 0)
    return false;
  for (uint16_t i = 0; i < s.length(); i++) {
    if (!isDigit(s.charAt(i)))
      return false;
  }
  temp = s.toInt();
  return temp >= 16 && temp <= 30;
}

static void printMqttConfig() {
  Serial.printf("enabled=%s\n", g_mqttConfig.enabled ? "ON" : "OFF");
  Serial.printf("broker=%s\n", g_mqttConfig.broker);
  Serial.printf("port=%d\n", g_mqttConfig.port);
  Serial.printf("client=%s\n", g_mqttConfig.clientId);
  Serial.printf("user=%s\n", g_mqttConfig.username);
  Serial.printf("topic=%s\n", g_mqttConfig.baseTopic);
  Serial.printf("keepalive=%d\n", g_mqttConfig.keepAlive);
}

static void handleMqttCommand(const String &cmdRaw, const String &cmdUpper) {
  if (cmdUpper == "MQTT SHOW") {
    printMqttConfig();
    return;
  }
  if (cmdUpper.startsWith("MQTT ENABLE ")) {
    bool enabled = false;
    if (!parseBoolValue(cmdUpper.substring(12), enabled)) {
      Serial.println("ERR: use MQTT ENABLE ON|OFF");
      return;
    }
    g_mqttConfig.enabled = enabled;
  } else if (cmdUpper.startsWith("MQTT SET BROKER ")) {
    strncpy(g_mqttConfig.broker, cmdRaw.substring(16).c_str(), sizeof(g_mqttConfig.broker) - 1);
    g_mqttConfig.broker[sizeof(g_mqttConfig.broker) - 1] = '\0';
  } else if (cmdUpper.startsWith("MQTT SET PORT ")) {
    g_mqttConfig.port = cmdUpper.substring(14).toInt();
  } else if (cmdUpper.startsWith("MQTT SET USER ")) {
    strncpy(g_mqttConfig.username, cmdRaw.substring(14).c_str(), sizeof(g_mqttConfig.username) - 1);
    g_mqttConfig.username[sizeof(g_mqttConfig.username) - 1] = '\0';
  } else if (cmdUpper.startsWith("MQTT SET PASS ")) {
    strncpy(g_mqttConfig.password, cmdRaw.substring(14).c_str(), sizeof(g_mqttConfig.password) - 1);
    g_mqttConfig.password[sizeof(g_mqttConfig.password) - 1] = '\0';
  } else if (cmdUpper.startsWith("MQTT SET KEEPALIVE ")) {
    g_mqttConfig.keepAlive = cmdUpper.substring(19).toInt();
  } else {
    Serial.println("ERR: unknown MQTT command");
    return;
  }
  saveMqttConfig();
  g_mqttConfigChanged = true;
  Serial.println("OK");
}

static void handleSerialCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0)
    return;
  String raw = cmd;
  String upper = cmd;
  upper.toUpperCase();

  int p1 = upper.indexOf(' ');
  String head = p1 < 0 ? upper : upper.substring(0, p1);
  String rest = p1 < 0 ? "" : upper.substring(p1 + 1);

  if (upper == "HELP") {
    printHelp();
  } else if (upper == "STATUS") {
    Serial.println(buildStatusJSON());
  } else if (head == "RELAY") {
    int p2 = rest.indexOf(' ');
    if (p2 < 0) {
      Serial.println("ERR: use RELAY <1-7> ON|OFF");
    } else {
      handleRelayCommand(rest.substring(0, p2).toInt(), rest.substring(p2 + 1));
    }
  } else if (head == "LOCK") {
    int p2 = rest.indexOf(' ');
    if (p2 < 0) {
      Serial.println("ERR: use LOCK <1-7> ON|OFF");
    } else {
      handleLockCommand(rest.substring(0, p2).toInt(), rest.substring(p2 + 1));
    }
  } else if (head == "AC") {
    if (rest == "ON") {
      enqueueUartCmd("{\"t\":\"ac_cmd\",\"cmd\":\"power\",\"val\":\"on\"}");
      Serial.println("OK");
    } else if (rest == "OFF") {
      enqueueUartCmd("{\"t\":\"ac_cmd\",\"cmd\":\"power\",\"val\":\"off\"}");
      Serial.println("OK");
    } else if (rest == "TEMP+") {
      enqueueUartCmd("{\"t\":\"ac_cmd\",\"cmd\":\"temp_step\",\"val\":\"up\"}");
      Serial.println("OK");
    } else if (rest == "TEMP-") {
      enqueueUartCmd("{\"t\":\"ac_cmd\",\"cmd\":\"temp_step\",\"val\":\"down\"}");
      Serial.println("OK");
    } else if (rest.startsWith("TEMP ")) {
      int temp = 0;
      if (!parseAcTempValue(rest.substring(5), temp)) {
        Serial.println("ERR: use AC TEMP <16-30>");
      } else {
        enqueueUartCmd("{\"t\":\"ac_cmd\",\"cmd\":\"temp\",\"val\":" + String(temp) + "}");
        Serial.println("OK");
      }
    } else if (rest.startsWith("FAN ")) {
      String fan = rest.substring(4);
      fan.trim();
      fan.toLowerCase();
      if (!isValidAcFanValue(fan)) {
        Serial.println("ERR: use AC FAN auto|min|low|med|high|max");
      } else {
        enqueueUartCmd("{\"t\":\"ac_cmd\",\"cmd\":\"fan\",\"val\":\"" + fan + "\"}");
        Serial.println("OK");
      }
    } else {
      Serial.println("ERR: use AC ON|OFF, AC TEMP <16-30>, AC TEMP+|TEMP-, or AC FAN auto|min|low|med|high|max");
    }
  } else if (head == "LIGHTS") {
    bool state = false;
    if (parseBoolValue(rest, state)) {
      for (int i = 0; i < 5; i++) {
        setRelayState(i, state);
      }
      Serial.println("OK");
    } else {
      Serial.println("ERR: use LIGHTS ON|OFF");
    }
  } else if (upper == "UNLOCK-ALL") {
    for (int i = 0; i < NUM_RELAYS; i++) {
      setLocalRelayLock(i, false);
    }
    saveLocalControlState();
    updateRelayHardware();
    enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"relay_unlock\"}");
    Serial.println("OK");
  } else if (upper == "SD INFO") {
    printSdInfo();
  } else if (upper == "SD LAST-RECORD") {
    g_sdLastRecordSerialPending = true;
    enqueueUartCmd("{\"t\":\"last_record_req\",\"file\":\"energy_log.csv\"}");
    Serial.println("OK");
  } else if (upper == "CLEAR ENERGY LOGS") {
    g_clearEnergyLogsSerialPending = true;
    enqueueUartCmd("{\"t\":\"clear_energy_logs_req\",\"file\":\"energy_log.csv\"}");
    Serial.println("OK");
  } else if (upper == "WIFI STATUS") {
    Serial.printf("WiFi Status: %s\n", WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("SSID:        %s\n", WiFi.SSID().c_str());
      Serial.printf("IP Address:  %s\n", WiFi.localIP().toString().c_str());
      Serial.printf("RSSI:        %d dBm\n", WiFi.RSSI());
    }
  } else if (upper == "REBOOT") {
    Serial.println("Rebooting master and slave...");
    xTaskCreatePinnedToCore(startSystemRebootTask, "SysReboot", 2048, NULL, 1,
                            NULL, 0);
  } else if (upper == "SHUTDOWN ON") {
    Serial.println("SHUTDOWN: Turning off all relays...");
    for (int i = 0; i < NUM_RELAYS; i++) {
      setRelayState(i, false);
    }
    enqueueUartCmd("{\"t\":\"ac_cmd\",\"cmd\":\"power\",\"val\":\"off\"}");
    enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"relay_off\"}");
    Serial.println("OK");
  } else if (head == "MQTT") {
    handleMqttCommand(raw, upper);
  } else {
    Serial.println("ERR: unknown command. Type HELP");
  }
}

void serialCommandInit() {
  printHelp();
}

void serialCommandLoop() {
  static String line;
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n') {
      handleSerialCommand(line);
      line = "";
    } else if (c != '\r') {
      line += c;
      if (line.length() > 220)
        line = "";
    }
  }
}

static void loadDashCredentials(String& user, String& pass) {
  Preferences prefs;
  prefs.begin("dash_auth", true);
  user = prefs.getString("dash_user", "admin");
  pass = prefs.getString("dash_pass", "smartnest");
  prefs.end();
}

static void saveDashCredentials(const String& user, const String& pass) {
  Preferences prefs;
  prefs.begin("dash_auth", false);
  prefs.putString("dash_user", user);
  prefs.putString("dash_pass", pass);
  prefs.end();
}

static String generateToken() {
  char token[17];
  snprintf(token, sizeof(token), "%08lx%08lx",
           (unsigned long)esp_random(), (unsigned long)esp_random());
  return String(token);
}

static bool isAuthenticated(AsyncWebServerRequest* request) {
  if (g_dashSessionToken.length() == 0 || !request->hasHeader("Cookie"))
    return false;

  String cookie = request->getHeader("Cookie")->value();
  String needle = "sn_session=" + g_dashSessionToken;
  int pos = cookie.indexOf(needle);
  if (pos < 0)
    return false;
  bool startsOk = pos == 0 || cookie.charAt(pos - 1) == ' ' ||
                  cookie.charAt(pos - 1) == ';';
  int end = pos + needle.length();
  bool endsOk = end >= cookie.length() || cookie.charAt(end) == ';';
  return startsOk && endsOk;
}

static void sendJson(AsyncWebServerRequest* request, int code,
                     const String& json) {
  request->send(code, "application/json", json);
}

static void checkHttpPendingTimeouts() {
  uint32_t now = millis();
  if (g_sdLastRecordHttpPending &&
      (now - g_sdLastRecordHttpStartMs) > 8000) {
    if (g_sdLastRecordHttpReq != nullptr) {
      sendJson(g_sdLastRecordHttpReq, 504,
               "{\"ok\":false,\"message\":\"Timeout\"}");
    }
    g_sdLastRecordHttpPending = false;
    g_sdLastRecordHttpReq = nullptr;
  }

  if (g_clearLogsHttpPending &&
      (now - g_clearLogsHttpStartMs) > 8000) {
    if (g_clearLogsHttpReq != nullptr) {
      sendJson(g_clearLogsHttpReq, 504,
               "{\"ok\":false,\"message\":\"Timeout\"}");
    }
    g_clearLogsHttpPending = false;
    g_clearLogsHttpReq = nullptr;
  }
}

static void dashboardInit() {
  pDashServer = new AsyncWebServer(80);

  pDashServer->on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!isAuthenticated(req)) {
      req->redirect("/login");
      return;
    }
    req->send_P(200, "text/html", DASHBOARD_HTML);
  });

  pDashServer->on("/login", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", DASHBOARD_HTML);
  });

  pDashServer->on(
      "/api/login", HTTP_POST, [](AsyncWebServerRequest* req) {},
      NULL,
      [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index,
         size_t total) {
        if (index + len != total)
          return;

        StaticJsonDocument<256> doc;
        if (deserializeJson(doc, data, len)) {
          sendJson(req, 400, "{\"ok\":false,\"message\":\"Invalid JSON\"}");
          return;
        }

        String savedUser, savedPass;
        loadDashCredentials(savedUser, savedPass);
        const char* username = doc["username"] | "";
        const char* password = doc["password"] | "";
        if (savedUser == username && savedPass == password) {
          g_dashSessionToken = generateToken();
          AsyncWebServerResponse* response =
              req->beginResponse(200, "application/json", "{\"ok\":true}");
          response->addHeader("Set-Cookie",
                              "sn_session=" + g_dashSessionToken +
                                  "; Path=/; HttpOnly; SameSite=Strict");
          req->send(response);
        } else {
          sendJson(req, 401,
                   "{\"ok\":false,\"message\":\"Invalid credentials\"}");
        }
      });

  pDashServer->on("/api/logout", HTTP_POST, [](AsyncWebServerRequest* req) {
    g_dashSessionToken = "";
    AsyncWebServerResponse* response =
        req->beginResponse(200, "application/json", "{\"ok\":true}");
    response->addHeader(
        "Set-Cookie",
        "sn_session=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
    req->send(response);
  });

  pDashServer->on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!isAuthenticated(req)) {
      sendJson(req, 401, "{\"ok\":false,\"message\":\"Unauthorized\"}");
      return;
    }
    sendJson(req, 200, buildStatusJSON());
  });

  pDashServer->on(
      "/api/relay", HTTP_POST, [](AsyncWebServerRequest* req) {},
      NULL,
      [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index,
         size_t total) {
        if (index + len != total)
          return;
        if (!isAuthenticated(req)) {
          sendJson(req, 401, "{\"ok\":false,\"message\":\"Unauthorized\"}");
          return;
        }
        StaticJsonDocument<128> doc;
        if (deserializeJson(doc, data, len)) {
          sendJson(req, 400, "{\"ok\":false,\"message\":\"Invalid JSON\"}");
          return;
        }
        int relay = doc["relay"] | 0;
        if (relay < 1 || relay > 7 || !doc["state"].is<bool>()) {
          sendJson(req, 400, "{\"ok\":false,\"message\":\"Invalid\"}");
          return;
        }
        bool state = doc["state"];
        handleRelayCommand(relay, state ? "ON" : "OFF");
        sendJson(req, 200, "{\"ok\":true}");
      });

  pDashServer->on(
      "/api/lock", HTTP_POST, [](AsyncWebServerRequest* req) {},
      NULL,
      [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index,
         size_t total) {
        if (index + len != total)
          return;
        if (!isAuthenticated(req)) {
          sendJson(req, 401, "{\"ok\":false,\"message\":\"Unauthorized\"}");
          return;
        }
        StaticJsonDocument<128> doc;
        if (deserializeJson(doc, data, len)) {
          sendJson(req, 400, "{\"ok\":false,\"message\":\"Invalid JSON\"}");
          return;
        }
        int relay = doc["relay"] | 0;
        if (relay < 1 || relay > 7 || !doc["locked"].is<bool>()) {
          sendJson(req, 400, "{\"ok\":false,\"message\":\"Invalid\"}");
          return;
        }
        bool locked = doc["locked"];
        handleLockCommand(relay, locked ? "ON" : "OFF");
        sendJson(req, 200, "{\"ok\":true}");
      });

  pDashServer->on(
      "/api/lights", HTTP_POST, [](AsyncWebServerRequest* req) {},
      NULL,
      [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index,
         size_t total) {
        if (index + len != total)
          return;
        if (!isAuthenticated(req)) {
          sendJson(req, 401, "{\"ok\":false,\"message\":\"Unauthorized\"}");
          return;
        }
        StaticJsonDocument<128> doc;
        if (deserializeJson(doc, data, len) || !doc["state"].is<bool>()) {
          sendJson(req, 400, "{\"ok\":false,\"message\":\"Invalid\"}");
          return;
        }
        bool state = doc["state"];
        for (int i = 0; i < 5; i++) {
          setRelayState(i, state);
        }
        sendJson(req, 200, "{\"ok\":true}");
      });

  pDashServer->on("/api/shutdown", HTTP_POST,
                  [](AsyncWebServerRequest* req) {
                    if (!isAuthenticated(req)) {
                      sendJson(req, 401,
                               "{\"ok\":false,\"message\":\"Unauthorized\"}");
                      return;
                    }
                    for (int i = 0; i < NUM_RELAYS; i++) {
                      setRelayState(i, false);
                    }
                    enqueueUartCmd(
                        "{\"t\":\"ac_cmd\",\"cmd\":\"power\",\"val\":\"off\"}");
                    enqueueUartCmd(
                        "{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"relay_off\"}");
                    sendJson(req, 200, "{\"ok\":true}");
                  });

  pDashServer->on("/api/unlock-all", HTTP_POST,
                  [](AsyncWebServerRequest* req) {
                    if (!isAuthenticated(req)) {
                      sendJson(req, 401,
                               "{\"ok\":false,\"message\":\"Unauthorized\"}");
                      return;
                    }
                    for (int i = 0; i < NUM_RELAYS; i++) {
                      setLocalRelayLock(i, false);
                    }
                    saveLocalControlState();
                    updateRelayHardware();
                    enqueueUartCmd(
                        "{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"relay_unlock\"}");
                    sendJson(req, 200, "{\"ok\":true}");
                  });

  pDashServer->on(
      "/api/ac", HTTP_POST, [](AsyncWebServerRequest* req) {},
      NULL,
      [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index,
         size_t total) {
        if (index + len != total)
          return;
        if (!isAuthenticated(req)) {
          sendJson(req, 401, "{\"ok\":false,\"message\":\"Unauthorized\"}");
          return;
        }
        StaticJsonDocument<160> doc;
        if (deserializeJson(doc, data, len)) {
          sendJson(req, 400, "{\"ok\":false,\"message\":\"Invalid\"}");
          return;
        }

        String cmd = doc["cmd"] | "";
        cmd.toLowerCase();
        String serialCmd = "";
        if (cmd == "power") {
          String val = doc["val"] | "";
          val.toLowerCase();
          if (val == "on")
            serialCmd = "AC ON";
          else if (val == "off")
            serialCmd = "AC OFF";
        } else if (cmd == "temp") {
          int temp = doc["val"] | 0;
          if (temp >= 16 && temp <= 30)
            serialCmd = "AC TEMP " + String(temp);
        } else if (cmd == "temp_step") {
          String val = doc["val"] | "";
          val.toLowerCase();
          if (val == "up")
            serialCmd = "AC TEMP+";
          else if (val == "down")
            serialCmd = "AC TEMP-";
        } else if (cmd == "fan") {
          String fan = doc["val"] | "";
          fan.toLowerCase();
          if (isValidAcFanValue(fan))
            serialCmd = "AC FAN " + fan;
        }

        if (serialCmd.length() == 0) {
          sendJson(req, 400, "{\"ok\":false,\"message\":\"Invalid\"}");
          return;
        }
        handleSerialCommand(serialCmd);
        sendJson(req, 200, "{\"ok\":true}");
      });





  pDashServer->on("/api/wifi-status", HTTP_GET,
                  [](AsyncWebServerRequest* req) {
                    if (!isAuthenticated(req)) {
                      sendJson(req, 401,
                               "{\"ok\":false,\"message\":\"Unauthorized\"}");
                      return;
                    }
                    String json = "{";
                    json += "\"connected\":" +
                            String(WiFi.status() == WL_CONNECTED ? "true"
                                                                  : "false");
                    json += ",\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\"";
                    json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
                    json += ",\"rssi\":" + String(WiFi.RSSI()) + "}";
                    sendJson(req, 200, json);
                  });

  pDashServer->on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!isAuthenticated(req)) {
      sendJson(req, 401, "{\"ok\":false,\"message\":\"Unauthorized\"}");
      return;
    }
    xTaskCreatePinnedToCore(startSystemRebootTask, "SysReboot", 2048, NULL, 1,
                            NULL, 0);
    sendJson(req, 200, "{\"ok\":true}");
  });

  pDashServer->on("/api/mqtt-show", HTTP_GET,
                  [](AsyncWebServerRequest* req) {
                    if (!isAuthenticated(req)) {
                      sendJson(req, 401,
                               "{\"ok\":false,\"message\":\"Unauthorized\"}");
                      return;
                    }
                    String json = "{";
                    json += "\"enabled\":" +
                            String(g_mqttConfig.enabled ? "true" : "false");
                    json += ",\"broker\":\"" + jsonEscape(g_mqttConfig.broker) +
                            "\"";
                    json += ",\"port\":" + String(g_mqttConfig.port);
                    json += ",\"username\":\"" +
                            jsonEscape(g_mqttConfig.username) + "\"";
                    json += ",\"topic\":\"" +
                            jsonEscape(g_mqttConfig.baseTopic) + "\"";
                    json += ",\"keepalive\":" +
                            String(g_mqttConfig.keepAlive) + "}";
                    sendJson(req, 200, json);
                  });

  pDashServer->on(
      "/api/mqtt", HTTP_POST, [](AsyncWebServerRequest* req) {},
      NULL,
      [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index,
         size_t total) {
        if (index + len != total)
          return;
        if (!isAuthenticated(req)) {
          sendJson(req, 401, "{\"ok\":false,\"message\":\"Unauthorized\"}");
          return;
        }
        StaticJsonDocument<512> doc;
        if (deserializeJson(doc, data, len)) {
          sendJson(req, 400, "{\"ok\":false,\"message\":\"Invalid JSON\"}");
          return;
        }
        if (doc.containsKey("enabled")) {
          String raw = String("MQTT ENABLE ") +
                       ((doc["enabled"] | false) ? "ON" : "OFF");
          String upper = raw;
          upper.toUpperCase();
          handleMqttCommand(raw, upper);
        }
        if (doc.containsKey("broker")) {
          String raw = String("MQTT SET BROKER ") +
                       String((const char*)doc["broker"]);
          String upper = raw;
          upper.toUpperCase();
          handleMqttCommand(raw, upper);
        }
        if (doc.containsKey("port")) {
          String raw = "MQTT SET PORT " + String((int)(doc["port"] | 0));
          String upper = raw;
          upper.toUpperCase();
          handleMqttCommand(raw, upper);
        }
        if (doc.containsKey("username")) {
          String raw = String("MQTT SET USER ") +
                       String((const char*)doc["username"]);
          String upper = raw;
          upper.toUpperCase();
          handleMqttCommand(raw, upper);
        }
        if (doc.containsKey("password")) {
          String raw = String("MQTT SET PASS ") +
                       String((const char*)doc["password"]);
          String upper = raw;
          upper.toUpperCase();
          handleMqttCommand(raw, upper);
        }
        if (doc.containsKey("keepalive")) {
          String raw =
              "MQTT SET KEEPALIVE " + String((int)(doc["keepalive"] | 0));
          String upper = raw;
          upper.toUpperCase();
          handleMqttCommand(raw, upper);
        }
        sendJson(req, 200, "{\"ok\":true}");
      });

  pDashServer->on(
      "/api/change-credentials", HTTP_POST,
      [](AsyncWebServerRequest* req) {}, NULL,
      [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index,
         size_t total) {
        if (index + len != total)
          return;
        if (!isAuthenticated(req)) {
          sendJson(req, 401, "{\"ok\":false,\"message\":\"Unauthorized\"}");
          return;
        }
        StaticJsonDocument<256> doc;
        if (deserializeJson(doc, data, len)) {
          sendJson(req, 400, "{\"ok\":false,\"message\":\"Invalid JSON\"}");
          return;
        }
        String currentUser, currentPass;
        loadDashCredentials(currentUser, currentPass);
        String currentPassInput = doc["current_pass"] | "";
        String newUser = doc["new_user"] | "";
        String newPass = doc["new_pass"] | "";
        if (currentPassInput != currentPass) {
          sendJson(req, 403, "{\"ok\":false,\"message\":\"Wrong password\"}");
          return;
        }
        if (newUser.length() == 0 || newPass.length() < 4) {
          sendJson(req, 400, "{\"ok\":false,\"message\":\"Invalid input\"}");
          return;
        }
        saveDashCredentials(newUser, newPass);
        g_dashSessionToken = "";
        sendJson(req, 200,
                 "{\"ok\":true,\"message\":\"Credentials updated\"}");
      });

  pDashServer->onNotFound([](AsyncWebServerRequest* req) {
    if (!isAuthenticated(req)) {
      req->redirect("/login");
      return;
    }
    req->send(404, "text/plain", "Not found");
  });

  pDashServer->begin();
  MDNS.addService("http", "tcp", 80);
  Serial.println("[DASHBOARD] Server started at http://smart-nest.local");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  g_resetReason = resetReasonToString(esp_reset_reason());
  Serial.printf("[RESET] SmartNest reset_reason=%s\n", g_resetReason.c_str());

  memset(&sysState, 0, sizeof(SystemState));
  strncpy(sysState.resetReason, g_resetReason.c_str(), sizeof(sysState.resetReason) - 1);
  strncpy(sysState.timeSource, "NONE", sizeof(sysState.timeSource) - 1);
  loadMqttConfig();

  stateMutex = xSemaphoreCreateMutex();
  UartCmdQueue = xQueueCreate(5, sizeof(UartCmdItem));

  if (!stateMutex || !UartCmdQueue) {
    delay(1000);
    ESP.restart();
  }

  serialCommandInit();

  xTaskCreatePinnedToCore(relaySwitchTask, "RelaySwitch", 4096, NULL, 2,
                          &hRelaySwitch, 1);

  xTaskCreatePinnedToCore(currentSensorTask, "CurrentSensor", 4096, NULL, 1,
                          &hCurrentSensor, 1);

  xTaskCreatePinnedToCore(dhtTask, "DHT11", 4096, NULL, 1, NULL, 1);

  xTaskCreatePinnedToCore(resetButtonTask, "ResetBtn", 2048, NULL, 1,
                          &hResetButton, 0);

  bool wifiConnected = wifiManagerInit();

  if (wifiConnected && !isProvisioningMode) {
    dashboardInit();
  }

  xTaskCreatePinnedToCore(uartCommTask, "UartComm", 8192, NULL, 1, &hUartComm,
                          0);

  if (wifiConnected) {
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
  serialCommandLoop();
  checkHttpPendingTimeouts();

  if (isProvisioningMode) {
    wifiManagerLoop();
    vTaskDelay(pdMS_TO_TICKS(10));
  } else {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
