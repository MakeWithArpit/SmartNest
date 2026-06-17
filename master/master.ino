#include <WiFi.h>
#include <esp_now.h>
#include <ArduinoJson.h>

// Configuration & Constants
const uint32_t SLAVE_TIMEOUT_MS = 60000;
const uint32_t ACK_INTERVAL_MS = 15000;
const uint32_t ACK_STAGGER_OFFSET_MS = 7500;

// MAC Addresses of Slaves
const uint8_t slave1MacAddress[] = {0x14, 0x08, 0x08, 0xA4, 0x94, 0x1C};
const uint8_t slave2MacAddress[] = {0x78, 0x21, 0x84, 0x9C, 0x98, 0x4C};

// Enum for Slaves
enum SlaveType {
    SLAVE_DIGITAL,
    SLAVE_PZEM,
    SLAVE_COUNT
};

// Thread-safe Buffer for ESP-NOW RX
struct SlaveMessageBuffer {
    uint8_t data[250];
    int len;
    int rssi;
    volatile bool pending;
};

SlaveMessageBuffer slave1RxBuffer = { {}, 0, 0, false };
SlaveMessageBuffer slave2RxBuffer = { {}, 0, 0, false };

// Slave Health Tracking Structure
struct SlaveCommHealth {
    int rssi;
    uint32_t lastSeenTime;
    uint32_t packetReceivedCounter;
    bool onlineStatus;
};

// Global Data Structures
struct DigitalBoardData {
    float current;
    int relay;
    int manualSwitch;
    SlaveCommHealth health;
    uint32_t lastAckSentTime;
};

struct PzemData {
    float voltage;
    float current;
    float power;
    float energy;
    SlaveCommHealth health;
    uint32_t lastAckSentTime;
};

DigitalBoardData g_digitalBoard = { 0.0f, 0, 0, { 0, 0, 0, false }, 0 };
PzemData g_pzemBoard = { 0.0f, 0.0f, 0.0f, 0.0f, { 0, 0, 0, false }, 0 };

// Slave Registry structure for uniform health operations
struct SlaveDevice {
    const char* name;
    SlaveCommHealth* health;
};

SlaveDevice g_slaves[] = {
    { "Slave-1 (Digital Board)", &g_digitalBoard.health },
    { "Slave-2 (PZEM Board)", &g_pzemBoard.health }
};
const size_t g_numSlaves = sizeof(g_slaves) / sizeof(g_slaves[0]);

// Volatile Command Buffer
String g_pendingCommand = "";

// Serial Input Buffer
char serialCommandBuffer[64];
size_t serialBufferIndex = 0;

