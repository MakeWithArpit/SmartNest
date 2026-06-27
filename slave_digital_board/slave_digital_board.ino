#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>

struct __attribute__((packed)) CmdPacket {
  uint8_t type;
};

struct __attribute__((packed)) DigitalSlavePacket {
  uint8_t type;
  float rmsCurrent;
  uint8_t relayState;
  uint8_t switchState;
  uint8_t lockState;
};

struct __attribute__((packed)) DigitalCmdAckPacket {
  uint8_t type;
  uint8_t cmdType;
  uint8_t accepted;
  uint8_t reason;
  uint8_t relayState;
  uint8_t relayLockState;
  uint8_t masterLockState;
  uint8_t overcurrentLockState;
};

// Pins
#define RELAY_PIN 15
#define CURRENT_SENSOR_PIN 34
#define MANUAL_SWITCH_PIN 2

#define RED_LED_PIN 5
#define GREEN_LED_PIN 18
#define BLUE_LED_PIN 19

// Global State Variables
uint8_t masterMacAddress[] = {0x88, 0x57, 0x21, 0xB1, 0xD3, 0x74};
uint8_t localDigitalMac[] = {0x14, 0x08, 0x08, 0xA4, 0x94, 0x1C};

bool overcurrentLock = false;
bool relayLock = false;
bool relayCondition = false;
bool relayOutputState = false;
float currentAmperes = 0.0f;
float zeroMilliVolts = 0.0f;
Preferences controlPrefs;

// Non-blocking current measurement integration variables
double sqSum = 0.0;
uint32_t sampleCount = 0;
int consecutiveLowCurrentCount = 0;

// Communication state buffer
volatile bool messageReceived = false;
uint8_t receiveBuffer[250];
int receiveLength = 0;
uint32_t lastAcknowledgementTime = 0;
portMUX_TYPE receiveMux = portMUX_INITIALIZER_UNLOCKED;

// ESP-NOW Receive Callback
void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  portENTER_CRITICAL_ISR(&receiveMux);
  int copyLen = len > 250 ? 250 : len;
  memcpy(receiveBuffer, data, copyLen);
  receiveLength = copyLen;
  messageReceived = true;
  portEXIT_CRITICAL_ISR(&receiveMux);
}

void saveControlState() {
  controlPrefs.putBool("relay", relayCondition);
  controlPrefs.putBool("rLock", relayLock);
  controlPrefs.remove("mLock");
}

void applyRelayHardware() {
  bool actualRelay = relayCondition && !relayLock && !overcurrentLock;
  digitalWrite(RELAY_PIN, actualRelay ? LOW : HIGH);
  relayOutputState = actualRelay;
}

void loadControlState() {
  controlPrefs.begin("d1_ctrl", false);
  relayCondition = controlPrefs.getBool("relay", false);
  relayLock = controlPrefs.getBool("rLock", false);
  if (controlPrefs.isKey("mLock")) {
    controlPrefs.remove("mLock");
    Serial.println("[DigitalBoard] Cleared legacy mLock NVS key");
  }
  applyRelayHardware();
  Serial.printf("[DigitalBoard] Loaded state: relay=%s relayLock=%s\n",
                relayCondition ? "ON" : "OFF", relayLock ? "ON" : "OFF");
}

// Relay Control
uint8_t currentLockReason() {
  if (relayLock || overcurrentLock) {
    if (relayLock)
      return 1;
    return 3;
  }
  return 0;
}

void sendCommandAck(uint8_t cmdType, bool accepted, uint8_t reason) {
  DigitalCmdAckPacket pkt;
  pkt.type = 0x12;
  pkt.cmdType = cmdType;
  pkt.accepted = accepted ? 1 : 0;
  pkt.reason = reason;
  pkt.relayState = relayOutputState ? 1 : 0;
  pkt.relayLockState = relayLock ? 1 : 0;
  pkt.masterLockState = 0; // Legacy/deprecated field kept for packet compatibility.
  pkt.overcurrentLockState = overcurrentLock ? 1 : 0;
  esp_now_send(masterMacAddress, (uint8_t *)&pkt, sizeof(pkt));
}

