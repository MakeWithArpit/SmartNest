#include <PZEM004Tv30.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

struct __attribute__((packed)) CmdPacket {
  uint8_t type;
};

struct __attribute__((packed)) PzemSlavePacket {
  uint8_t type;
  float voltage;
  float current;
  float power;
  float energy;
};

// Pins
#define PIN_LED_R 5
#define PIN_LED_G 18
#define PIN_LED_B 19

// Constants
const uint32_t TELEMETRY_INTERVAL_MS = 15000;
const uint32_t MASTER_TIMEOUT_MS = 60000;
const uint32_t YELLOW_BLINK_DURATION_MS = 1000;

// MAC Addresses
const uint8_t masterMAC[] = {0x88, 0x57, 0x21, 0xB1, 0xD3, 0x74};
const uint8_t localSlaveMac[] = {0x78, 0x21, 0x84, 0x9C, 0x98, 0x4C};

// PZEM-004T V3.0 using Serial2 (RX pin 16, TX pin 17)
PZEM004Tv30 pzem(Serial2, 16, 17);

// Thread-safe RX buffer
struct RxMessageBuffer {
  uint8_t data[250];
  int len;
  volatile bool pending;
};
RxMessageBuffer rxBuffer = {{}, 0, false};
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

// Communication & Device Health States
bool espNowInitialized = false;
bool firstContactMade = false;
uint32_t lastMasterContactTime = 0;
bool pzemHealthy = true;

// LED control variables
uint32_t yellowActiveUntilTime = 0;

// Non-blocking Reboot Scheduling
bool rebootRequested = false;
uint32_t rebootTime = 0;

uint32_t lastTelemetrySentTime = 0;

// RGB LED Control
void setRGBColor(bool red, bool green, bool blue) {
  digitalWrite(PIN_LED_R, red ? HIGH : LOW);
  digitalWrite(PIN_LED_G, green ? HIGH : LOW);
  digitalWrite(PIN_LED_B, blue ? HIGH : LOW);
}

// LED Status Indicator based on Priority Matrix
void updateRGBLED() {
  uint32_t currentMillis = millis();

  bool espNowError =
      (currentMillis - lastMasterContactTime > MASTER_TIMEOUT_MS) ||
      !espNowInitialized;
  bool pzemError = !pzemHealthy;
  bool criticalError = espNowError || pzemError;

  bool showYellow = (currentMillis < yellowActiveUntilTime);
  bool showBlue = !firstContactMade && !criticalError;

  if (criticalError) {
    // Red - Critical Connection or Sensor Read Error
    setRGBColor(true, false, false);
  } else if (showYellow) {
    // Yellow - Master command receipt blink
    setRGBColor(true, true, false);
  } else if (showBlue) {
    // Blue - Boot / Initialization
    setRGBColor(false, false, true);
  } else {
    // Green - Everything Healthy
    setRGBColor(false, true, false);
  }
}

// ESP-NOW Receive Callback
void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  const uint8_t *senderMac = info->src_addr;

  if (memcmp(senderMac, masterMAC, 6) == 0) {
    if (len <= sizeof(rxBuffer.data)) {
      portENTER_CRITICAL_ISR(&rxMux);
      memcpy((void *)rxBuffer.data, data, len);
      rxBuffer.len = len;
      rxBuffer.pending = true;
      portEXIT_CRITICAL_ISR(&rxMux);
    }
  }
}

// Read PZEM and send binary packet
// If PZEM returns NaN (AC mains disconnected), falls back to 0.0f and still
// transmits as an online heartbeat so the Master does not timeout and mark the
// node offline.
void sendPzemPacket(uint8_t pktType) {
  float v = pzem.voltage();
  float i = pzem.current();
  float p = pzem.power();
  float e = pzem.energy();

  if (isnan(v) || isnan(i) || isnan(p) || isnan(e)) {
    pzemHealthy = false;
    Serial.println(
        "[PZEM] WARNING: Sensor returned NAN — AC mains may be disconnected.");
    if (isnan(v))
      v = 0.0f;
    if (isnan(i))
      i = 0.0f;
    if (isnan(p))
      p = 0.0f;
    if (isnan(e))
      e = 0.0f;
  } else {
    pzemHealthy = true;
  }

  PzemSlavePacket pkt;
  pkt.type = pktType;
  pkt.voltage = v;
  pkt.current = i;
  pkt.power = p;
  pkt.energy = e;

  if (espNowInitialized) {
    esp_err_t result = esp_now_send(masterMAC, (uint8_t *)&pkt, sizeof(pkt));
    if (result == ESP_OK) {
      Serial.printf("[PZEM] TX OK — Type: 0x%02X | V: %.1fV | I: %.3fA | P: "
                    "%.1fW | E: %.1fWh | Healthy: %s\n",
                    pktType, v, i, p, e, pzemHealthy ? "YES" : "NO");
    } else {
      Serial.println("[PZEM] TX FAIL — ESP-NOW send error");
    }
  }
}

