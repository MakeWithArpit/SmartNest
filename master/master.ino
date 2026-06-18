#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <HardwareSerial.h>
#include <SD.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <math.h>

// Configuration & Constants
const uint8_t SLAVE1_MAC[] = {0x14, 0x08, 0x08, 0xA4, 0x94, 0x1C}; // Digital Slave
const uint8_t SLAVE2_MAC[] = {0x78, 0x21, 0x84, 0x9C, 0x98, 0x4C}; // PZEM Slave

const int SWITCH_PINS[6] = {13, 15, 19, 4, 32, 18};

// SD SPI Pins
#define SD_CS   5
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCK  18

// Hardware Structures & Centralized State
struct DigitalBoardState {
    float    rmsCurrent;
    uint8_t  relayState;   // 0 or 1
    uint8_t  switchState;  // 0 or 1
    bool     locked;       // Optimistic local lock flag
    int      rssi;
    uint32_t lastSeenTime;
    bool     online;
};

struct PzemBoardState {
    float    voltage;
    float    current;
    float    power;
    float    energy;
    int      rssi;
    uint32_t lastSeenTime;
    bool     online;
};

struct EnergyState {
    double   activePowerVA;
    double   dailyEnergy;      // Wh
    double   monthlyEnergy;    // Wh
    double   lifetimeEnergy;   // Wh
    uint32_t lastCalcTime;
    bool     pendingSave;
    double   lastSavedLifetime;
};

struct TimeState {
    uint32_t baseEpoch;
    uint32_t baseMillis;
    int8_t   tzHours;
    uint8_t  tzMins;
    bool     timeValid;
};

struct SdCardState {
    uint64_t totalBytes;
    uint64_t usedBytes;
    uint32_t droppedRecords;
    uint8_t  syncStatus;       // 0=Idle, 1=Syncing, 2=Error
    bool     cardPresent;
    uint32_t lastSyncedEpoch;
    uint32_t lastLoggedEpoch;
};

struct SystemData {
    DigitalBoardState digital;
    PzemBoardState    pzem;
    EnergyState       energy;
    TimeState         time;
    SdCardState       sd;
    float internetAcsCurrentA; // Combined current
    bool  cloudOnline;
    bool  masterLock;          // Global lock — all relays OFF, ignores commands
    // SD log access flags (set by uartTask on Core 0, consumed by sdLoggingTask on Core 1)
    bool  filesRequest;
    bool  readRequest;
    char  readFilename[64];
    int   readChunk;
    bool  factoryResetRequest;
};

SemaphoreHandle_t g_stateMutex = NULL;
SystemData        g_systemState;

// Queue definitions
struct EspNowPacket {
    uint8_t  srcMac[6];
    uint8_t  data[32];
    uint8_t  len;
    int      rssi;
};
QueueHandle_t g_espNowQueue = NULL;

struct UartTxItem {
    char     json[768];
};
QueueHandle_t g_uartTxQueue = NULL;

struct __attribute__((packed)) SdLogRecord {
    uint32_t epoch;
    float    voltage;
    float    acsCurrent; // Combined current
    float    pzemCurrent;
    float    powerVA;
    uint8_t  relayStates;
};
QueueHandle_t g_sdQueue = NULL;

struct SwitchEvent {
    uint8_t idx;
    bool    state;
};
QueueHandle_t g_switchQueue = NULL;

SemaphoreHandle_t g_histAckSem = NULL;

// Binary ESP-NOW Structs
struct __attribute__((packed)) CmdPacket {
    uint8_t type;
};

struct __attribute__((packed)) DigitalSlavePacket {
    uint8_t type;
    float   rmsCurrent;
    uint8_t relayState;
    uint8_t switchState;
};

struct __attribute__((packed)) PzemSlavePacket {
    uint8_t type;
    float   voltage;
    float   current;
    float   power;
    float   energy;
};

