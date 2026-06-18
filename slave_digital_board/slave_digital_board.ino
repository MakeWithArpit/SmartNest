#include <WiFi.h>
#include <esp_now.h>

struct __attribute__((packed)) CmdPacket {
    uint8_t type;
};

struct __attribute__((packed)) DigitalSlavePacket {
    uint8_t type;
    float   rmsCurrent;
    uint8_t relayState;
    uint8_t switchState;
};


// Pins
#define RELAY_PIN            15   
#define CURRENT_SENSOR_PIN   34
#define MANUAL_SWITCH_PIN    2  

#define RED_LED_PIN          5
#define GREEN_LED_PIN        18
#define BLUE_LED_PIN         19

// Global State Variables
uint8_t masterMacAddress[] = {0x88, 0x57, 0x21, 0xB1, 0xD3, 0x74}; 

bool overcurrentLock = false;
bool masterLock = false;
bool relayCondition = false;
float currentAmperes = 0.0f;
float zeroMilliVolts = 0.0f;

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
void onReceive(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    portENTER_CRITICAL_ISR(&receiveMux);
    int copyLen = len > 250 ? 250 : len;
    memcpy(receiveBuffer, data, copyLen);
    receiveLength = copyLen;
    messageReceived = true;
    portEXIT_CRITICAL_ISR(&receiveMux);
}

// Relay Control
void relayOn() {
    if (masterLock || overcurrentLock) return;
    digitalWrite(RELAY_PIN, LOW);
    relayCondition = true;
}

void relayOff() {
    digitalWrite(RELAY_PIN, HIGH);
    relayCondition = false;
}

// 1 kHz non-blocking current sampler
void sampleCurrentNonBlocking() {
    uint32_t currentMicros = micros();
    static uint32_t lastSampleMicros = 0;
    
    if (currentMicros - lastSampleMicros >= 1000) {
        lastSampleMicros = currentMicros;
        
        float milliVolts = analogReadMilliVolts(CURRENT_SENSOR_PIN);
        float deltaMilliVolts = milliVolts - zeroMilliVolts;
        float instCurrent = deltaMilliVolts / 66.0f; // 66 mV/A sensitivity for ACS712-30A
        
        sqSum += (double)(instCurrent * instCurrent);
        sampleCount++;
    }
}

// Overcurrent Protection check (Threshold: 6.0A RMS)
void checkOvercurrentProtection() {
    if (currentAmperes >= 6.0f) {
        overcurrentLock = true;
        relayOff();
        Serial.print("[CRITICAL ERROR] Overcurrent detected: ");
        Serial.print(currentAmperes, 2);
        Serial.println(" A! Relay locked OFF.");
    }
    
    if (overcurrentLock) {
        if (currentAmperes < 5.5f) {
            consecutiveLowCurrentCount++;
            if (consecutiveLowCurrentCount >= 3) { // Stays below 5.5A for 600ms total
                overcurrentLock = false;
                consecutiveLowCurrentCount = 0;
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
    
    bool rawState = digitalRead(MANUAL_SWITCH_PIN);
    unsigned long currentTime = millis();
    
    if (rawState != lastRawState) {
        stateStableTime = currentTime;
        lastRawState = rawState;
    } else if (currentTime - stateStableTime >= 50) {
        if (rawState != lastDebouncedState) {
            lastDebouncedState = rawState;
            
            if (rawState == HIGH) {
                if (masterLock || overcurrentLock) {
                    Serial.println("Switch turned ON, but ignored due to lock");
                } else {
                    relayOn();
                }
            } else {
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
    } else if (masterLock) {
        // Master Lock: Cyan solid
        setLEDColor(false, true, true);
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
    pkt.type        = 0x10;
    pkt.rmsCurrent  = relayCondition ? currentAmperes : 0.0f;
    pkt.relayState  = relayCondition ? 1 : 0;
    pkt.switchState = (digitalRead(MANUAL_SWITCH_PIN) == LOW) ? 1 : 0;
    esp_now_send(masterMacAddress, (uint8_t*)&pkt, sizeof(pkt));
}

void sendAckReply() {
    DigitalSlavePacket pkt;
    pkt.type        = 0x11;
    pkt.rmsCurrent  = currentAmperes;
    pkt.relayState  = relayCondition ? 1 : 0;
    pkt.switchState = 0;
    esp_now_send(masterMacAddress, (uint8_t*)&pkt, sizeof(pkt));
}


// Setup
void setup() {
    Serial.begin(115200);
    
    pinMode(RED_LED_PIN, OUTPUT);
    pinMode(GREEN_LED_PIN, OUTPUT);
    pinMode(BLUE_LED_PIN, OUTPUT);
    
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, HIGH); // Start with Relay OFF (Active HIGH)
    
    pinMode(MANUAL_SWITCH_PIN, INPUT_PULLUP);
    pinMode(CURRENT_SENSOR_PIN, INPUT);
    analogSetAttenuation(ADC_11db);
    analogReadResolution(12);  
    
    setLEDColor(true, true, true);
    
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    
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
            cmdType = ((CmdPacket*)receiveBuffer)->type;
        }
        portEXIT_CRITICAL(&receiveMux);
        
        if (len >= 1) {
            switch (cmdType) {
                case 0x01:  // ACK ping
                    lastAcknowledgementTime = millis();
                    sendAckReply();
                    break;
                case 0x02: relayOn();  break;
                case 0x03: relayOff(); break;
                case 0x04: masterLock = true;  relayOff(); break;
                case 0x05: masterLock = false; break;
                case 0x06: delay(100); ESP.restart(); break;
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
    
    static bool lastRelayCondition = false;
    static unsigned long lastTelemetryTime = 0;
    unsigned long currentTime = millis();
    
    if (relayCondition != lastRelayCondition || (currentTime - lastTelemetryTime >= 30000)) {
        lastRelayCondition = relayCondition;
        lastTelemetryTime = currentTime;
        
        if (lastAcknowledgementTime > 0) {
            sendSlaveData();
        }
    }
    
    updateLEDState();
}