// Receive Callback
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
void onReceive(const esp_now_recv_info_t* receiveInfo, const uint8_t* data, int len) {
    const uint8_t* senderMac = receiveInfo->src_addr;
    int rssi = receiveInfo->rx_ctrl->rssi;
#else
void onReceive(const uint8_t* macAddress, const uint8_t* data, int len) {
    const uint8_t* senderMac = macAddress;
    int rssi = 0;
#endif

    if (memcmp(senderMac, slave1MacAddress, 6) == 0) {
        if (len <= sizeof(slave1RxBuffer.data)) {
            memcpy((void*)slave1RxBuffer.data, data, len);
            slave1RxBuffer.len = len;
            slave1RxBuffer.rssi = rssi;
            slave1RxBuffer.pending = true;
        }
    } else if (memcmp(senderMac, slave2MacAddress, 6) == 0) {
        if (len <= sizeof(slave2RxBuffer.data)) {
            memcpy((void*)slave2RxBuffer.data, data, len);
            slave2RxBuffer.len = len;
            slave2RxBuffer.rssi = rssi;
            slave2RxBuffer.pending = true;
        }
    }
}

// Function to Send ESP-NOW Commands
void sendMasterCommand(const uint8_t* peerMac, String command) {
    #if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument doc;
    #else
    StaticJsonDocument<256> doc;
    #endif

    doc["cmd"] = command;

    char buffer[256];
    size_t len = serializeJson(doc, buffer);
    esp_err_t result = esp_now_send(peerMac, (uint8_t*)buffer, len);
    
    Serial.print("[CMD SENT] Command '");
    Serial.print(command);
    Serial.print("' sent to MAC: ");
    for (int i = 0; i < 6; i++) {
        Serial.printf("%02X%s", peerMac[i], (i < 5) ? ":" : "");
    }
    Serial.println(result == ESP_OK ? " (Success)" : " (Failed)");
}

// Function to Send ACK
void sendAcknowledgement(const uint8_t* peerMac, const char* peerName) {
    #if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument doc;
    #else
    StaticJsonDocument<256> doc;
    #endif

    doc["type"] = "ack";

    char buffer[256];
    size_t len = serializeJson(doc, buffer);
    esp_now_send(peerMac, (uint8_t*)buffer, len);
    
    Serial.print("[ACK SENT] To ");
    Serial.println(peerName);
}

// Staggered ACK Scheduler
void handlePeriodicAcks() {
    uint32_t currentMillis = millis();
    
    if (currentMillis - g_digitalBoard.lastAckSentTime >= ACK_INTERVAL_MS) {
        g_digitalBoard.lastAckSentTime = currentMillis;
        sendAcknowledgement(slave1MacAddress, g_slaves[SLAVE_DIGITAL].name);
    }
    
    if (currentMillis - g_pzemBoard.lastAckSentTime >= ACK_INTERVAL_MS) {
        g_pzemBoard.lastAckSentTime = currentMillis;
        sendAcknowledgement(slave2MacAddress, g_slaves[SLAVE_PZEM].name);
    }
}

// System Status Printer
void printSystemStatus(SlaveType type, const char* eventType) {
    if (type == SLAVE_DIGITAL) {
        Serial.print("[SLAVE-1 ");
        Serial.print(eventType);
        Serial.print("] Current: ");
        Serial.print(g_digitalBoard.current, 2);
        Serial.print(" A | Relay: ");
        Serial.print(g_digitalBoard.relay == 1 ? "ON" : "OFF");
        Serial.print(" | Switch: ");
        Serial.print(g_digitalBoard.manualSwitch == 1 ? "PRESSED" : "RELEASED");
        Serial.print(" | RSSI: ");
        Serial.print(g_digitalBoard.health.rssi);
        Serial.println(" dBm");
    } else if (type == SLAVE_PZEM) {
        Serial.print("[SLAVE-2 ");
        Serial.print(eventType);
        Serial.print("] Voltage: ");
        Serial.print(g_pzemBoard.voltage, 1);
        Serial.print(" V | Current: ");
        Serial.print(g_pzemBoard.current, 3);
        Serial.print(" A | Power: ");
        Serial.print(g_pzemBoard.power, 1);
        Serial.print(" W | Energy: ");
        Serial.print(g_pzemBoard.energy, 1);
        Serial.print(" Wh | RSSI: ");
        Serial.print(g_pzemBoard.health.rssi);
        Serial.println(" dBm");
    }
}

// Common Helper to Update Communication Health
void updateSlaveHealth(SlaveCommHealth& health, int rssi) {
    health.rssi = rssi;
    health.lastSeenTime = millis();
    health.packetReceivedCounter++;
    health.onlineStatus = true;
}

// Change Detection Helper for Digital Board
bool hasDigitalBoardDataChanged(float incomingCurrent, int incomingRelay, bool hasSwitch, int incomingSwitch) {
    bool currentChanged = fabs(g_digitalBoard.current - incomingCurrent) > 0.01f;
    bool relayChanged = g_digitalBoard.relay != incomingRelay;
    bool switchChanged = hasSwitch ? (g_digitalBoard.manualSwitch != incomingSwitch) : false;
    return currentChanged || relayChanged || switchChanged;
}

// Change Detection Helper for PZEM Board
bool hasPzemDataChanged(float incomingVoltage, float incomingCurrent, float incomingPower, float incomingEnergy) {
    bool voltageChanged = fabs(g_pzemBoard.voltage - incomingVoltage) > 0.1f;
    bool currentChanged = fabs(g_pzemBoard.current - incomingCurrent) > 0.001f;
    bool powerChanged = fabs(g_pzemBoard.power - incomingPower) > 0.1f;
    bool energyChanged = fabs(g_pzemBoard.energy - incomingEnergy) > 0.1f;
    return voltageChanged || currentChanged || powerChanged || energyChanged;
}

// Dedicated Digital Board Packet Handler
void handleDigitalBoardPacket() {
    if (!slave1RxBuffer.pending) return;
    slave1RxBuffer.pending = false;

    #if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument doc;
    #else
    StaticJsonDocument<256> doc;
    #endif

    DeserializationError error = deserializeJson(doc, (uint8_t*)slave1RxBuffer.data, slave1RxBuffer.len);
    if (error) {
        Serial.print("[SLAVE-1 JSON ERROR] Deserialization failed: ");
        Serial.println(error.c_str());
        return;
    }

    updateSlaveHealth(g_digitalBoard.health, slave1RxBuffer.rssi);

    if (doc.containsKey("type")) {
        const char* type = doc["type"];
        bool isData = (strcmp(type, "data") == 0);
        bool isAckOk = (strcmp(type, "ack_ok") == 0);

        if (isData || isAckOk) {
            float incomingCurrent = doc["current"];
            int incomingRelay = doc["relay"];
            bool hasSwitch = doc.containsKey("switch");
            int incomingSwitch = hasSwitch ? doc["switch"].as<int>() : 0;

            if (hasDigitalBoardDataChanged(incomingCurrent, incomingRelay, hasSwitch, incomingSwitch)) {
                g_digitalBoard.current = incomingCurrent;
                g_digitalBoard.relay = incomingRelay;
                if (hasSwitch) {
                    g_digitalBoard.manualSwitch = incomingSwitch;
                }
                printSystemStatus(SLAVE_DIGITAL, isData ? "DATA" : "ACK REPLY");
            }
        }
    }
}

// Dedicated PZEM Packet Handler
void handlePzemPacket() {
    if (!slave2RxBuffer.pending) return;
    slave2RxBuffer.pending = false;

    #if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument doc;
    #else
    StaticJsonDocument<256> doc;
    #endif

    DeserializationError error = deserializeJson(doc, (uint8_t*)slave2RxBuffer.data, slave2RxBuffer.len);
    if (error) {
        Serial.print("[SLAVE-2 JSON ERROR] Deserialization failed: ");
        Serial.println(error.c_str());
        return;
    }

    updateSlaveHealth(g_pzemBoard.health, slave2RxBuffer.rssi);

    if (doc.containsKey("type")) {
        const char* type = doc["type"];
        bool isData = (strcmp(type, "data") == 0);
        bool isAckOk = (strcmp(type, "ack_ok") == 0);

        if (isData || isAckOk) {
            float incomingVoltage = doc["voltage"];
            float incomingCurrent = doc["current"];
            float incomingPower = doc["power"];
            float incomingEnergy = doc["energy"];

            if (hasPzemDataChanged(incomingVoltage, incomingCurrent, incomingPower, incomingEnergy)) {
                g_pzemBoard.voltage = incomingVoltage;
                g_pzemBoard.current = incomingCurrent;
                g_pzemBoard.power = incomingPower;
                g_pzemBoard.energy = incomingEnergy;
                printSystemStatus(SLAVE_PZEM, isData ? "DATA" : "ACK REPLY");
            }
        }
    }
}

// Unified Receive processing function
void handleReceiveProcessing() {
    handleDigitalBoardPacket();
    handlePzemPacket();
}

// Independent Slave Offline Detection
void checkSlaveStatus() {
    uint32_t currentMillis = millis();
    for (size_t i = 0; i < g_numSlaves; i++) {
        if (g_slaves[i].health->onlineStatus && 
            (currentMillis - g_slaves[i].health->lastSeenTime > SLAVE_TIMEOUT_MS)) {
            g_slaves[i].health->onlineStatus = false;
            Serial.print("[");
            Serial.print(g_slaves[i].name);
            Serial.println(" OFFLINE] No data for 60+ seconds");
        }
    }
}

// Volatile Command Buffer - Process, Validate, Route, and Clear
void processPendingCommand() {
    if (g_pendingCommand.length() == 0) return;

    bool isValid = false;
    SlaveType targetSlave = SLAVE_COUNT;
    String cmdToSend = "";

    if (g_pendingCommand == "relay_on" || g_pendingCommand == "relay_on_slave1") {
        isValid = true;
        targetSlave = SLAVE_DIGITAL;
        cmdToSend = "relay_on";
    } else if (g_pendingCommand == "relay_off" || g_pendingCommand == "relay_off_slave1") {
        isValid = true;
        targetSlave = SLAVE_DIGITAL;
        cmdToSend = "relay_off";
    } else if (g_pendingCommand == "relay_lock" || g_pendingCommand == "relay_lock_slave1") {
        isValid = true;
        targetSlave = SLAVE_DIGITAL;
        cmdToSend = "relay_lock";
    } else if (g_pendingCommand == "relay_unlock" || g_pendingCommand == "relay_unlock_slave1") {
        isValid = true;
        targetSlave = SLAVE_DIGITAL;
        cmdToSend = "relay_unlock";
    } else if (g_pendingCommand == "reboot" || g_pendingCommand == "reboot_slave1") {
        isValid = true;
        targetSlave = SLAVE_DIGITAL;
        cmdToSend = "reboot";
    } else if (g_pendingCommand == "reboot_slave2") {
        isValid = true;
        targetSlave = SLAVE_PZEM;
        cmdToSend = "reboot";
    } else if (g_pendingCommand == "energy_reset_slave2") {
        isValid = true;
        targetSlave = SLAVE_PZEM;
        cmdToSend = "energy_reset";
    }

    if (isValid) {
        if (targetSlave == SLAVE_DIGITAL) {
            sendMasterCommand(slave1MacAddress, cmdToSend);
        } else if (targetSlave == SLAVE_PZEM) {
            sendMasterCommand(slave2MacAddress, cmdToSend);
        }
    } else {
        Serial.print("[CMD ERROR] Unknown command: '");
        Serial.print(g_pendingCommand);
        Serial.println("'. Try: relay_on_slave1, relay_off_slave1, reboot_slave1, reboot_slave2, energy_reset_slave2");
    }

    g_pendingCommand = "";
}

// Non-blocking Serial character accumulator
void readSerial() {
    while (Serial.available() > 0) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (serialBufferIndex > 0) {
                serialCommandBuffer[serialBufferIndex] = '\0';
                g_pendingCommand = String(serialCommandBuffer);
                g_pendingCommand.trim();
                serialBufferIndex = 0;
            }
        } else {
            if (serialBufferIndex < sizeof(serialCommandBuffer) - 1) {
                serialCommandBuffer[serialBufferIndex++] = c;
            }
        }
    }
}

