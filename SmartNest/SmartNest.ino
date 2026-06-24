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

#define NUM_RELAYS 6

#define RELAY_1_PIN 2
#define RELAY_2_PIN 4
#define RELAY_3_PIN 5
#define RELAY_4_PIN 19
#define RELAY_5_PIN 18
#define RELAY_6_PIN 15

#define ACS712_PIN 34
#define ACS712_DIVIDER_RATIO 1.5f
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
#define DASHBOARD_NVS_NAMESPACE "dashboard"
#define DASHBOARD_PASS_KEY "dash_pass"
#define DASHBOARD_USER "admin"
#define DASHBOARD_PASS "smartnest"
#define DASHBOARD_SESSION_MS 900000UL
#define UART_RX_BUFFER_MAX 4096

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
#define RESTORE_RELAYS_ON_BOOT false

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

  bool digitalRelayState;
  bool digitalRelayLocked;
  bool digitalSwitchState;

  bool sdOk;
  uint64_t sdTotal;
  uint64_t sdUsed;

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

#if MQTT_ENABLED
WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);
#endif

DHT dht(DHT_PIN, DHT_TYPE);

void setRelayState(int index, bool state);
void toggleRelay(int index);
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

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (state && sysState.lockedStates[index]) {
      Serial.printf("[LOCK] setRelayState(%d, ON) blocked due to relay lock\n",
                    index);
    } else {
      sysState.requestedRelayStates[index] = state;
      xSemaphoreGive(stateMutex);
      saveLocalControlState();
      updateRelayHardware();
      return;
    }
    xSemaphoreGive(stateMutex);
  }

  updateRelayHardware();
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

void toggleRelay(int index) {
  bool currentCmd = false;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    currentCmd = sysState.requestedRelayStates[index];
    xSemaphoreGive(stateMutex);
  }
  setRelayState(index, !currentCmd);
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
    float deltaMv = (milliVolts - acs712ZeroMv) * ACS712_DIVIDER_RATIO;
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
static String g_dashboardToken = "";
static uint32_t g_dashboardTokenUntil = 0;

#include "provision_html.h"