bool relayOn() {
  uint8_t reason = currentLockReason();
  if (reason != 0) {
    Serial.printf(
        "[DigitalBoard] relayOn() blocked - relayLock=%d overcurrentLock=%d\n",
        relayLock, overcurrentLock);
    return false;
  }
  relayCondition = true;
  saveControlState();
  applyRelayHardware();
  Serial.println("[DigitalBoard] Relay -> ON");
  return true;
}

void relayOff() {
  relayCondition = false;
  saveControlState();
  applyRelayHardware();
  Serial.println("[DigitalBoard] Relay -> OFF");
}

void forceRelayOff() {
  bool wasOn = relayOutputState;
  applyRelayHardware();
  if (wasOn) {
    Serial.println("[DigitalBoard] Relay forced OFF");
  }
}

void setRelayLock(bool locked) {
  relayLock = locked;
  if (relayLock) {
    relayCondition = false;
  }
  saveControlState();
  applyRelayHardware();
  Serial.printf("[DigitalBoard] Relay lock -> %s\n", locked ? "ON" : "OFF");
}

// 1 kHz non-blocking current sampler
void sampleCurrentNonBlocking() {
  uint32_t currentMicros = micros();
  static uint32_t lastSampleMicros = 0;

  if (currentMicros - lastSampleMicros >= 1000) {
    lastSampleMicros = currentMicros;

    float milliVolts = analogReadMilliVolts(CURRENT_SENSOR_PIN);
    float deltaMilliVolts = milliVolts - zeroMilliVolts;
    float instCurrent =
        deltaMilliVolts / 66.0f; // 66 mV/A sensitivity for ACS712-30A

    sqSum += (double)(instCurrent * instCurrent);
    sampleCount++;
  }
}

// Overcurrent Protection check (Threshold: 6.0A RMS)
void checkOvercurrentProtection() {
  if (currentAmperes >= 6.0f) {
    if (!overcurrentLock) {
      overcurrentLock = true;
      forceRelayOff();
      Serial.print("[CRITICAL ERROR] Overcurrent detected: ");
      Serial.print(currentAmperes, 2);
      Serial.println(" A! Relay locked OFF.");
    } else {
      forceRelayOff();
    }
  }

  if (overcurrentLock) {
    if (currentAmperes < 5.5f) {
      consecutiveLowCurrentCount++;
      if (consecutiveLowCurrentCount >= 3) { // Stays below 5.5A for 600ms total
        overcurrentLock = false;
        consecutiveLowCurrentCount = 0;
        applyRelayHardware();
        Serial.println("[INFO] Overcurrent cleared. System restored.");
      }
    } else {
      consecutiveLowCurrentCount = 0;
    }
  } else {
    consecutiveLowCurrentCount = 0;
  }
}

// Process RMS current every 200ms
void processCurrentMeasurements() {
  if (sampleCount > 0) {
    float meanSquare = (float)(sqSum / (double)sampleCount);
    float rmsCurrent = sqrt(meanSquare);

    if (rmsCurrent < 0.15f) {
      currentAmperes = 0.0f; // Deadband filter
    } else {
      currentAmperes = rmsCurrent;
    }

    sqSum = 0.0;
    sampleCount = 0;

    checkOvercurrentProtection();
  }
}