// Date / Time parsing helper
void epochToDateTime(uint32_t epoch, int &year, int &month, int &day, int &hour, int &minute, int &second) {
    uint32_t days = epoch / 86400;
    int time_of_day = epoch % 86400;
    hour = time_of_day / 3600;
    minute = (time_of_day % 3600) / 60;
    second = time_of_day % 60;
    
    int y = 1970;
    while (true) {
        bool isLeap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        int daysInYear = isLeap ? 366 : 365;
        if (days >= daysInYear) {
            days -= daysInYear;
            y++;
        } else {
            break;
        }
    }
    year = y;
    
    bool isLeap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    const int monthDays[12] = {31, isLeap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    month = 0;
    while (days >= monthDays[month]) {
        days -= monthDays[month];
        month++;
    }
    month += 1;
    day = days + 1;
}

// Binary search inside day's log file to seek to target record
int findFirstRecordAfter(File &file, uint32_t targetEpoch) {
    int low = 0;
    int high = (file.size() / 21) - 1;
    int ans = -1;
    while (low <= high) {
        int mid = low + (high - low) / 2;
        if (!file.seek(mid * 21)) break;
        uint32_t recEpoch = 0;
        if (file.read((uint8_t*)&recEpoch, 4) != 4) break;
        if (recEpoch > targetEpoch) {
            ans = mid;
            high = mid - 1;
        } else {
            low = mid + 1;
        }
    }
    return ans;
}

// ESP-NOW Receive Callback (ISR context)
void onReceive(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    EspNowPacket pkt;
    memcpy(pkt.srcMac, info->src_addr, 6);
    memcpy(pkt.data, data, len > 32 ? 32 : len);
    pkt.len = len;
    pkt.rssi = info->rx_ctrl->rssi;
    
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(g_espNowQueue, &pkt, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

// UART output helper
void enqueueUartTx(const char* jsonStr) {
    UartTxItem item;
    strncpy(item.json, jsonStr, sizeof(item.json) - 1);
    item.json[sizeof(item.json) - 1] = '\0';
    xQueueSend(g_uartTxQueue, &item, 0);
}

// Core 0 Tasks

void espNowTask(void* pvParameters) {
    EspNowPacket pkt;
    uint32_t lastPingTime = millis();
    bool pingDigital = true;
    
    while (true) {
        // Run staggered ACK ping checks
        uint32_t now = millis();
        if (now - lastPingTime >= 5000) {
            lastPingTime = now;
            CmdPacket pingPkt;
            pingPkt.type = 0x01;
            if (pingDigital) {
                esp_now_send(SLAVE1_MAC, (uint8_t*)&pingPkt, sizeof(pingPkt));
                pingDigital = false;
            } else {
                esp_now_send(SLAVE2_MAC, (uint8_t*)&pingPkt, sizeof(pingPkt));
                pingDigital = true;
            }
        }
        
        // Dequeue incoming packets
        if (xQueueReceive(g_espNowQueue, &pkt, pdMS_TO_TICKS(100)) == pdTRUE) {
            bool isSlave1 = (memcmp(pkt.srcMac, SLAVE1_MAC, 6) == 0);
            bool isSlave2 = (memcmp(pkt.srcMac, SLAVE2_MAC, 6) == 0);
            
            if (isSlave1) {
                if (pkt.len == sizeof(DigitalSlavePacket)) {
                    DigitalSlavePacket* p = (DigitalSlavePacket*)pkt.data;
                    if (p->type == 0x10 || p->type == 0x11) {
                        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            bool wasOffline = !g_systemState.digital.online;
                            g_systemState.digital.rmsCurrent = p->rmsCurrent;
                            g_systemState.digital.relayState = p->relayState;
                            g_systemState.digital.switchState = p->switchState;
                            g_systemState.digital.rssi = pkt.rssi;
                            g_systemState.digital.lastSeenTime = millis();
                            g_systemState.digital.online = true;
                            xSemaphoreGive(g_stateMutex);
                            
                            if (wasOffline) {
                                char buf[64];
                                snprintf(buf, sizeof(buf), "{\"t\":\"on\",\"dev\":\"d1\"}");
                                enqueueUartTx(buf);
                            }
                        }
                    }
                }
            } else if (isSlave2) {
                if (pkt.len == sizeof(PzemSlavePacket)) {
                    PzemSlavePacket* p = (PzemSlavePacket*)pkt.data;
                    if (p->type == 0x20 || p->type == 0x21) {
                        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            bool wasOffline = !g_systemState.pzem.online;
                            g_systemState.pzem.voltage = p->voltage;
                            g_systemState.pzem.current = p->current;
                            g_systemState.pzem.power = p->power;
                            g_systemState.pzem.energy = p->energy;
                            g_systemState.pzem.rssi = pkt.rssi;
                            g_systemState.pzem.lastSeenTime = millis();
                            g_systemState.pzem.online = true;
                            xSemaphoreGive(g_stateMutex);
                            
                            if (wasOffline) {
                                char buf[64];
                                snprintf(buf, sizeof(buf), "{\"t\":\"on\",\"dev\":\"pzem\"}");
                                enqueueUartTx(buf);
                            }
                        }
                    }
                }
            }
        }
    }
}

void uartTask(void* pvParameters) {
    char rxLine[256];
    int rxIndex = 0;
    UartTxItem txItem;
    
    // HardwareSerial on UART2
    HardwareSerial MasterUart(2);
    MasterUart.begin(115200, SERIAL_8N1, 16, 17); // RX=16, TX=17
    
    while (true) {
        // TX processing
        if (xQueueReceive(g_uartTxQueue, &txItem, pdMS_TO_TICKS(10)) == pdTRUE) {
            MasterUart.println(txItem.json);
        }
        
        // RX processing
        while (MasterUart.available() > 0) {
            char c = MasterUart.read();
            if (c == '\n') {
                rxLine[rxIndex] = '\0';
                
                // Parse commands
                char* pCmd = strstr(rxLine, "\"t\":\"cmd\"");
                char* pNtp = strstr(rxLine, "\"t\":\"ntp\"");
                char* pAcs = strstr(rxLine, "\"t\":\"acs\"");
                char* pCloud = strstr(rxLine, "\"t\":\"cloud\"");
                char* pHistAck = strstr(rxLine, "\"t\":\"hist_ack\"");
                char* pLock = strstr(rxLine, "\"t\":\"lock\"");
                char* pFilesReq = strstr(rxLine, "\"t\":\"files_req\"");
                char* pReadReq = strstr(rxLine, "\"t\":\"read_req\"");
                char* pFactoryReset = strstr(rxLine, "\"t\":\"factory_reset\"");
                
                if (pCmd) {
                    char* tgtPtr = strstr(rxLine, "\"tgt\":\"");
                    char* cPtr = strstr(rxLine, "\"cmd\":\"");
                    if (tgtPtr && cPtr) {
                        char tgt[16] = {0};
                        char cmd[32] = {0};
                        sscanf(tgtPtr + 7, "%[^\"]", tgt);
                        sscanf(cPtr + 7, "%[^\"]", cmd);
                        
                        uint8_t typeVal = 0;
                        const uint8_t* targetMac = NULL;
                        bool isD1 = (strcmp(tgt, "d1") == 0);
                        bool isPzem = (strcmp(tgt, "pzem") == 0);
                        
                        if (isD1) {
                            targetMac = SLAVE1_MAC;
                            if (strcmp(cmd, "relay_on") == 0) typeVal = 0x02;
                            else if (strcmp(cmd, "relay_off") == 0) typeVal = 0x03;
                            else if (strcmp(cmd, "relay_lock") == 0) {
                                typeVal = 0x04;
                                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                                    g_systemState.digital.locked = true;
                                    xSemaphoreGive(g_stateMutex);
                                }
                            }
                            else if (strcmp(cmd, "relay_unlock") == 0) {
                                typeVal = 0x05;
                                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                                    g_systemState.digital.locked = false;
                                    xSemaphoreGive(g_stateMutex);
                                }
                            }
                            else if (strcmp(cmd, "reboot") == 0) typeVal = 0x06;
                        } else if (isPzem) {
                            targetMac = SLAVE2_MAC;
                            if (strcmp(cmd, "reboot") == 0) typeVal = 0x06;
                            else if (strcmp(cmd, "energy_reset") == 0) typeVal = 0x07;
                        }
                        
                        if (typeVal != 0 && targetMac != NULL) {
                            CmdPacket cp;
                            cp.type = typeVal;
                            esp_now_send(targetMac, (uint8_t*)&cp, sizeof(cp));
                            Serial.printf("[Master] CMD -> tgt=%s cmd=%s type=0x%02X\n", tgt, cmd, typeVal);
                        }
                    }
                }
                else if (pNtp) {
                    char* epochPtr = strstr(rxLine, "\"epoch\":");
                    char* tzHPtr = strstr(rxLine, "\"tz_h\":");
                    char* tzMPtr = strstr(rxLine, "\"tz_m\":");
                    if (epochPtr && tzHPtr && tzMPtr) {
                        uint32_t newEpoch = strtoul(epochPtr + 8, NULL, 10);
                        int8_t tzH = (int8_t)strtol(tzHPtr + 7, NULL, 10);
                        uint8_t tzM = (uint8_t)strtoul(tzMPtr + 7, NULL, 10);
                        
                        uint32_t currentEpoch = 0;
                        bool hardStep = false;
                        bool gotMutex = false;
                        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            currentEpoch = g_systemState.time.baseEpoch + (millis() - g_systemState.time.baseMillis) / 1000;
                            uint32_t diff = (newEpoch > currentEpoch) ? (newEpoch - currentEpoch) : (currentEpoch - newEpoch);
                            hardStep = !g_systemState.time.timeValid || (diff >= 2);
                            
                            g_systemState.time.baseEpoch = newEpoch;
                            g_systemState.time.baseMillis = millis();
                            g_systemState.time.tzHours = tzH;
                            g_systemState.time.tzMins = tzM;
                            g_systemState.time.timeValid = true;
                            gotMutex = true;
                            xSemaphoreGive(g_stateMutex);
                        }
                        
                        if (gotMutex) {
                            Serial.println(hardStep ? "[NTP] Hard Step Sync Completed" : "[NTP] Smooth Sync Completed");
                        }
                    }
                }
                else if (pAcs) {
                    char* iPtr = strstr(rxLine, "\"i\":");
                    if (iPtr) {
                        float iVal = strtof(iPtr + 4, NULL);
                        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            g_systemState.internetAcsCurrentA = iVal;
                            xSemaphoreGive(g_stateMutex);
                        }
                    }
                }
                else if (pCloud) {
                    char* upPtr = strstr(rxLine, "\"up\":");
                    if (upPtr) {
                        bool upVal = (strncmp(upPtr + 5, "true", 4) == 0);
                        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            g_systemState.cloudOnline = upVal;
                            xSemaphoreGive(g_stateMutex);
                        }
                    }
                }
                else if (pHistAck) {
                    char* uptoPtr = strstr(rxLine, "\"upto\":");
                    if (uptoPtr) {
                        uint32_t uptoVal = strtoul(uptoPtr + 7, NULL, 10);
                        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            g_systemState.sd.lastSyncedEpoch = uptoVal;
                            xSemaphoreGive(g_stateMutex);
                        }
                        xSemaphoreGive(g_histAckSem);
                    }
                }
                // Master Lock: {"t":"lock","val":true/false}
                else if (pLock) {
                    char* valPtr = strstr(rxLine, "\"val\":");
                    if (valPtr) {
                        bool lockVal = (strncmp(valPtr + 5, "true", 4) == 0);
                        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            g_systemState.masterLock = lockVal;
                            xSemaphoreGive(g_stateMutex);
                        }
                        // Forward lock/unlock to Digital Board slave via ESP-NOW
                        CmdPacket cp;
                        cp.type = lockVal ? 0x04 : 0x05;
                        esp_now_send(SLAVE1_MAC, (uint8_t*)&cp, sizeof(cp));
                        Serial.printf("[Master] Master Lock -> %s (sent 0x%02X to Digital Board)\n", lockVal ? "ON" : "OFF", cp.type);
                    }
                }
                // SD Log file listing: {"t":"files_req"}
                else if (pFilesReq) {
                    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        g_systemState.filesRequest = true;
                        xSemaphoreGive(g_stateMutex);
                    }
                    Serial.println("[Master] files_req received — queued for sdLoggingTask");
                }
                // SD Log chunk read: {"t":"read_req","file":"...","chunk":N}
                else if (pReadReq) {
                    char* filePtr = strstr(rxLine, "\"file\":\"");
                    char* chunkPtr = strstr(rxLine, "\"chunk\":");
                    if (filePtr && chunkPtr) {
                        char fname[64] = {0};
                        sscanf(filePtr + 8, "%[^\"]", fname);
                        int chunkVal = (int)strtol(chunkPtr + 8, NULL, 10);
                        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            g_systemState.readRequest = true;
                            strncpy(g_systemState.readFilename, fname, sizeof(g_systemState.readFilename) - 1);
                            g_systemState.readFilename[sizeof(g_systemState.readFilename) - 1] = '\0';
                            g_systemState.readChunk = chunkVal;
                            xSemaphoreGive(g_stateMutex);
                        }
                        Serial.printf("[Master] read_req received — file=%s chunk=%d\n", fname, chunkVal);
                    }
                }
                // Factory Reset: {"t":"factory_reset"}
                else if (pFactoryReset) {
                    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        g_systemState.factoryResetRequest = true;
                        xSemaphoreGive(g_stateMutex);
                    }
                    Serial.println("[Master] factory_reset received — queued for sdLoggingTask");
                }
                
                rxIndex = 0;
            } else if (c != '\r') {
                if (rxIndex < sizeof(rxLine) - 1) {
                    rxLine[rxIndex++] = c;
                }
            }
        }
    }
}