static const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SmartNest Dashboard</title>
<style>
*{box-sizing:border-box}body{margin:0;font-family:system-ui,Segoe UI,Arial,sans-serif;background:#f4f6f8;color:#111827}.wrap{max-width:1120px;margin:0 auto;padding:18px}header{display:flex;justify-content:space-between;gap:12px;align-items:center;margin-bottom:14px}h1{font-size:24px;margin:0;}h3{margin:0 0 10px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(165px,1fr));gap:10px}.card{background:#fff;border:1px solid #d9dee7;border-radius:8px;padding:12px;min-width:0}.label{font-size:12px;color:#5b6472}.val{font-size:21px;font-weight:700;margin-top:4px}.ok{color:#087443}.warn{color:#b54708}.bad{color:#b42318}.controls{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:10px;margin-top:10px}button,select,input{font:inherit;border-radius:6px;min-width:0}button{border:0;background:#174ea6;color:#fff;font-weight:700;min-height:50px;padding:9px 10px;cursor:pointer;white-space:normal;line-height:1.15}button.secondary{background:#eef2f7;color:#111827;border:1px solid #c8d0dc}button.danger{background:#b42318}select,input{border:1px solid #c8d0dc;padding:9px;width:100%;background:#fff;min-height:40px}.row{display:grid;gap:8px;align-items:stretch}.cmdrow{grid-template-columns:minmax(120px,1.4fr) minmax(92px,1fr) minmax(68px,.6fr)}.btnrow{grid-template-columns:repeat(3,minmax(0,1fr))}.two{grid-template-columns:repeat(2,minmax(0,1fr))}.mqttgrid{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:8px}.readonlybox{border:1px solid #c8d0dc;border-radius:6px;background:#eef2f7;padding:9px;min-height:40px;word-break:break-word}.topiclist{display:grid;gap:4px;margin-top:8px}.topiclist div{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:12px;background:#f8fafc;border:1px solid #d9dee7;border-radius:6px;padding:7px;word-break:break-word}.wide{grid-column:1/-1}pre{white-space:pre-wrap;word-break:break-word;background:#0b1220;color:#d7e1f2;border-radius:8px;padding:12px;min-height:240px;max-height:520px;overflow:auto}.relays{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:8px}.relay{border:1px solid #d9dee7;border-radius:8px;padding:10px;background:#fff}.relay b{display:block;margin-bottom:8px}.mini{font-size:12px;color:#5b6472}.topbtn{width:auto;min-height:40px}.login{max-width:360px;margin:12vh auto}.hidden{display:none}
</style></head><body>
<div id="loginPanel" class="login card"><h1 style="text-align:center;">SmartNest</h1><br><form id="loginForm"><input id="user" placeholder="User ID" autocomplete="username"><input id="pass" type="password" placeholder="Password" autocomplete="current-password" style="margin-top:8px"><button id="loginBtn" style="width:100%;margin-top:10px" type="submit">Login</button></form><div id="loginMsg" class="mini"></div></div>
<div id="app" class="wrap hidden">
<header><div><h1>SmartNest Dashboard</h1><div class="mini" id="stamp">Loading...</div></div><div><button class="secondary topbtn" onclick="refresh()">Refresh</button></div></header>
<section class="grid" id="metrics"></section>
<section class="card wide" style="margin-top:10px"><h3>Signal Strength</h3><div class="grid" id="rssi"></div></section>
<section class="card wide" style="margin-top:10px"><h3>Relays</h3><div class="relays" id="relays"></div></section>
<section class="controls">
<div class="card"><h3>Relay Command</h3><div class="row cmdrow"><select id="relayNo"></select><select id="relayAction"><option>ON</option><option>OFF</option><option>TOGGLE</option></select><button onclick="cmd('RELAY '+relayNo.value+' '+relayAction.value)">Send</button></div></div>
<div class="card"><h3>Main Relays</h3><div class="row two"><button id="allOnBtn" onclick="allLocalRelays(true)">All ON</button><button id="allOffBtn" class="secondary" onclick="allLocalRelays(false)">All OFF</button></div></div>
<div class="card"><h3>Lock Command</h3><div class="row cmdrow"><select id="lockNo"></select><select id="lockAction"><option>ON</option><option>OFF</option></select><button onclick="cmd('LOCK '+lockNo.value+' '+lockAction.value)">Send</button></div></div>
<div class="card"><h3>All Locks</h3><div class="row two"><button id="lockAllBtn" onclick="allRelayLocks(true)">Lock All</button><button id="unlockAllBtn" class="secondary" onclick="allRelayLocks(false)">Unlock All</button></div></div>
<div class="card"><h3>Slave</h3><div class="row btnrow"><button onclick="cmd('SLAVE D1 reboot')">Digital Reboot</button><button onclick="cmd('SLAVE PZEM reboot')">PZEM Reboot</button><button class="danger" onclick="cmd('SLAVE PZEM energy_reset')">Energy Reset</button></div></div>
<div class="card"><h3>System</h3><div class="row two"><button onclick="apiText('/api/sd')">SD Info</button><button onclick="apiText('/api/status?pretty=1')">Status</button></div></div>
<div class="card"><h3>Password</h3><div class="row"><input id="oldPass" type="password" placeholder="Old password" autocomplete="current-password"><input id="newPass" type="password" placeholder="New password" autocomplete="new-password"><input id="newPass2" type="password" placeholder="Confirm new password" autocomplete="new-password"><button onclick="changePassword()">Change Password</button></div></div>
<div class="card wide"><h3>MQTT Settings</h3><div class="mqttgrid"><select id="mqttEnabled"><option value="1">Enabled</option><option value="0">Disabled</option></select><input id="mqttBroker" placeholder="Broker"><input id="mqttPort" placeholder="Port" inputmode="numeric"><input id="mqttUser" placeholder="Username"><input id="mqttPass" placeholder="Password"><input id="mqttKeepalive" placeholder="Keepalive seconds" inputmode="numeric"></div><div style="margin-top:8px"><div class="label">Client ID</div><div id="mqttClient" class="readonlybox"></div><div class="label" style="margin-top:8px">Base topic</div><div id="mqttTopic" class="readonlybox"></div><div class="label" style="margin-top:8px">Publish topics</div><div id="mqttPublishTopics" class="topiclist"></div><div class="label" style="margin-top:8px">Subscribe topics</div><div id="mqttSubscribeTopics" class="topiclist"></div></div><div class="row btnrow" style="margin-top:8px"><button onclick="loadMqtt()">Load MQTT</button><button onclick="saveMqtt()">Save MQTT</button><button class="danger" onclick="cmd('RESET MQTT').then(loadMqtt)">Reset MQTT</button></div></div>
<div class="card wide"><h3>Command Output</h3><pre id="out"></pre></div>
</section></div>
<script>
for(let i=1;i<=7;i++){relayNo.add(new Option('Relay '+i,i));lockNo.add(new Option('Relay '+i,i))}
const token=()=>sessionStorage.getItem('sn_token')||'';const ah=()=>({'X-SN-Token':token()});
function cell(k,v,c=''){return `<div class="card"><div class="label">${k}</div><div class="val ${c}">${v}</div></div>`}
function b(v){return v?'ON':'OFF'}function bytes(n){if(!n)return'0 B';let u=['B','KB','MB','GB'],i=0;while(n>1024&&i<u.length-1){n/=1024;i++}return n.toFixed(i?1:0)+' '+u[i]}
function num(v,u='',d=null){let n=Number(v);return Number.isFinite(n)?(d===null?String(v):n.toFixed(d))+u:'N/A'}
function hasNum(v){return Number.isFinite(Number(v))}
function displayVoltage(s){if(s.voltage_estimated===true)return hasNum(s.energy_voltage)?num(s.energy_voltage,' V',1)+' est':'N/A';return hasNum(s.voltage)?num(s.voltage,' V',1):'N/A'}
function arr(a,i,fb=false){return Array.isArray(a)&&i<a.length?a[i]:fb}
const sleep=ms=>new Promise(r=>setTimeout(r,ms));let allSeq=false;
function quality(r){if(r===null||r===undefined||r===0)return['N/A','bad'];if(r>=-50)return['Outstanding','ok'];if(r>=-60)return['Excellent','ok'];if(r>=-70)return['Good','ok'];if(r>=-80)return['Fair','warn'];return['Weak','bad']}
let loginBusy=false;
async function doLogin(){if(loginBusy)return;let loginButton=document.getElementById('loginBtn');loginBusy=true;if(loginButton)loginButton.disabled=true;let u=document.getElementById('user').value,p=document.getElementById('pass').value,m=document.getElementById('loginMsg');m.textContent='';try{let d=new URLSearchParams();d.set('user',u);d.set('pass',p);let r=await fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:d});if(!r.ok){m.textContent='Invalid login';return}let j=await r.json();sessionStorage.setItem('sn_token',j.token);document.getElementById('loginPanel').classList.add('hidden');document.getElementById('app').classList.remove('hidden');refresh();loadMqtt()}finally{loginBusy=false;if(loginButton)loginButton.disabled=false}}
document.getElementById('loginForm').addEventListener('submit',e=>{e.preventDefault();doLogin()});
async function logout(){let t=token();sessionStorage.removeItem('sn_token');if(t)navigator.sendBeacon('/api/logout?token='+encodeURIComponent(t));app.classList.add('hidden');loginPanel.classList.remove('hidden')}
async function refresh(){try{let r=await fetch('/api/status',{cache:'no-store',headers:ah()});if(r.status===401){logout();return null}if(!r.ok){out.textContent='Status error: HTTP '+r.status;return null}let s=await r.json();stamp.textContent=(s.time||'N/A')+' | uptime '+Math.floor((Number(s.uptime)||0)/1000)+'s';
metrics.innerHTML=cell('Temperature',s.dht_ok?num(s.temp_c,' C',1):'N/A',s.dht_ok?'ok':'bad')+cell('Humidity',s.dht_ok?num(s.humidity,' %',1):'N/A',s.dht_ok?'ok':'bad')+cell('Main board current',num(s.load,' A',2))+cell('Main board energy',num(s.main_energy,' kWh',3))+cell('Digital board current',num(s.acs,' A',2))+cell('Digital board energy',num(s.digital_energy,' kWh',3))+cell('AC current',num(s.ac_current,' A',3))+cell('AC power',num(s.ac_power,' W',1))+cell('AC daily energy',num(s.ac_energy,' kWh',3))+cell('PZEM cumulative',num(s.pzem_energy_cumulative,' kWh',3))+cell('Time source',s.time_source||'N/A',s.time_source==='ESTIMATED'?'warn':'ok')+cell('Voltage',displayVoltage(s),s.voltage_estimated===true?'warn':'ok')+cell('SD',s.sd_ok?'OK':'ERROR',s.sd_ok?'ok':'bad')+cell('SD free',bytes((Number(s.sd_total)||0)-(Number(s.sd_used)||0)))+cell('Digital board',s.d_on?'ONLINE':'OFFLINE',s.d_on?'ok':'bad')+cell('PZEM board',s.p_on?'ONLINE':'OFFLINE',s.p_on?'ok':'bad')+cell('All locked',b(s.m_lock),s.m_lock?'bad':'ok');
let q1=quality(s.rssi),q2=quality(s.d_rssi),q3=quality(s.p_rssi);rssi.innerHTML=cell('Router to SmartNest',num(s.rssi,' dBm')+' '+q1[0],q1[1])+cell('Digital Board RSSI',s.d_on?num(s.d_rssi,' dBm')+' '+q2[0]:'N/A',s.d_on?q2[1]:'bad')+cell('PZEM Board RSSI',s.p_on?num(s.p_rssi,' dBm')+' '+q3[0]:'N/A',s.p_on?q3[1]:'bad');
let h='';for(let i=0;i<6;i++){let locked=arr(s.locks,i);h+=`<div class="relay"><b>Relay ${i+1}</b><div>State: ${b(arr(s.relays,i))}</div><div>Lock: ${b(locked)}</div><div>Runtime: ${num(arr(s.relay_runtime,i,0),'s')}</div></div>`}let dLocked=s.d_lock;h+=`<div class="relay"><b>Relay 7</b><div>State: ${b(s.d_relay)}</div><div>Lock: ${b(dLocked)}</div><div>Runtime: ${num(arr(s.relay_runtime,6,0),'s')}</div></div>`;relays.innerHTML=h;return s}catch(e){out.textContent='Status unavailable: '+e.message;return null}}
async function cmd(c){c=(c||'').trim();if(!c)return false;out.textContent='Sending: '+c;try{let d=new URLSearchParams();d.set('cmd',c);let r=await fetch('/api/command',{method:'POST',headers:{...ah(),'Content-Type':'application/x-www-form-urlencoded'},body:d});if(r.status===401){logout();return false}out.textContent=await r.text();setTimeout(refresh,500);return r.ok}catch(e){out.textContent='Command failed: '+e.message;return false}}
async function allLocalRelays(on){if(allSeq)return;let s=await refresh();if(!s)return;allSeq=true;allOnBtn.disabled=true;allOffBtn.disabled=true;lockAllBtn.disabled=true;unlockAllBtn.disabled=true;try{for(let i=1;i<=6;i++){let ok=await cmd('RELAY '+i+' '+(on?'ON':'OFF'));if(!ok)break;if(i<6)await sleep(500)}}finally{allSeq=false;allOnBtn.disabled=false;allOffBtn.disabled=false;lockAllBtn.disabled=false;unlockAllBtn.disabled=false;refresh()}}
async function allRelayLocks(locked){if(allSeq)return;allSeq=true;allOnBtn.disabled=true;allOffBtn.disabled=true;lockAllBtn.disabled=true;unlockAllBtn.disabled=true;try{for(let i=1;i<=7;i++){let ok=await cmd('LOCK '+i+' '+(locked?'ON':'OFF'));if(!ok)break;if(i<7)await sleep(400)}}finally{allSeq=false;allOnBtn.disabled=false;allOffBtn.disabled=false;lockAllBtn.disabled=false;unlockAllBtn.disabled=false;refresh()}}
async function apiText(url,opt={}){try{opt.headers={...(opt.headers||{}),...ah()};let r=await fetch(url,opt);if(r.status===401){logout();return}out.textContent=await r.text();setTimeout(refresh,500)}catch(e){out.textContent='Request failed: '+e.message}}
async function changePassword(){let d=new URLSearchParams();d.set('old',oldPass.value);d.set('new',newPass.value);d.set('confirm',newPass2.value);let r=await fetch('/api/password',{method:'POST',headers:{...ah(),'Content-Type':'application/x-www-form-urlencoded'},body:d});out.textContent=await r.text();oldPass.value='';newPass.value='';newPass2.value='';if(r.ok)setTimeout(logout,800)}
function topicItems(a){return(Array.isArray(a)?a:[]).map(t=>`<div>${t}</div>`).join('')}
async function loadMqtt(){try{let r=await fetch('/api/mqtt',{headers:ah(),cache:'no-store'});if(r.status===401){logout();return}if(!r.ok){out.textContent=await r.text();return}let m=await r.json();mqttEnabled.value=m.enabled?'1':'0';mqttBroker.value=m.broker||'';mqttPort.value=m.port||'';mqttClient.textContent=m.client||'';mqttUser.value=m.user||'';mqttPass.value=m.pass||'';mqttTopic.textContent=m.baseTopic||m.topic||'';mqttPublishTopics.innerHTML=topicItems(m.publish_topics);mqttSubscribeTopics.innerHTML=topicItems(m.subscribe_topics);mqttKeepalive.value=m.keepalive||'';out.textContent='MQTT settings loaded.'}catch(e){out.textContent='MQTT load failed: '+e.message}}
async function saveMqtt(){try{let d=new URLSearchParams();d.set('enabled',mqttEnabled.value);d.set('broker',mqttBroker.value);d.set('port',mqttPort.value);d.set('user',mqttUser.value);d.set('pass',mqttPass.value);d.set('keepalive',mqttKeepalive.value);let r=await fetch('/api/mqtt',{method:'POST',headers:{...ah(),'Content-Type':'application/x-www-form-urlencoded'},body:d});if(r.status===401){logout();return}out.textContent=await r.text();loadMqtt();setTimeout(refresh,500)}catch(e){out.textContent='MQTT save failed: '+e.message}}
if(token()){loginPanel.classList.add('hidden');app.classList.remove('hidden');refresh();loadMqtt()}setInterval(()=>{if(token())refresh()},5000);
</script></body></html>
)rawliteral";

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

