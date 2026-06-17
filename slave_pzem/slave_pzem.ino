#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <PZEM004Tv30.h>

// Pins
#define PIN_LED_R            5 
#define PIN_LED_G            18 
#define PIN_LED_B            19 

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
RxMessageBuffer rxBuffer = { {}, 0, false };

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
    
    bool espNowError = (currentMillis - lastMasterContactTime > MASTER_TIMEOUT_MS) || !espNowInitialized;
    bool pzemError = !pzemHealthy;
    bool criticalError = espNowError || pzemError;
    
    bool showYellow = (currentMillis < yellowActiveUntilTime);
    bool showBlue = !firstContactMade && !criticalError;
    
    if (criticalError) {
        // Red - Critical Connection or Sensor Read Error
        setRGBColor(true, false, false); 
    } 
    else if (showYellow) {
        // Yellow - Master command receipt blink
        setRGBColor(true, true, false); 
    } 
    else if (showBlue) {
        // Blue - Boot / Initialization
        setRGBColor(false, false, true); 
    } 
    else {
        // Green - Everything Healthy
        setRGBColor(false, true, false); 
    }
}

// ESP-NOW Receive Callback
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
void onReceive(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    const uint8_t* senderMac = info->src_addr;
#else
void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    const uint8_t* senderMac = mac;
#endif

    if (memcmp(senderMac, masterMAC, 6) == 0) {
        if (len <= sizeof(rxBuffer.data)) {
            memcpy((void*)rxBuffer.data, data, len);
            rxBuffer.len = len;
            rxBuffer.pending = true;
        }
    }
}

// Read PZEM and send JSON packet
void readPzemAndSend(const char* type) {
    float voltage = pzem.voltage();
    float current = pzem.current();
    float power = pzem.power();
    float energy = pzem.energy();
    
    if (isnan(voltage) || isnan(current) || isnan(power) || isnan(energy)) {
        pzemHealthy = false;
        Serial.println("[PZEM ERROR] Failed to read sensor values (returned NAN)");
        return;
    }
    
    pzemHealthy = true;

    #if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument doc;
    #else
    StaticJsonDocument<256> doc;
    #endif

    doc["type"] = type;
    doc["voltage"] = voltage;
    doc["current"] = current;
    doc["power"] = power;
    doc["energy"] = energy;

    char txBuffer[256];
    size_t len = serializeJson(doc, txBuffer);
    
    if (espNowInitialized) {
        esp_err_t result = esp_now_send(masterMAC, (uint8_t*)txBuffer, len);
        if (result == ESP_OK) {
            Serial.printf("[TX OK] Type: %s | V: %.1fV | I: %.3fA | P: %.1fW | E: %.1fWh\n", 
                          type, voltage, current, power, energy);
        } else {
            Serial.println("[TX FAIL] ESP-NOW transmission failed");
        }
    }
}

// Process incoming packages in loop context
void handleIncomingPackets() {
    if (!rxBuffer.pending) return;
    
    rxBuffer.pending = false;
    lastMasterContactTime = millis();
    firstContactMade = true;

    #if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument doc;
    #else
    StaticJsonDocument<256> doc;
    #endif

    DeserializationError error = deserializeJson(doc, rxBuffer.data, rxBuffer.len);
    if (error) {
        Serial.print("[JSON ERROR] Parse failed: ");
        Serial.println(error.c_str());
        return;
    }

    if (doc.containsKey("type")) {
        const char* type = doc["type"];
        if (strcmp(type, "ack") == 0) {
            Serial.println("[RX] ACK ping received from Master");
            readPzemAndSend("ack_ok");
        }
    }

    if (doc.containsKey("cmd")) {
        const char* cmd = doc["cmd"];
        Serial.printf("[RX] Command received: %s\n", cmd);
        
        yellowActiveUntilTime = millis() + YELLOW_BLINK_DURATION_MS;

        if (strcmp(cmd, "reboot") == 0) {
            Serial.println("[CMD] Rebooting system in 1 second...");
            rebootRequested = true;
            rebootTime = millis() + 1000;
        } 
        else if (strcmp(cmd, "energy_reset") == 0) {
            Serial.println("[CMD] Resetting energy accumulation register...");
            
            bool resetOk = pzem.resetEnergy();
            if (resetOk) {
                Serial.println("[CMD] PZEM Energy reset successfully completed");
                readPzemAndSend("data");
            } else {
                Serial.println("[ERROR] PZEM Energy reset operation failed");
                pzemHealthy = false;
            }
        }
    }
}

// Setup
void setup() {
    Serial.begin(115200);
    Serial.println("\n[SYSTEM START] PZEM Slave Initializing...");

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

    if (millis() - lastTelemetrySentTime >= TELEMETRY_INTERVAL_MS) {
        lastTelemetrySentTime = millis();
        readPzemAndSend("data");
    }

    if (rebootRequested && (millis() >= rebootTime)) {
        ESP.restart();
    }

    updateRGBLED();
}