void switchPollTask(void* pvParameters) {
    for (int i = 0; i < 6; i++) {
        pinMode(SWITCH_PINS[i], INPUT_PULLDOWN);
    }
    
    bool pinStates[6] = {false};
    bool lastDebouncedStates[6] = {false};
    uint32_t stableStartTimes[6] = {0};
    
    while (true) {
        uint32_t now = millis();
        for (int i = 0; i < 6; i++) {
            bool raw = (digitalRead(SWITCH_PINS[i]) == HIGH);
            if (raw != pinStates[i]) {
                stableStartTimes[i] = now;
                pinStates[i] = raw;
            } else if (now - stableStartTimes[i] >= 50) {
                if (raw != lastDebouncedStates[i]) {
                    lastDebouncedStates[i] = raw;
                    SwitchEvent ev;
                    ev.idx = i;
                    ev.state = raw;
                    xQueueSend(g_switchQueue, &ev, 0);
                }
            }
        }
        
        // Process outgoing switch events
        SwitchEvent outEv;
        if (xQueueReceive(g_switchQueue, &outEv, 0) == pdTRUE) {
            char buf[64];
            snprintf(buf, sizeof(buf), "{\"t\":\"sw\",\"idx\":%d,\"s\":%d}", outEv.idx, outEv.state ? 1 : 0);
            enqueueUartTx(buf);
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Core 1 Tasks

void energyTimeTask(void* pvParameters) {
    uint32_t lastTick = millis();
    uint32_t lastTxTime = 0;
    int lastDay = -1;
    int lastMonth = -1;
    
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        uint32_t now = millis();
        uint32_t deltaMs = now - lastTick;
        lastTick = now;
        
        double apparentPower = 0.0;
        double dailyEnergyVal = 0.0;
        double monthlyEnergyVal = 0.0;
        double lifetimeEnergyVal = 0.0;
        float rmsCurrentVal = 0.0f;
        int rssiVal = 0;
        int pzemRssiVal = 0;
        uint8_t relayStateVal = 0;
        uint8_t switchStateVal = 0;
        bool lockedVal = false;
        bool digitalOnlineVal = false;
        bool pzemOnlineVal = false;
        float pzemCurrentVal = 0.0f;
        float pzemPowerVal = 0.0f;
        float pzemVoltageVal = 0.0f;
        float combinedCurrent = 0.0f;
        bool masterLockVal = false;
        bool sdOkVal = false;
        uint64_t sdTotalVal = 0;
        uint64_t sdUsedVal = 0;
        bool gotMutex = false;
        
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            pzemVoltageVal = g_systemState.pzem.voltage;
            combinedCurrent = g_systemState.internetAcsCurrentA;
            apparentPower = (double)pzemVoltageVal * (double)combinedCurrent;
            g_systemState.energy.activePowerVA = apparentPower;
            
            double deltaWh = (apparentPower * (double)deltaMs) / 3600000.0;
            g_systemState.energy.dailyEnergy += deltaWh;
            g_systemState.energy.monthlyEnergy += deltaWh;
            g_systemState.energy.lifetimeEnergy += deltaWh;
            g_systemState.energy.pendingSave = true;
            
            dailyEnergyVal = g_systemState.energy.dailyEnergy;
            monthlyEnergyVal = g_systemState.energy.monthlyEnergy;
            lifetimeEnergyVal = g_systemState.energy.lifetimeEnergy;
            
            // Calendar check for resets
            if (g_systemState.time.timeValid) {
                uint32_t curEpoch = g_systemState.time.baseEpoch + (millis() - g_systemState.time.baseMillis) / 1000;
                int localEpoch = curEpoch + g_systemState.time.tzHours * 3600 + g_systemState.time.tzMins * 60;
                
                int y, m, d, hh, mm, ss;
                epochToDateTime(localEpoch, y, m, d, hh, mm, ss);
                
                if (lastDay == -1) {
                    uint32_t savedEpoch = g_systemState.energy.lastCalcTime;
                    if (savedEpoch > 0) {
                        uint32_t savedLocalEpoch = savedEpoch + g_systemState.time.tzHours * 3600 + g_systemState.time.tzMins * 60;
                        int y_s, m_s, d_s, hh_s, mm_s, ss_s;
                        epochToDateTime(savedLocalEpoch, y_s, m_s, d_s, hh_s, mm_s, ss_s);
                        if (y_s != y || m_s != m) {
                            g_systemState.energy.dailyEnergy = 0.0;
                            g_systemState.energy.monthlyEnergy = 0.0;
                            dailyEnergyVal = 0.0;
                            monthlyEnergyVal = 0.0;
                            Serial.println("[ENERGY] Restored state from different month. Daily & Monthly reset.");
                        } else if (d_s != d) {
                            g_systemState.energy.dailyEnergy = 0.0;
                            dailyEnergyVal = 0.0;
                            Serial.println("[ENERGY] Restored state from different day. Daily reset.");
                        }
                    }
                    lastDay = d;
                    lastMonth = m;
                } else {
                    if (m != lastMonth) {
                        g_systemState.energy.monthlyEnergy = 0.0;
                        g_systemState.energy.dailyEnergy = 0.0;
                        dailyEnergyVal = 0.0;
                        monthlyEnergyVal = 0.0;
                        lastMonth = m;
                        lastDay = d;
                        Serial.println("[ENERGY] Monthly & Daily Counters Reset");
                    }
                    else if (d != lastDay) {
                        g_systemState.energy.dailyEnergy = 0.0;
                        dailyEnergyVal = 0.0;
                        lastDay = d;
                        Serial.println("[ENERGY] Daily Counter Reset");
                    }
                }
            }
            
            pzemCurrentVal = g_systemState.pzem.current;
            pzemPowerVal = g_systemState.pzem.power;
            rmsCurrentVal = g_systemState.digital.rmsCurrent;
            digitalOnlineVal = g_systemState.digital.online;
            pzemOnlineVal = g_systemState.pzem.online;
            rssiVal = g_systemState.digital.rssi;
            pzemRssiVal = g_systemState.pzem.rssi;
            relayStateVal = g_systemState.digital.relayState;
            switchStateVal = g_systemState.digital.switchState;
            lockedVal = g_systemState.digital.locked;
            masterLockVal = g_systemState.masterLock;
            sdOkVal = g_systemState.sd.cardPresent;
            sdTotalVal = g_systemState.sd.totalBytes;
            sdUsedVal = g_systemState.sd.usedBytes;
            
            gotMutex = true;
            xSemaphoreGive(g_stateMutex);
        }
        
        if (gotMutex) {
            // Periodical telemetry & logging every 10s
            if (now - lastTxTime >= 10000) {
                lastTxTime = now;
                
                // Log Binary Record
                bool isTimeValid = false;
                uint32_t curEpoch = 0;
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    isTimeValid = g_systemState.time.timeValid;
                    if (isTimeValid) {
                        curEpoch = g_systemState.time.baseEpoch + (millis() - g_systemState.time.baseMillis) / 1000;
                        g_systemState.sd.lastLoggedEpoch = curEpoch;
                    }
                    xSemaphoreGive(g_stateMutex);
                }
                
                if (isTimeValid) {
                    SdLogRecord rec;
                    rec.epoch = curEpoch;
                    rec.voltage = pzemVoltageVal;
                    rec.acsCurrent = combinedCurrent;
                    rec.pzemCurrent = pzemCurrentVal;
                    rec.powerVA = apparentPower;
                    rec.relayStates = 0;
                    
                    // overflow logic for sd queue
                    if (xQueueSend(g_sdQueue, &rec, 0) != pdTRUE) {
                        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            g_systemState.sd.droppedRecords++;
                            xSemaphoreGive(g_stateMutex);
                        }
                        SdLogRecord discard;
                        xQueueReceive(g_sdQueue, &discard, 0);
                        xQueueSend(g_sdQueue, &rec, 0);
                    }
                }
                
                // Build and send telemetry (with SD stats + master lock)
                char tel[640];
                snprintf(tel, sizeof(tel),
                         "{\"t\":\"tel\",\"acs\":%.2f,\"v\":%.1f,\"pi\":%.3f,\"pp\":%.1f,"
                         "\"d_wh\":%.1f,\"m_wh\":%.1f,\"l_wh\":%.1f,"
                         "\"d_on\":%s,\"p_on\":%s,\"d_rssi\":%d,\"p_rssi\":%d,"
                         "\"d_relay\":%d,\"d_sw\":%d,\"d_lock\":%s,"
                         "\"sd_ok\":%s,\"sd_total\":%llu,\"sd_used\":%llu,"
                         "\"m_lock\":%s}",
                         rmsCurrentVal, pzemVoltageVal, pzemCurrentVal, pzemPowerVal,
                         dailyEnergyVal, monthlyEnergyVal, lifetimeEnergyVal,
                         digitalOnlineVal ? "true" : "false", pzemOnlineVal ? "true" : "false",
                         rssiVal, pzemRssiVal,
                         relayStateVal, switchStateVal,
                         lockedVal ? "true" : "false",
                         sdOkVal ? "true" : "false",
                         (unsigned long long)sdTotalVal, (unsigned long long)sdUsedVal,
                         masterLockVal ? "true" : "false");
                
                enqueueUartTx(tel);
            }
        }
    }
}