static String getDashboardToken(AsyncWebServerRequest *req) {
  if (req->hasHeader("X-SN-Token"))
    return req->getHeader("X-SN-Token")->value();
  if (req->hasParam("token"))
    return req->getParam("token")->value();
  return "";
}

static String getDashboardPassword() {
  Preferences prefs;
  prefs.begin(DASHBOARD_NVS_NAMESPACE, true);
  String password = prefs.getString(DASHBOARD_PASS_KEY, DASHBOARD_PASS);
  prefs.end();
  return password;
}

static void saveDashboardPassword(const String &password) {
  Preferences prefs;
  prefs.begin(DASHBOARD_NVS_NAMESPACE, false);
  prefs.putString(DASHBOARD_PASS_KEY, password);
  prefs.end();
}

static bool dashboardAuth(AsyncWebServerRequest *req) {
  String token = getDashboardToken(req);
  uint32_t now = millis();
  if (token.length() > 0 && token == g_dashboardToken &&
      (int32_t)(g_dashboardTokenUntil - now) > 0) {
    g_dashboardTokenUntil = now + DASHBOARD_SESSION_MS;
    return true;
  }
  req->send(401, "text/plain", "ERR: login required");
  return false;
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

static String buildMqttConfigJSON() {
  String base = String(g_mqttConfig.baseTopic);
  String json = "{";
  json += "\"enabled\":" + String(g_mqttConfig.enabled ? "true" : "false") + ",";
  json += "\"broker\":\"" + jsonEscape(g_mqttConfig.broker) + "\",";
  json += "\"port\":" + String(g_mqttConfig.port) + ",";
  json += "\"client\":\"" + jsonEscape(g_mqttConfig.clientId) + "\",";
  json += "\"user\":\"" + jsonEscape(g_mqttConfig.username) + "\",";
  json += "\"pass\":\"" + jsonEscape(g_mqttConfig.password) + "\",";
  json += "\"topic\":\"" + jsonEscape(g_mqttConfig.baseTopic) + "\",";
  json += "\"baseTopic\":\"" + jsonEscape(g_mqttConfig.baseTopic) + "\",";
  json += "\"client_readonly\":true,";
  json += "\"topic_readonly\":true,";
  json += "\"publish_topics\":[";
  json += "\"" + jsonEscape(base + "/live/sensors") + "\",";
  json += "\"" + jsonEscape(base + "/live/relays") + "\",";
  json += "\"" + jsonEscape(base + "/live/status") + "\",";
  json += "\"" + jsonEscape(base + "/history/batch") + "\",";
  json += "\"" + jsonEscape(base + "/cmd/ack") + "\"";
  json += "],";
  json += "\"subscribe_topics\":[";
  json += "\"" + jsonEscape(base + "/cmd/request") + "\",";
  json += "\"" + jsonEscape(base + "/history/ack") + "\"";
  json += "],";
  json += "\"keepalive\":" + String(g_mqttConfig.keepAlive);
  json += "}";
  return json;
}

static void setupDashboardRoutes() {
  pProvServer->on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send_P(200, "text/html", DASHBOARD_HTML);
  });

  pProvServer->on("/api/login", HTTP_POST, [](AsyncWebServerRequest *req) {
    String user = req->hasParam("user", true) ? req->getParam("user", true)->value() : "";
    String pass = req->hasParam("pass", true) ? req->getParam("pass", true)->value() : "";
    if (user != DASHBOARD_USER || pass != getDashboardPassword()) {
      req->send(401, "text/plain", "ERR: invalid login");
      return;
    }
    g_dashboardToken = String((uint32_t)esp_random(), HEX) + String(millis(), HEX);
    g_dashboardTokenUntil = millis() + DASHBOARD_SESSION_MS;
    req->send(200, "application/json", "{\"token\":\"" + g_dashboardToken + "\"}");
  });

  pProvServer->on("/api/logout", HTTP_ANY, [](AsyncWebServerRequest *req) {
    String token = getDashboardToken(req);
    if (token == g_dashboardToken) {
      g_dashboardToken = "";
      g_dashboardTokenUntil = 0;
    }
    req->send(200, "text/plain", "OK");
  });

  pProvServer->on("/api/password", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!dashboardAuth(req))
      return;

    String oldPass = req->hasParam("old", true) ? req->getParam("old", true)->value() : "";
    String newPass = req->hasParam("new", true) ? req->getParam("new", true)->value() : "";
    String confirmPass = req->hasParam("confirm", true) ? req->getParam("confirm", true)->value() : "";

    if (oldPass != getDashboardPassword()) {
      req->send(403, "text/plain", "ERR: old password is incorrect");
      return;
    }
    if (newPass.length() < 4 || newPass.length() > 63) {
      req->send(400, "text/plain", "ERR: new password must be 4-63 characters");
      return;
    }
    if (newPass != confirmPass) {
      req->send(400, "text/plain", "ERR: new password confirmation does not match");
      return;
    }

    saveDashboardPassword(newPass);
    g_dashboardToken = "";
    g_dashboardTokenUntil = 0;
    req->send(200, "text/plain", "Password changed. Login again.");
  });

  pProvServer->on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!dashboardAuth(req))
      return;
    if (req->hasParam("pretty")) {
      req->send(200, "application/json", buildStatusJSON());
      return;
    }
    req->send(200, "application/json", buildStatusJSON());
  });

  pProvServer->on("/api/command", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!dashboardAuth(req))
      return;
    if (!req->hasParam("cmd", true)) {
      req->send(400, "text/plain", "ERR: cmd required");
      return;
    }
    String cmd = req->getParam("cmd", true)->value();
    cmd.trim();
    if (cmd.length() == 0 || cmd.length() > 160) {
      req->send(400, "text/plain", "ERR: invalid command");
      return;
    }
    String upper = cmd;
    upper.toUpperCase();
    if (upper == "STATUS") {
      req->send(200, "application/json", buildStatusJSON());
      return;
    }
    if (upper == "SD INFO") {
      req->send(200, "text/plain", formatSdInfoText());
      return;
    }
    if (upper.startsWith("MQTT SET TOPIC ")) {
      req->send(400, "text/plain", "ERR: MQTT topic is fixed/read-only");
      return;
    }
    if (upper.startsWith("MQTT SET CLIENT ")) {
      req->send(400, "text/plain", "ERR: MQTT client ID is fixed/read-only");
      return;
    }
    handleSerialCommand(cmd);
    req->send(200, "text/plain", "OK: " + cmd);
  });

  pProvServer->on("/api/sd", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!dashboardAuth(req))
      return;
    req->send(200, "text/plain", formatSdInfoText());
  });

  pProvServer->on("/api/mqtt", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!dashboardAuth(req))
      return;
    req->send(200, "application/json", buildMqttConfigJSON());
  });

  pProvServer->on("/api/mqtt", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!dashboardAuth(req))
      return;

    bool enabled = req->hasParam("enabled", true) && req->getParam("enabled", true)->value() == "1";
    String broker = req->hasParam("broker", true) ? req->getParam("broker", true)->value() : "";
    int port = req->hasParam("port", true) ? req->getParam("port", true)->value().toInt() : 0;
    String client = req->hasParam("client", true) ? req->getParam("client", true)->value() : "";
    String user = req->hasParam("user", true) ? req->getParam("user", true)->value() : "";
    String pass = req->hasParam("pass", true) ? req->getParam("pass", true)->value() : "";
    int keepalive = req->hasParam("keepalive", true) ? req->getParam("keepalive", true)->value().toInt() : 0;

    broker.trim();
    client.trim();
    if (req->hasParam("client", true) && client != String(g_mqttConfig.clientId)) {
      req->send(400, "text/plain", "ERR: MQTT client ID is read-only");
      return;
    }
    if (req->hasParam("topic", true)) {
      String topic = req->getParam("topic", true)->value();
      topic.trim();
      if (topic != String(g_mqttConfig.baseTopic)) {
        req->send(400, "text/plain", "ERR: MQTT topic is read-only");
        return;
      }
    }
    if (req->hasParam("baseTopic", true)) {
      String baseTopic = req->getParam("baseTopic", true)->value();
      baseTopic.trim();
      if (baseTopic != String(g_mqttConfig.baseTopic)) {
        req->send(400, "text/plain", "ERR: MQTT topic is read-only");
        return;
      }
    }
    if (broker.length() == 0 || broker.length() >= (int)sizeof(g_mqttConfig.broker) ||
        port <= 0 || port > 65535 || keepalive <= 0 || keepalive > 3600 ||
        user.length() >= (int)sizeof(g_mqttConfig.username) ||
        pass.length() >= (int)sizeof(g_mqttConfig.password)) {
      req->send(400, "text/plain", "ERR: invalid MQTT settings");
      return;
    }

    g_mqttConfig.enabled = enabled;
    g_mqttConfig.port = port;
    g_mqttConfig.keepAlive = keepalive;
    strncpy(g_mqttConfig.broker, broker.c_str(), sizeof(g_mqttConfig.broker) - 1);
    g_mqttConfig.broker[sizeof(g_mqttConfig.broker) - 1] = '\0';
    strncpy(g_mqttConfig.username, user.c_str(), sizeof(g_mqttConfig.username) - 1);
    g_mqttConfig.username[sizeof(g_mqttConfig.username) - 1] = '\0';
    strncpy(g_mqttConfig.password, pass.c_str(), sizeof(g_mqttConfig.password) - 1);
    g_mqttConfig.password[sizeof(g_mqttConfig.password) - 1] = '\0';
    saveMqttConfig();
    g_mqttConfigChanged = true;
    req->send(200, "text/plain", "MQTT settings saved.");
  });

  pProvServer->onNotFound([](AsyncWebServerRequest *req) {
    req->redirect("/");
  });
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

      pProvServer = new AsyncWebServer(80);
      setupDashboardRoutes();
      pProvServer->begin();
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
#if MQTT_ENABLED
static String mqttBool(bool value) { return value ? "true" : "false"; }