// Switch reading with 50ms debouncer
void checkManualSwitch() {
  static bool lastRawState = HIGH;
  static bool lastDebouncedState = HIGH;
  static unsigned long stateStableTime = 0;
  static bool initialized = false;

  bool rawState = digitalRead(MANUAL_SWITCH_PIN);
  unsigned long currentTime = millis();

  if (!initialized) {
    lastRawState = rawState;
    lastDebouncedState = rawState;
    stateStableTime = currentTime;
    initialized = true;
    return;
  }

  if (rawState != lastRawState) {
    stateStableTime = currentTime;
    lastRawState = rawState;
  } else if (currentTime - stateStableTime >= 50) {
    if (rawState != lastDebouncedState) {
      lastDebouncedState = rawState;

      if (rawState == HIGH) {
        if (relayLock || overcurrentLock) {
          Serial.println("[DigitalBoard] Switch ON ignored — locked");
        } else {
          Serial.println("[DigitalBoard] Switch toggled ON");
          relayOn();
        }
      } else {
        Serial.println("[DigitalBoard] Switch toggled OFF");
        relayOff();
      }
    }
  }
}

// RGB Indicator LED Priorities
void setLEDColor(bool red, bool green, bool blue) {
  digitalWrite(RED_LED_PIN, red ? HIGH : LOW);
  digitalWrite(GREEN_LED_PIN, green ? HIGH : LOW);
  digitalWrite(BLUE_LED_PIN, blue ? HIGH : LOW);
}

void updateLEDState() {
  unsigned long currentTime = millis();
  static unsigned long lastLEDToggleTime = 0;
  static bool ledState = false;

  if (lastAcknowledgementTime == 0) {
    // Boot: White blink (500ms)
    if (currentTime - lastLEDToggleTime >= 500) {
      lastLEDToggleTime = currentTime;
      ledState = !ledState;
    }
    setLEDColor(ledState, ledState, ledState);
  } else if (overcurrentLock) {
    // Overcurrent: Red blink (300ms)
    if (currentTime - lastLEDToggleTime >= 300) {
      lastLEDToggleTime = currentTime;
      ledState = !ledState;
    }
    setLEDColor(ledState, false, false);
  } else if (relayLock) {
    // Relay Lock: Yellow solid
    setLEDColor(true, true, false);
  } else if (relayCondition) {
    // Relay ON: Green solid
    setLEDColor(false, true, false);
  } else if (currentTime - lastAcknowledgementTime > 60000) {
    // No connection: Magenta blink (1s)
    if (currentTime - lastLEDToggleTime >= 1000) {
      lastLEDToggleTime = currentTime;
      ledState = !ledState;
    }
    setLEDColor(ledState, false, ledState);
  } else {
    // Normal: Blue solid
    setLEDColor(false, false, true);
  }
}

// Transmissions
void sendSlaveData() {
  DigitalSlavePacket pkt;
  pkt.type = 0x10;
  pkt.rmsCurrent = relayOutputState ? currentAmperes : 0.0f;
  pkt.relayState = relayOutputState ? 1 : 0;
  pkt.switchState = (digitalRead(MANUAL_SWITCH_PIN) == LOW) ? 1 : 0;
  pkt.lockState = relayLock ? 1 : 0;
  esp_now_send(masterMacAddress, (uint8_t *)&pkt, sizeof(pkt));
}

void sendAckReply() {
  DigitalSlavePacket pkt;
  pkt.type = 0x11;
  pkt.rmsCurrent = relayOutputState ? currentAmperes : 0.0f;
  pkt.relayState = relayOutputState ? 1 : 0;
  pkt.switchState = 0;
  pkt.lockState = relayLock ? 1 : 0;
  esp_now_send(masterMacAddress, (uint8_t *)&pkt, sizeof(pkt));
}