void sdLoggingTask(void* pvParameters) {
    // SPI Init
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    bool sdOk = SD.begin(SD_CS);
    
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        g_systemState.sd.cardPresent = sdOk;
        g_systemState.sd.syncStatus = 0;
        g_systemState.sd.lastSyncedEpoch = 0;
        g_systemState.sd.lastLoggedEpoch = 0;
        g_systemState.sd.droppedRecords = 0;
        xSemaphoreGive(g_stateMutex);
    }
    
    if (sdOk) {
        SD.mkdir("/logs");
        
        // Restore State
        File stateFile = SD.open("/state.bin", FILE_READ);
        if (stateFile) {
            double savedDaily = 0, savedMonthly = 0, savedLifetime = 0;
            uint32_t savedEpoch = 0;
            if (stateFile.size() == 28) {
                stateFile.read((uint8_t*)&savedDaily, 8);
                stateFile.read((uint8_t*)&savedMonthly, 8);
                stateFile.read((uint8_t*)&savedLifetime, 8);
                stateFile.read((uint8_t*)&savedEpoch, 4);
                
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    g_systemState.energy.dailyEnergy = savedDaily;
                    g_systemState.energy.monthlyEnergy = savedMonthly;
                    g_systemState.energy.lifetimeEnergy = savedLifetime;
                    g_systemState.energy.lastSavedLifetime = savedLifetime;
                    g_systemState.energy.lastCalcTime = savedEpoch;
                    xSemaphoreGive(g_stateMutex);
                    Serial.printf("[SD] Restored energy. Lifetime: %.2f Wh\n", savedLifetime);
                }
            }
            stateFile.close();
        }
        
        // Restore Sync Pointer
        File syncFile = SD.open("/sync.bin", FILE_READ);
        if (syncFile) {
            uint32_t savedSync = 0;
            if (syncFile.size() == 4) {
                syncFile.read((uint8_t*)&savedSync, 4);
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    g_systemState.sd.lastSyncedEpoch = savedSync;
                    xSemaphoreGive(g_stateMutex);
                    Serial.printf("[SD] Restored sync epoch: %u\n", savedSync);
                }
            }
            syncFile.close();
        }
    }
    
    uint32_t lastStateSaveTime = millis();
    
    while (true) {
        // Log incoming records
        SdLogRecord rec;
        if (xQueueReceive(g_sdQueue, &rec, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (sdOk) {
                // Determine filename (local time)
                int8_t tzH = 5;
                uint8_t tzM = 30;
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    tzH = g_systemState.time.tzHours;
                    tzM = g_systemState.time.tzMins;
                    xSemaphoreGive(g_stateMutex);
                }
                
                uint32_t localEpoch = rec.epoch + tzH * 3600 + tzM * 60;
                int y, m, d, hh, mm, ss;
                epochToDateTime(localEpoch, y, m, d, hh, mm, ss);
                
                char filename[32];
                snprintf(filename, sizeof(filename), "/logs/%04d_%02d_%02d.dat", y, m, d);
                
                File logFile = SD.open(filename, FILE_WRITE);
                if (logFile) {
                    logFile.seek(logFile.size());
                    logFile.write((uint8_t*)&rec, sizeof(rec));
                    logFile.close();
                } else {
                    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        g_systemState.sd.droppedRecords++;
                        xSemaphoreGive(g_stateMutex);
                    }
                }
            }
        }
        
        // --- Handle files_req (scan /logs directory on Core 1) ---
        bool wantFiles = false;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            wantFiles = g_systemState.filesRequest;
            if (wantFiles) g_systemState.filesRequest = false;
            xSemaphoreGive(g_stateMutex);
        }
        if (wantFiles && sdOk) {
            Serial.println("[SD] Scanning /logs directory...");
            File logsDir = SD.open("/logs");
            char jsonBuf[512];
            int written = snprintf(jsonBuf, sizeof(jsonBuf), "{\"t\":\"files_res\",\"files\":[");
            bool first = true;
            if (logsDir && logsDir.isDirectory()) {
                File entry = logsDir.openNextFile();
                while (entry) {
                    const char* name = entry.name();
                    if (!entry.isDirectory() && name) {
                        if (!first && written + 2 < (int)sizeof(jsonBuf)) {
                            jsonBuf[written++] = ',';
                        }
                        int needed = snprintf(jsonBuf + written, sizeof(jsonBuf) - written, "\"%s\"", name);
                        if (written + needed < (int)sizeof(jsonBuf)) {
                            written += needed;
                            first = false;
                        }
                    }
                    entry.close();
                    entry = logsDir.openNextFile();
                }
                logsDir.close();
            }
            snprintf(jsonBuf + written, sizeof(jsonBuf) - written, "]}");
            enqueueUartTx(jsonBuf);
            Serial.printf("[SD] files_res sent (%d bytes)\n", (int)strlen(jsonBuf));
        }
        
        // --- Handle read_req (read 10 records at chunk offset on Core 1) ---
        bool wantRead = false;
        char readFname[64] = {0};
        int readChunkVal = 0;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            wantRead = g_systemState.readRequest;
            if (wantRead) {
                g_systemState.readRequest = false;
                strncpy(readFname, g_systemState.readFilename, sizeof(readFname) - 1);
                readChunkVal = g_systemState.readChunk;
            }
            xSemaphoreGive(g_stateMutex);
        }
        if (wantRead && sdOk) {
            char fullPath[80];
            snprintf(fullPath, sizeof(fullPath), "/logs/%s", readFname);
            Serial.printf("[SD] Reading %s chunk %d...\n", fullPath, readChunkVal);
            
            File readFile = SD.open(fullPath, FILE_READ);
            if (readFile) {
                int totalRecs = readFile.size() / sizeof(SdLogRecord);
                int startRec = readChunkVal * 10;
                char jsonBuf[768];
                int written = snprintf(jsonBuf, sizeof(jsonBuf), 
                    "{\"t\":\"read_res\",\"file\":\"%s\",\"chunk\":%d,\"total\":%d,\"recs\":[", 
                    readFname, readChunkVal, totalRecs);
                
                if (startRec < totalRecs) {
                    readFile.seek(startRec * sizeof(SdLogRecord));
                    bool first = true;
                    for (int i = 0; i < 10 && (startRec + i) < totalRecs; i++) {
                        SdLogRecord r;
                        if (readFile.read((uint8_t*)&r, sizeof(r)) == sizeof(r)) {
                            char recBuf[128];
                            int n = snprintf(recBuf, sizeof(recBuf),
                                "%s{\"epoch\":%u,\"v\":%.1f,\"load\":%.2f,\"pi\":%.3f,\"pva\":%.1f}",
                                first ? "" : ",",
                                r.epoch, r.voltage, r.acsCurrent, r.pzemCurrent, r.powerVA);
                            if (written + n + 5 < (int)sizeof(jsonBuf)) {
                                strcpy(jsonBuf + written, recBuf);
                                written += n;
                                first = false;
                            }
                        }
                    }
                }
                snprintf(jsonBuf + written, sizeof(jsonBuf) - written, "]}");
                readFile.close();
                enqueueUartTx(jsonBuf);
                Serial.printf("[SD] read_res sent for %s chunk %d\n", readFname, readChunkVal);
            } else {
                char errBuf[128];
                snprintf(errBuf, sizeof(errBuf), "{\"t\":\"read_res\",\"file\":\"%s\",\"chunk\":%d,\"total\":0,\"recs\":[]}", readFname, readChunkVal);
                enqueueUartTx(errBuf);
                Serial.printf("[SD] read_req FAIL — file %s not found\n", fullPath);
            }
        }
        
        // --- Handle factory_reset (delete state files, reset counters on Core 1) ---
        bool wantReset = false;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            wantReset = g_systemState.factoryResetRequest;
            if (wantReset) g_systemState.factoryResetRequest = false;
            xSemaphoreGive(g_stateMutex);
        }
        if (wantReset && sdOk) {
            Serial.println("[SD] FACTORY RESET — deleting /state.bin and /sync.bin...");
            SD.remove("/state.bin");
            SD.remove("/sync.bin");
            if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                g_systemState.energy.dailyEnergy = 0.0;
                g_systemState.energy.monthlyEnergy = 0.0;
                g_systemState.energy.lifetimeEnergy = 0.0;
                g_systemState.energy.lastSavedLifetime = 0.0;
                g_systemState.energy.lastCalcTime = 0;
                g_systemState.energy.pendingSave = false;
                g_systemState.sd.lastSyncedEpoch = 0;
                xSemaphoreGive(g_stateMutex);
            }
            Serial.println("[SD] Factory reset complete. Energy counters zeroed.");
        }
        
        // Check for state save triggers
        uint32_t now = millis();
        bool pendingSave = false;
        double currentLifetime = 0;
        double lastSavedLifetime = 0;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            pendingSave = g_systemState.energy.pendingSave;
            currentLifetime = g_systemState.energy.lifetimeEnergy;
            lastSavedLifetime = g_systemState.energy.lastSavedLifetime;
            xSemaphoreGive(g_stateMutex);
        }
        
        bool saveTriggered = (pendingSave && (now - lastStateSaveTime >= 300000)) || 
                             ((currentLifetime - lastSavedLifetime) > 20.0);
        
        if (sdOk && saveTriggered) {
            lastStateSaveTime = now;
            File stateFile = SD.open("/state.bin", FILE_WRITE);
            if (stateFile) {
                double d = 0, m = 0, l = 0;
                uint32_t curEpoch = 0;
                bool gotMutex = false;
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    d = g_systemState.energy.dailyEnergy;
                    m = g_systemState.energy.monthlyEnergy;
                    l = g_systemState.energy.lifetimeEnergy;
                    if (g_systemState.time.timeValid) {
                        curEpoch = g_systemState.time.baseEpoch + (millis() - g_systemState.time.baseMillis) / 1000;
                    }
                    g_systemState.energy.pendingSave = false;
                    g_systemState.energy.lastSavedLifetime = l;
                    xSemaphoreGive(g_stateMutex);
                    gotMutex = true;
                }
                if (gotMutex) {
                    stateFile.write((uint8_t*)&d, 8);
                    stateFile.write((uint8_t*)&m, 8);
                    stateFile.write((uint8_t*)&l, 8);
                    stateFile.write((uint8_t*)&curEpoch, 4);
                    stateFile.close();
                    Serial.printf("[SD] Saved state: lifetime %.2f Wh\n", l);
                } else {
                    stateFile.close();
                }
            }
        }
        
        // Backfill loop (process one batch)
        bool cloudOnline = false;
        uint32_t lastSyncedEpoch = 0;
        uint32_t lastLoggedEpoch = 0;
        int8_t tzH_bf = 5;
        uint8_t tzM_bf = 30;
        bool gotBfMutex = false;
        
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            cloudOnline = g_systemState.cloudOnline;
            lastSyncedEpoch = g_systemState.sd.lastSyncedEpoch;
            lastLoggedEpoch = g_systemState.sd.lastLoggedEpoch;
            tzH_bf = g_systemState.time.tzHours;
            tzM_bf = g_systemState.time.tzMins;
            if (sdOk) {
                g_systemState.sd.totalBytes = SD.totalBytes();
                g_systemState.sd.usedBytes = SD.usedBytes();
            }
            gotBfMutex = true;
            xSemaphoreGive(g_stateMutex);
        }
        
        if (gotBfMutex && sdOk && cloudOnline && (lastSyncedEpoch < lastLoggedEpoch)) {
            uint32_t searchEpoch = lastSyncedEpoch + 1;
            uint32_t searchLocalEpoch = searchEpoch + tzH_bf * 3600 + tzM_bf * 60;
            int y, m, d, hh, mm, ss;
            epochToDateTime(searchLocalEpoch, y, m, d, hh, mm, ss);
            
            char filename[32];
            snprintf(filename, sizeof(filename), "/logs/%04d_%02d_%02d.dat", y, m, d);
            
            if (!SD.exists(filename)) {
                // Advance past this day start
                uint32_t nextLocalDayStart = ((searchLocalEpoch / 86400) + 1) * 86400;
                uint32_t nextUtcDayStart = nextLocalDayStart - tzH_bf * 3600 - tzM_bf * 60;
                
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    if (nextUtcDayStart < lastLoggedEpoch) {
                        g_systemState.sd.lastSyncedEpoch = nextUtcDayStart - 1;
                    } else {
                        g_systemState.sd.lastSyncedEpoch = lastLoggedEpoch;
                    }
                    uint32_t updatedEpoch = g_systemState.sd.lastSyncedEpoch;
                    g_systemState.sd.syncStatus = (updatedEpoch >= lastLoggedEpoch) ? 0 : 1;
                    xSemaphoreGive(g_stateMutex);
                    
                    File sFile = SD.open("/sync.bin", FILE_WRITE);
                    if (sFile) {
                        sFile.write((uint8_t*)&updatedEpoch, 4);
                        sFile.close();
                    }
                }
            } else {
                File logFile = SD.open(filename, FILE_READ);
                if (logFile) {
                    int firstRecIdx = findFirstRecordAfter(logFile, lastSyncedEpoch);
                    SdLogRecord batchRecs[10];
                    int count = 0;
                    if (firstRecIdx != -1) {
                        if (logFile.seek(firstRecIdx * 21)) {
                            while (logFile.available() >= sizeof(SdLogRecord) && count < 10) {
                                logFile.read((uint8_t*)&batchRecs[count], sizeof(SdLogRecord));
                                count++;
                            }
                        }
                    }
                    logFile.close();
                    
                    if (count > 0) {
                        static uint32_t batchNum = 1;
                        char jsonBuf[768];
                        int written = snprintf(jsonBuf, sizeof(jsonBuf), "{\"t\":\"hist\",\"batch\":%u,\"recs\":[", batchNum);
                        
                        for (int i = 0; i < count; i++) {
                            char recBuf[128];
                            snprintf(recBuf, sizeof(recBuf),
                                     "{\"epoch\":%u,\"v\":%.1f,\"load\":%.2f,\"pi\":%.3f,\"powerVA\":%.1f}%s",
                                     batchRecs[i].epoch, batchRecs[i].voltage, batchRecs[i].acsCurrent,
                                     batchRecs[i].pzemCurrent, batchRecs[i].powerVA,
                                     (i < count - 1) ? "," : "");
                            if (written + strlen(recBuf) + 5 < sizeof(jsonBuf)) {
                                    strcpy(jsonBuf + written, recBuf);
                                    written += strlen(recBuf);
                            }
                        }
                        strcat(jsonBuf, "]}");
                        
                        // Send and wait for hist_ack
                        UartTxItem txItem;
                        strncpy(txItem.json, jsonBuf, sizeof(txItem.json) - 1);
                        txItem.json[sizeof(txItem.json) - 1] = '\0';
                        
                        int retry = 0;
                        bool success = false;
                        while (retry < 3) {
                            xSemaphoreTake(g_histAckSem, 0); // Drain stale latch
                            if (xQueueSend(g_uartTxQueue, &txItem, 0) == pdTRUE) {
                                if (xSemaphoreTake(g_histAckSem, pdMS_TO_TICKS(5000)) == pdTRUE) {
                                    success = true;
                                    break;
                                }
                            } else {
                                vTaskDelay(pdMS_TO_TICKS(100));
                            }
                            retry++;
                        }
                        
                        if (success) {
                            if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                                uint32_t updatedEpoch = g_systemState.sd.lastSyncedEpoch;
                                g_systemState.sd.syncStatus = (updatedEpoch >= lastLoggedEpoch) ? 0 : 1;
                                xSemaphoreGive(g_stateMutex);
                                
                                File sFile = SD.open("/sync.bin", FILE_WRITE);
                                if (sFile) {
                                    sFile.write((uint8_t*)&updatedEpoch, 4);
                                    sFile.close();
                                }
                                batchNum++;
                            }
                        } else {
                            if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                                g_systemState.sd.syncStatus = 2;
                                xSemaphoreGive(g_stateMutex);
                            }
                            vTaskDelay(pdMS_TO_TICKS(30000)); // Sleep 30 seconds
                        }
                    } else {
                        // All records in file processed. Advance sync pointer to end of day.
                        uint32_t nextLocalDayStart = ((searchLocalEpoch / 86400) + 1) * 86400;
                        uint32_t nextUtcDayStart = nextLocalDayStart - tzH_bf * 3600 - tzM_bf * 60;
                        
                        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            if (nextUtcDayStart < lastLoggedEpoch) {
                                g_systemState.sd.lastSyncedEpoch = nextUtcDayStart - 1;
                            } else {
                                g_systemState.sd.lastSyncedEpoch = lastLoggedEpoch;
                            }
                            uint32_t updatedEpoch = g_systemState.sd.lastSyncedEpoch;
                            g_systemState.sd.syncStatus = (updatedEpoch >= lastLoggedEpoch) ? 0 : 1;
                            xSemaphoreGive(g_stateMutex);
                            
                            File sFile = SD.open("/sync.bin", FILE_WRITE);
                            if (sFile) {
                                sFile.write((uint8_t*)&updatedEpoch, 4);
                                sFile.close();
                            }
                        }
                    }
                }
            }
        }
    }
}