// Setup
void setup() {
    Serial.begin(115200);
    Serial.println("\n[SYSTEM START] ESP32 Master Initializing...");
    
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    Serial.print("[WIFI] Station mode initialized. MAC: ");
    Serial.println(WiFi.macAddress());
    
    if (esp_now_init() != ESP_OK) {
        Serial.println("[ERROR] ESP-NOW initialization failed");
        return;
    }
    Serial.println("[ESP-NOW] Initialized successfully");
    
    esp_now_register_recv_cb(onReceive);
    
    // Register Slave-1 Peer
    esp_now_peer_info_t peer1;
    memset(&peer1, 0, sizeof(peer1));
    memcpy(peer1.peer_addr, slave1MacAddress, 6);
    peer1.channel = 0;
    peer1.encrypt = false;
    if (esp_now_add_peer(&peer1) == ESP_OK) {
        Serial.println("[ESP-NOW] Slave-1 peer registered");
    } else {
        Serial.println("[ESP-NOW] Failed to register Slave-1 peer");
    }
    
    // Register Slave-2 Peer
    esp_now_peer_info_t peer2;
    memset(&peer2, 0, sizeof(peer2));
    memcpy(peer2.peer_addr, slave2MacAddress, 6);
    peer2.channel = 0;
    peer2.encrypt = false;
    if (esp_now_add_peer(&peer2) == ESP_OK) {
        Serial.println("[ESP-NOW] Slave-2 peer registered");
    } else {
        Serial.println("[ESP-NOW] Failed to register Slave-2 peer");
    }
    
    uint32_t current = millis();
    g_digitalBoard.health.lastSeenTime = current;
    g_digitalBoard.health.onlineStatus = true;
    g_digitalBoard.health.packetReceivedCounter = 0;
    
    g_pzemBoard.health.lastSeenTime = current;
    g_pzemBoard.health.onlineStatus = true;
    g_pzemBoard.health.packetReceivedCounter = 0;
    
    g_digitalBoard.lastAckSentTime = current;
    g_pzemBoard.lastAckSentTime = current - ACK_STAGGER_OFFSET_MS;
}

// Loop
void loop() {
    handlePeriodicAcks();
    readSerial();
    processPendingCommand();
    handleReceiveProcessing();
    checkSlaveStatus();
}