static void publishCommandAck(const String &cmdId, const String &type, bool ok,
                              const String &reason, int relay = 0,
                              bool state = false, bool locked = false) {
  if (!mqttClient.connected())
    return;
  String base = String(g_mqttConfig.baseTopic);
  String payload = "{\"cmd_id\":\"" + jsonEscape(cmdId) + "\",\"type\":\"" +
                   jsonEscape(type) + "\",\"ok\":" + mqttBool(ok) +
                   ",\"reason\":\"" + jsonEscape(reason) + "\"";
  if (relay > 0) {
    payload += ",\"relay\":" + String(relay);
    payload += ",\"state\":" + mqttBool(state);
    payload += ",\"locked\":" + mqttBool(locked);
  }
  payload += "}";
  mqttClient.publish((base + "/cmd/ack").c_str(), payload.c_str(), false);
}

static bool parseJsonBool(JsonVariantConst v, bool &out) {
  if (v.is<bool>()) {
    out = v.as<bool>();
    return true;
  }
  if (v.is<const char *>()) {
    String s = v.as<const char *>();
    s.trim();
    s.toLowerCase();
    if (s == "true" || s == "1" || s == "on") {
      out = true;
      return true;
    }
    if (s == "false" || s == "0" || s == "off") {
      out = false;
      return true;
    }
  }
  return false;
}