void healthTask(void* pvParameters) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        
        uint32_t now = millis();
        
        bool digitalOfflineAlert = false;
        bool pzemOfflineAlert = false;
        
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (g_systemState.digital.online && (now - g_systemState.digital.lastSeenTime > 30000)) {
                g_systemState.digital.online = false;
                digitalOfflineAlert = true;
            }
            xSemaphoreGive(g_stateMutex);
        }
        
        if (digitalOfflineAlert) {
            enqueueUartTx("{\"t\":\"off\",\"dev\":\"d1\"}");
        }
        
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (g_systemState.pzem.online && (now - g_systemState.pzem.lastSeenTime > 30000)) {
                g_systemState.pzem.online = false;
                pzemOfflineAlert = true;
            }
            xSemaphoreGive(g_stateMutex);
        }
        
        if (pzemOfflineAlert) {
            enqueueUartTx("{\"t\":\"off\",\"dev\":\"pzem\"}");
        }
    }
}

// Setup
void setup() {
    Serial.begin(115200);
    Serial.println("\n[SYSTEM] Master Initializing...");
    
    // Create mutex & queues
    g_stateMutex = xSemaphoreCreateMutex();
    g_espNowQueue = xQueueCreate(10, sizeof(EspNowPacket));
    g_uartTxQueue = xQueueCreate(15, sizeof(UartTxItem));
    g_sdQueue = xQueueCreate(30, sizeof(SdLogRecord));
    g_switchQueue = xQueueCreate(10, sizeof(SwitchEvent));
    g_histAckSem = xSemaphoreCreateBinary();
    
    if (!g_stateMutex || !g_espNowQueue || !g_uartTxQueue || !g_sdQueue || !g_switchQueue || !g_histAckSem) {
        Serial.println("[ERROR] Failed to create FreeRTOS primitives");
        delay(1000);
        ESP.restart();
    }
    
    // Initialize system state
    memset(&g_systemState, 0, sizeof(g_systemState));
    
    // WiFi Station mode before ESP-NOW to prevent boot looping
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    
    if (esp_now_init() != ESP_OK) {
        Serial.println("[ERROR] ESP-NOW initialization failed");
        delay(1000);
        ESP.restart();
    }
    
    esp_now_register_recv_cb(onReceive);
    
    // Peer registration
    esp_now_peer_info_t peer1;
    memset(&peer1, 0, sizeof(peer1));
    memcpy(peer1.peer_addr, SLAVE1_MAC, 6);
    peer1.channel = 0;
    peer1.encrypt = false;
    if (esp_now_add_peer(&peer1) != ESP_OK) {
        Serial.println("[ERROR] Failed to add Digital Board peer");
    }
    
    esp_now_peer_info_t peer2;
    memset(&peer2, 0, sizeof(peer2));
    memcpy(peer2.peer_addr, SLAVE2_MAC, 6);
    peer2.channel = 0;
    peer2.encrypt = false;
    if (esp_now_add_peer(&peer2) != ESP_OK) {
        Serial.println("[ERROR] Failed to add PZEM Board peer");
    }
    
    // Launch FreeRTOS Tasks
    // Core 0
    xTaskCreatePinnedToCore(espNowTask, "espNowTask", 4096, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(uartTask, "uartTask", 4096, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(switchPollTask, "switchPollTask", 2048, NULL, 3, NULL, 0);
    
    // Core 1
    xTaskCreatePinnedToCore(energyTimeTask, "energyTimeTask", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(sdLoggingTask, "sdLoggingTask", 8192, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(healthTask, "healthTask", 2048, NULL, 1, NULL, 1);
    
    Serial.println("[SYSTEM] Master Initialization Successful. Tasks started.");
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}