// Setup
void setup() {
  Serial.begin(115200);

  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(BLUE_LED_PIN, OUTPUT);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // Start with Relay OFF (Active HIGH)
  loadControlState();

  pinMode(MANUAL_SWITCH_PIN, INPUT_PULLUP);
  pinMode(CURRENT_SENSOR_PIN, INPUT);
  analogSetAttenuation(ADC_11db);
  analogReadResolution(12);

  setLEDColor(true, true, true);

  WiFi.mode(WIFI_STA);
  esp_err_t macResult = esp_wifi_set_mac(WIFI_IF_STA, localDigitalMac);
  if (macResult != ESP_OK) {
    Serial.printf("[DigitalBoard] Failed to set STA MAC err=%d\n",
                  (int)macResult);
  }
  WiFi.disconnect();
  Serial.print("[DigitalBoard] STA MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  esp_now_register_recv_cb(onReceive);

  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, masterMacAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add master peer");
    return;
  }

  Serial.println("[SYSTEM] Calibrating ACS712 zero offset...");
  long sumMilliVolts = 0;
  for (int i = 0; i < 1000; i++) {
    sumMilliVolts += analogReadMilliVolts(CURRENT_SENSOR_PIN);
    delay(2);
  }
  zeroMilliVolts = (float)sumMilliVolts / 1000.0f;
  Serial.print("[SYSTEM] Calibration completed. Zero-Offset: ");
  Serial.print(zeroMilliVolts);
  Serial.println(" mV");
}

// Loop
void loop() {
  if (messageReceived) {
    portENTER_CRITICAL(&receiveMux);
    messageReceived = false;
    int len = receiveLength;
    uint8_t cmdType = 0;
    if (len >= 1) {
      cmdType = ((CmdPacket *)receiveBuffer)->type;
    }
    portEXIT_CRITICAL(&receiveMux);

    if (len >= 1) {
      switch (cmdType) {
      case 0x01: // ACK ping
        lastAcknowledgementTime = millis();
        sendAckReply();
        break;
      case 0x02:
        Serial.printf("[TIMING] Digital Board relay toggled at: %lu\n",
                      millis());
        Serial.println("[DigitalBoard] CMD: relay_on");
        {
          bool accepted = relayOn();
          sendCommandAck(cmdType, accepted, accepted ? 0 : currentLockReason());
        }
        break;
      case 0x03:
        Serial.printf("[TIMING] Digital Board relay toggled at: %lu\n",
                      millis());
        Serial.println("[DigitalBoard] CMD: relay_off");
        relayOff();
        sendCommandAck(cmdType, true, 0);
        break;
      case 0x04:
        Serial.println("[DigitalBoard] CMD: relay_lock");
        setRelayLock(true);
        sendCommandAck(cmdType, true, 0);
        break;
      case 0x05:
        Serial.println("[DigitalBoard] CMD: relay_unlock");
        setRelayLock(false);
        sendCommandAck(cmdType, true, 0);
        break;
      case 0x06:
        Serial.println("[DigitalBoard] CMD: reboot");
        sendCommandAck(cmdType, true, 0);
        delay(100);
        ESP.restart();
        break;

      }
    }
  }

  sampleCurrentNonBlocking();

  static unsigned long lastCurrentProcessTime = 0;
  if (millis() - lastCurrentProcessTime >= 200) {
    lastCurrentProcessTime = millis();
    processCurrentMeasurements();
  }

  static unsigned long lastSwitchCheckTime = 0;
  if (millis() - lastSwitchCheckTime >= 50) {
    lastSwitchCheckTime = millis();
    checkManualSwitch();
  }

  // 5-second periodic status log
  static unsigned long lastStatusLogTime = 0;
  if (millis() - lastStatusLogTime >= 5000) {
    lastStatusLogTime = millis();
    Serial.printf("[DigitalBoard] RMS: %.2fA | Relay: %s | RelayLock: %s | "
                  "OC-Lock: %s\n",
                  currentAmperes, relayOutputState ? "ON" : "OFF",
                  relayLock ? "YES" : "NO",
                  overcurrentLock ? "YES" : "NO");
  }

  static bool lastRelayCondition = false;
  static unsigned long lastTelemetryTime = 0;
  unsigned long currentTime = millis();

  if (relayOutputState != lastRelayCondition ||
      (currentTime - lastTelemetryTime >= 30000)) {
    lastRelayCondition = relayOutputState;
    lastTelemetryTime = currentTime;

    if (lastAcknowledgementTime > 0) {
      sendSlaveData();
    }
  }

  updateLEDState();
}