static void handleCloudCommand(const String &payload, const String &legacyType,
                               int legacyRelay = 0, bool legacyBool = false,
                               const String &legacyTarget = "") {
  StaticJsonDocument<512> doc;
  String cmdId = "";
  String type = legacyType;
  int relay = legacyRelay;
  bool boolValue = legacyBool;
  String target = legacyTarget;

  if (legacyType.length() == 0) {
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      publishCommandAck("unknown", "unknown", false, "invalid_payload");
      return;
    }
    cmdId = doc["cmd_id"] | "";
    type = doc["type"] | "";
    relay = doc["relay"] | 0;
    target = doc["target"] | "";
    if (!parseJsonBool(doc["state"], boolValue) &&
        !parseJsonBool(doc["locked"], boolValue)) {
      boolValue = false;
    }
  } else {
    cmdId = "legacy-" + String(millis());
  }

  if (cmdId.length() == 0)
    cmdId = "cmd-" + String(millis());

  if (type == "relay_set" || type == "relay_toggle" || type == "relay_lock") {
    if (relay < 1 || relay > 7) {
      publishCommandAck(cmdId, type, false, "invalid_relay");
      return;
    }

    if (relay <= NUM_RELAYS) {
      if (type == "relay_set") {
        setRelayState(relay - 1, boolValue);
      } else if (type == "relay_toggle") {
        toggleRelay(relay - 1);
      } else {
        setLocalRelayLock(relay - 1, boolValue);
        saveLocalControlState();
        updateRelayHardware();
      }

      bool stateVal = false;
      bool lockedVal = false;
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        stateVal = sysState.relayStates[relay - 1];
        lockedVal = sysState.lockedStates[relay - 1];
        xSemaphoreGive(stateMutex);
      }
      bool ok = true;
      String reason = "done";
      if (type == "relay_set" && boolValue && !stateVal) {
        ok = false;
        reason = lockedVal ? "locked" : "rejected";
      }
      publishCommandAck(cmdId, type, ok, reason, relay, stateVal, lockedVal);
      publishLiveData();
      return;
    }

    if (g_pendingD1CmdId.length() > 0) {
      publishCommandAck(cmdId, type, false, "busy", relay);
      return;
    }
    String d1Cmd = "relay_off";
    if (type == "relay_set")
      d1Cmd = boolValue ? "relay_on" : "relay_off";
    else if (type == "relay_toggle") {
      bool current = false;
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        current = sysState.digitalRelayState;
        xSemaphoreGive(stateMutex);
      }
      d1Cmd = current ? "relay_off" : "relay_on";
    } else if (type == "relay_lock") {
      d1Cmd = boolValue ? "relay_lock" : "relay_unlock";
    }
    g_pendingD1CmdId = cmdId;
    g_pendingD1CmdType = type;
    g_pendingD1CmdMs = millis();
    enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"" + d1Cmd + "\"}");
    return;
  }

  if (type == "master_lock") {
    bool ok = setMasterLock(boolValue);
    publishCommandAck(cmdId, type, ok, ok ? "alias_sent" : "busy");
    if (ok)
      publishLiveData();
    return;
  }

  if (type == "slave_reboot") {
    if (target == "digital" || target == "d1") {
      enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"reboot\"}");
    } else if (target == "pzem") {
      enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"pzem\",\"cmd\":\"reboot\"}");
    } else {
      publishCommandAck(cmdId, type, false, "invalid_target");
      return;
    }
    publishCommandAck(cmdId, type, true, "sent");
    return;
  }

  if (type == "pzem_energy_reset") {
    enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"pzem\",\"cmd\":\"energy_reset\"}");
    publishCommandAck(cmdId, type, true, "sent");
    return;
  }

  publishCommandAck(cmdId, type.length() ? type : "unknown", false,
                    "unsupported");
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
          Serial.printf("[HISTORY] ACK rejected batch_id=%s last_id=%u pending_last=%u\n",
                        batchId.c_str(), lastId, g_historyPendingLastId);
          return;
        }
        enqueueUartCmd("{\"t\":\"hist_ack\",\"last\":" + String(lastId) + "}");
        g_historyLastAckedId = lastId;
        g_historyPending = false;
        g_historyPendingBatchId = "";
        g_historyPendingLastId = 0;
        Serial.printf("[HISTORY] ACK received batch_id=%s last_id=%u\n",
                      batchId.c_str(), lastId);
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
    status += "\"digital_online\":" + mqttBool(sysState.digitalSlaveOnline) + ",";
    status += "\"pzem_online\":" + mqttBool(sysState.pzemSlaveOnline) + ",";
    status += "\"pzem_health\":" + mqttBool(sysState.pzemSensorHealthy) + ",";
    status += "\"dht_ok\":" + mqttBool(sysState.dhtHealthy) + ",";
    status += "\"voltage_estimated\":" + mqttBool(sysState.voltageEstimated) + ",";
    status += "\"time_source\":\"" + jsonEscape(sysState.timeSource) + "\",";
    status += "\"reset_reason\":\"" + jsonEscape(sysState.resetReason) + "\"";
    status += "}";

    xSemaphoreGive(stateMutex);
    mqttClient.publish((base + "/live/sensors").c_str(), sensors.c_str(),
                       false);
    mqttClient.publish((base + "/live/relays").c_str(), relays.c_str(), false);
    mqttClient.publish((base + "/live/status").c_str(), status.c_str(), false);
  }
}