// Process incoming packages in loop context
void handleIncomingPackets() {
  portENTER_CRITICAL(&rxMux);
  if (!rxBuffer.pending) {
    portEXIT_CRITICAL(&rxMux);
    return;
  }

  rxBuffer.pending = false;
  int len = rxBuffer.len;
  uint8_t cmdType = 0;
  if (len >= 1) {
    cmdType = ((CmdPacket *)rxBuffer.data)->type;
  }
  portEXIT_CRITICAL(&rxMux);

  lastMasterContactTime = millis();
  firstContactMade = true;

  if (len >= 1) {
    yellowActiveUntilTime = millis() + YELLOW_BLINK_DURATION_MS;

    switch (cmdType) {
    case 0x01: // ACK ping
      Serial.println("[RX] ACK ping received from Master");
      sendPzemPacket(0x21);
      break;
    case 0x06: // Reboot
      Serial.println("[CMD] Rebooting system in 1 second...");
      rebootRequested = true;
      rebootTime = millis() + 1000;
      break;
    case 0x07: // Energy reset
      Serial.println("[CMD] Resetting energy accumulation register...");
      if (pzem.resetEnergy()) {
        Serial.println("[CMD] PZEM Energy reset successfully completed");
        pzemHealthy = true;
        sendPzemPacket(0x20);
      } else {
        Serial.println("[ERROR] PZEM Energy reset operation failed");
        pzemHealthy = false;
      }
      break;
    }
  }
}

// Setup
void setup() {
  Serial.begin(115200);
  Serial.println("\n[SYSTEM START] PZEM Slave Initializing...");

  // Initialize Serial2 for PZEM-004T connection with pins 16 (RX) and 17 (TX)
  Serial2.begin(9600, SERIAL_8N1, 16, 17);
  Serial.println(
      "[PZEM] Hardware Serial2 (Baud: 9600, RX: 16, TX: 17) initialized.");

  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);

  setRGBColor(false, false, true); // Blue (Booting)

  WiFi.mode(WIFI_STA);
  esp_wifi_set_mac(WIFI_IF_STA, localSlaveMac);
  WiFi.disconnect();

  Serial.print("[WIFI] Station Mode initialized. MAC Address: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() == ESP_OK) {
    espNowInitialized = true;
    Serial.println("[ESP-NOW] Initialized successfully");
    esp_now_register_recv_cb(onReceive);

    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, masterMAC, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) == ESP_OK) {
      Serial.println("[ESP-NOW] Master peer added successfully");
    } else {
      Serial.println("[ESP-NOW ERROR] Failed to add Master peer");
    }
  } else {
    Serial.println("[ESP-NOW ERROR] ESP-NOW initialization failed");
  }

  lastMasterContactTime = millis();
  lastTelemetrySentTime = millis();
}

// Loop
void loop() {
  handleIncomingPackets();

  uint32_t now = millis();

  if (now - lastTelemetrySentTime >= TELEMETRY_INTERVAL_MS) {
    lastTelemetrySentTime = now;
    sendPzemPacket(0x20);
  }

  // Periodic 5s status log (non-spamming)
  static uint32_t lastStatusLog = 0;
  if (now - lastStatusLog >= 5000) {
    lastStatusLog = now;
    bool contacted = firstContactMade;
    bool timeout = (now - lastMasterContactTime > MASTER_TIMEOUT_MS);
    Serial.printf(
        "[PZEM] Status — Master: %s | PZEM Healthy: %s | ESP-NOW: %s\n",
        contacted ? (timeout ? "TIMEOUT" : "ONLINE") : "WAITING",
        pzemHealthy ? "YES" : "NO", espNowInitialized ? "OK" : "ERROR");

    if (!pzemHealthy) {
      Serial.println(
          "[PZEM DIAGNOSTIC] Warning: PZEM sensor is returning NAN.");
      Serial.println("  Possible Cause 1: AC mains input is NOT connected to "
                     "the PZEM module (no line voltage).");
      Serial.println("  Possible Cause 2: RX/TX wiring between ESP32 and PZEM "
                     "is incorrect/swapped.");
      Serial.println("  Possible Cause 3: Ground (GND) is not common between "
                     "ESP32 and PZEM.");
    }
    if (timeout || !espNowInitialized) {
      Serial.println("[PZEM DIAGNOSTIC] ESP-NOW Error: Master heartbeat "
                     "timeout. Check Master power, MAC, or channel settings.");
    }
  }

  if (rebootRequested && (now >= rebootTime)) {
    ESP.restart();
  }

  updateRGBLED();
}