void requestHistoryBatch(bool force) {
  if (!mqttClient.connected() || g_historyPending)
    return;
  uint32_t now = millis();
  if (!force && now - g_historyLastRequestMs < MQTT_HISTORY_RETRY_MS)
    return;
  g_historyLastRequestMs = now;
  Serial.printf("[HISTORY] Requesting batch after=%u limit=%u\n",
                g_historyLastAckedId, g_historyBatchLimit);
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
          publishCommandAck(g_pendingD1CmdId, g_pendingD1CmdType, false,
                            "timeout", 7);
          g_pendingD1CmdId = "";
          g_pendingD1CmdType = "";
        }
        if (g_historyPending &&
            millis() - g_historyLastRequestMs > MQTT_HISTORY_RETRY_MS) {
          g_historyPending = false;
          g_historyPendingBatchId = "";
          g_historyPendingLastId = 0;
          Serial.printf("[HISTORY] ACK timeout; retry from lastAckedId=%u\n",
                        g_historyLastAckedId);
        }
        if (millis() - lastHeartbeat > HEARTBEAT_MS) {
          lastHeartbeat = millis();
          publishTelemetry();
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
          StaticJsonDocument<4096> doc;
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
              static uint32_t lastTelLogTime = 0;
              if (millis() - lastTelLogTime >= 5000) {
                lastTelLogTime = millis();
                Serial.printf("[SmartNest] Telemetry update - V: %.1fV, Load: "
                              "%.2fA, SD: %s\n",
                              sysState.pzemVoltage, sysState.currentAmps,
                              sysState.sdOk ? "OK" : "ERROR");
              }
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
              Serial.printf("[LOCK] Legacy lock_ack received: %s\n", val ? "ON" : "OFF");
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
                Serial.printf("[CMD_ACK] d1 cmd=0x%02X ok=%s reason=%d relay=%d relayLock=%d masterLock=%d ocLock=%d\n",
                              cmdType, ok ? "YES" : "NO", reason, relay,
                              relayLock, masterLock, overcurrentLock);
#if MQTT_ENABLED
                if (mqttClient.connected()) {
                  if (g_pendingD1CmdId.length() > 0) {
                    String reasonText = "done";
                    if (!ok) {
                      if (reason == 1)
                        reasonText = "locked";
                      else if (reason == 3)
                        reasonText = "overcurrent_locked";
                      else
                        reasonText = "rejected";
                    }
                    publishCommandAck(g_pendingD1CmdId, g_pendingD1CmdType, ok,
                                      reasonText, 7, relay != 0,
                                      relayLock != 0);
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
                    Serial.printf("[HISTORY] Oversized batch skipped bytes=%u max=%u last_id=%u\n",
                                  payload.length(), MQTT_HISTORY_MAX_PAYLOAD_BYTES, lastId);
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
                    Serial.printf("[HISTORY] Published batch_id=%s count=%u last_id=%u bytes=%u\n",
                                  batchId.c_str(), records.size(), lastId, payload.length());
                  } else {
                    Serial.printf("[HISTORY] Publish failed batch_id=%s count=%u last_id=%u bytes=%u\n",
                                  batchId.c_str(), records.size(), lastId, payload.length());
                  }
                }
              }
#endif
            } else if (type == "off") {
              String dev = doc["dev"];
              if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                if (dev == "d1")
                  sysState.digitalSlaveOnline = false;
                else if (dev == "pzem")
                  sysState.pzemSlaveOnline = false;
                xSemaphoreGive(stateMutex);
              }
            } else if (type == "on") {
              String dev = doc["dev"];
              if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                if (dev == "d1")
                  sysState.digitalSlaveOnline = true;
                else if (dev == "pzem")
                  sysState.pzemSlaveOnline = true;
                xSemaphoreGive(stateMutex);
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
  float current = 0.0f, acs = 0.0f, voltage = 0.0f, energyVoltage = 0.0f, acCurrent = 0.0f;
  float acPower = 0.0f, tempC = 0.0f, humidity = 0.0f;
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
  json += "],\"locks\":[";
  for (int i = 0; i < NUM_RELAYS; i++) {
    if (i > 0)
      json += ",";
    json += locks[i] ? "true" : "false";
  }
  json += "],";
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
  json += "\"temp_c\":" + String(tempC, 1) + ",";
  json += "\"humidity\":" + String(humidity, 1) + ",";
  json += "\"dht_ok\":" + String(dhtOk ? "true" : "false") + ",";
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
  Serial.println("RELAY <1-7> ON|OFF|TOGGLE");
  Serial.println("LOCK <1-7> ON|OFF");
  Serial.println("MASTERLOCK ON|OFF (legacy alias for Lock All/Unlock All)");
  Serial.println("SLAVE D1 reboot");
  Serial.println("SLAVE PZEM reboot|energy_reset");
  Serial.println("SD INFO");
  Serial.println("RESET WIFI|MQTT|ENERGY|FULL");
  Serial.println("MQTT SHOW");
  Serial.println("MQTT ENABLE ON|OFF");
  Serial.println("MQTT SET BROKER <host>");
  Serial.println("MQTT SET PORT <port>");
  Serial.println("MQTT SET CLIENT <clientId> (read-only; rejected)");
  Serial.println("MQTT SET USER <username>");
  Serial.println("MQTT SET PASS <password>");
  Serial.println("MQTT SET TOPIC <baseTopic> (read-only; rejected)");
  Serial.println("MQTT SET KEEPALIVE <seconds>");
  Serial.println("MQTT RESET");
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

static void fullFactoryReset() {
  enqueueUartCmd("{\"t\":\"factory_reset\"}");
  enqueueUartCmd("{\"t\":\"clear_logs\"}");
  clearLocalControlState();
  resetMqttConfigToDefault();
  delay(1000);
  resetWifiAndRestart();
}

static void handleRelayCommand(int relayIdx, const String &action) {
  if (relayIdx < 1 || relayIdx > 7) {
    Serial.println("ERR: relay must be 1-7");
    return;
  }

  bool state = false;
  if (action == "TOGGLE") {
    if (relayIdx <= NUM_RELAYS) {
      toggleRelay(relayIdx - 1);
    } else {
      bool current = false;
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        current = sysState.digitalRelayState;
        xSemaphoreGive(stateMutex);
      }
      enqueueUartCmd(String("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"") +
                     (current ? "relay_off" : "relay_on") + "\"}");
    }
    Serial.println("OK");
    return;
  }

  if (!parseBoolValue(action, state)) {
    Serial.println("ERR: use ON, OFF, or TOGGLE");
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
  if (cmdUpper == "MQTT RESET") {
    resetMqttConfigToDefault();
    g_mqttConfigChanged = true;
    Serial.println("OK");
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
  } else if (cmdUpper.startsWith("MQTT SET CLIENT ")) {
    Serial.println("ERR: MQTT client ID is fixed/read-only");
    return;
  } else if (cmdUpper.startsWith("MQTT SET USER ")) {
    strncpy(g_mqttConfig.username, cmdRaw.substring(14).c_str(), sizeof(g_mqttConfig.username) - 1);
    g_mqttConfig.username[sizeof(g_mqttConfig.username) - 1] = '\0';
  } else if (cmdUpper.startsWith("MQTT SET PASS ")) {
    strncpy(g_mqttConfig.password, cmdRaw.substring(14).c_str(), sizeof(g_mqttConfig.password) - 1);
    g_mqttConfig.password[sizeof(g_mqttConfig.password) - 1] = '\0';
  } else if (cmdUpper.startsWith("MQTT SET TOPIC ")) {
    Serial.println("ERR: MQTT topic is fixed/read-only");
    return;
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
      Serial.println("ERR: use RELAY <1-7> ON|OFF|TOGGLE");
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
  } else if (head == "MASTERLOCK") {
    bool state = false;
    if (parseBoolValue(rest, state)) {
      Serial.println(setMasterLock(state) ? "OK" : "ERR: lock busy");
    } else {
      Serial.println("ERR: use MASTERLOCK ON|OFF");
    }
  } else if (upper.startsWith("SLAVE D1 ") || upper.startsWith("SLAVE PZEM ")) {
    String target = upper.startsWith("SLAVE D1 ") ? "d1" : "pzem";
    String slaveCmd = raw.substring(target == "d1" ? 9 : 11);
    slaveCmd.toLowerCase();
    bool validD1 = target == "d1" && slaveCmd == "reboot";
    bool validPzem = target == "pzem" &&
                     (slaveCmd == "reboot" || slaveCmd == "energy_reset");
    if (!validD1 && !validPzem) {
      Serial.println("ERR: invalid slave command");
      return;
    }
    enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"" + target + "\",\"cmd\":\"" + slaveCmd + "\"}");
    Serial.println("OK");
  } else if (upper == "SD INFO") {
    printSdInfo();
  } else if (upper == "RESET WIFI") {
    resetWifiAndRestart();
  } else if (upper == "RESET MQTT") {
    resetMqttConfigToDefault();
    g_mqttConfigChanged = true;
    Serial.println("OK");
  } else if (upper == "RESET ENERGY") {
    enqueueUartCmd("{\"t\":\"factory_reset\"}");
    Serial.println("OK");
  } else if (upper == "RESET FULL") {
    fullFactoryReset();
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

  if (isProvisioningMode) {
    wifiManagerLoop();
    vTaskDelay(pdMS_TO_TICKS(10));
  } else {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
