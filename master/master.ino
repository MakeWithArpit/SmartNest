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
const uint8_t MASTER_MAC[] = {0x88, 0x57, 0x21, 0xB1, 0xD3, 0x74};
const uint8_t SLAVE1_MAC[] = {0x14, 0x08, 0x08, 0xA4, 0x94, 0x1C}; // Digital Slave
const uint8_t SLAVE2_MAC[] = {0x78, 0x21, 0x84, 0x9C, 0x98, 0x4C}; // PZEM Slave

const int SWITCH_PINS[6] = {33, 32, 26, 27, 14, 13};

// SD SPI Pins
#define SD_CS   5
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCK  18
#define SD_LOG_DIR "/SmartNestLogs"
#define SD_ENERGY_LOG_FILE "energy_log.csv"
#define SD_SYNC_STATE_FILE "sync_state.txt"
#define SD_HISTORY_MAX_BATCH 10

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
    bool     sensorHealthy;  // true if PZEM sensor returned valid (non-NaN) readings
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
    uint32_t nextRecordId;
    uint32_t lastAckedRecordId;
    bool     cardPresent;
};

struct EnergyMeterState {
    double mainEnergyWh;
    double digitalEnergyWh;
    uint32_t relayRuntimeSec[7];
};

struct SystemData {
    DigitalBoardState digital;
    PzemBoardState    pzem;
    TimeState         time;
    SdCardState       sd;
    EnergyMeterState  energy;
    float smartNestAcsCurrentA; // SmartNest local six-relay ACS current
    uint8_t smartNestRelayMask;
    bool  cloudOnline;
    bool  masterLock;          // Global lock — all relays OFF, ignores commands
    bool  desiredDigitalRelay;
    bool  desiredDigitalLocked;
    // SD log access flags (set by uartTask on Core 0, consumed by sdLoggingTask on Core 1)
    bool  filesRequest;
    bool  readRequest;
    char  readFilename[64];
    int   readChunk;
    bool  factoryResetRequest;
    bool  clearLogsRequest;
    bool  historyRequest;
    uint32_t historyAfterId;
    uint8_t historyLimit;
    bool  historyAckRequest;
    uint32_t historyAckLastId;
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
    char     json[2048];
};
QueueHandle_t g_uartTxQueue = NULL;

struct EnergyLogSample {
    uint32_t recordId;
    uint32_t epoch;
    int8_t   tzHours;
    uint8_t  tzMins;
    float voltage;
    float mainCurrent;
    float mainPowerW;
    double mainEnergyWh;
    float digitalCurrent;
    float digitalPowerW;
    double digitalEnergyWh;
    float acCurrent;
    float acPowerW;
    float acEnergyKWh;
    uint32_t relayRuntimeSec[7];
};
QueueHandle_t g_sdQueue = NULL;

struct SwitchEvent {
    uint8_t idx;
    bool    state;
};
QueueHandle_t g_switchQueue = NULL;

// Binary ESP-NOW Structs
struct __attribute__((packed)) CmdPacket {
    uint8_t type;
};

void logEspNowSendResult(const char* target, uint8_t type, esp_err_t result) {
    if (result == ESP_OK) {
        Serial.printf("[Master] ESP-NOW queued -> %s type=0x%02X\n", target, type);
    } else {
        Serial.printf("[Master] ESP-NOW send failed -> %s type=0x%02X err=%d\n",
                      target, type, (int)result);
    }
}

esp_err_t sendEspNowCommand(const uint8_t* mac, const char* target, uint8_t type, bool logOk) {
    CmdPacket cp;
    cp.type = type;
    esp_err_t result = esp_now_send(mac, (uint8_t*)&cp, sizeof(cp));
    if (logOk || result != ESP_OK) {
        logEspNowSendResult(target, type, result);
    }
    return result;
}

void sendDigitalCommand(uint8_t type) {
    sendEspNowCommand(SLAVE1_MAC, "d1", type, true);
}

void setDesiredDigitalRelay(bool relay) {
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        g_systemState.desiredDigitalRelay = relay;
        xSemaphoreGive(g_stateMutex);
    }

    sendDigitalCommand(relay ? 0x02 : 0x03);
    Serial.printf("[Master] Digital relay command forwarded: %s\n", relay ? "ON" : "OFF");
}

void setDesiredDigitalLock(bool locked) {
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        g_systemState.desiredDigitalLocked = locked;
        g_systemState.digital.locked = g_systemState.masterLock || locked;
        xSemaphoreGive(g_stateMutex);
    }

    sendDigitalCommand(locked ? 0x04 : 0x05);
    Serial.printf("[Master] Digital relay lock command forwarded: %s\n", locked ? "ON" : "OFF");
}

void setDesiredMasterLock(bool locked) {
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        g_systemState.masterLock = locked;
        g_systemState.digital.locked = locked || g_systemState.desiredDigitalLocked;
        xSemaphoreGive(g_stateMutex);
    }

    sendDigitalCommand(locked ? 0x08 : 0x09);
    Serial.printf("[Master] Master lock command forwarded: %s\n", locked ? "ON" : "OFF");
}

struct __attribute__((packed)) DigitalSlavePacket {
    uint8_t type;
    float   rmsCurrent;
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

void buildLogFilePath(char* out, size_t outSize, const char* filename) {
    snprintf(out, outSize, "%s/%s", SD_LOG_DIR, filename);
}

void buildEnergyLogPath(char* out, size_t outSize) {
    buildLogFilePath(out, outSize, SD_ENERGY_LOG_FILE);
}

void buildSyncStatePath(char* out, size_t outSize) {
    buildLogFilePath(out, outSize, SD_SYNC_STATE_FILE);
}

uint32_t readUintFile(const char* path, uint32_t fallback) {
    File f = SD.open(path, FILE_READ);
    if (!f) {
        return fallback;
    }
    String s = f.readStringUntil('\n');
    f.close();
    s.trim();
    if (s.length() == 0) {
        return fallback;
    }
    return (uint32_t)strtoul(s.c_str(), NULL, 10);
}

void writeUintFile(const char* path, uint32_t value) {
    SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    if (f) {
        f.printf("%u\n", value);
        f.close();
    }
}

uint32_t scanMaxEnergyRecordId(const char* path) {
    File f = SD.open(path, FILE_READ);
    if (!f) {
        return 0;
    }
    uint32_t maxId = 0;
    bool header = true;
    char line[256];
    size_t idx = 0;
    while (f.available()) {
        char c = f.read();
        if (c == '\n') {
            line[idx] = '\0';
            if (!header && idx > 0) {
                uint32_t id = (uint32_t)strtoul(line, NULL, 10);
                if (id > maxId) {
                    maxId = id;
                }
            }
            header = false;
            idx = 0;
        } else if (c != '\r' && idx < sizeof(line) - 1) {
            line[idx++] = c;
        }
    }
    if (!header && idx > 0) {
        line[idx] = '\0';
        uint32_t id = (uint32_t)strtoul(line, NULL, 10);
        if (id > maxId) {
            maxId = id;
        }
    }
    f.close();
    return maxId;
}

void sendHistoryResponse(uint32_t afterId, uint8_t limit) {
    char energyLogPath[96];
    buildEnergyLogPath(energyLogPath, sizeof(energyLogPath));
    File f = SD.open(energyLogPath, FILE_READ);
    if (!f) {
        enqueueUartTx("{\"t\":\"hist_res\",\"records\":[],\"last\":0}");
        return;
    }

    uint32_t ackedId = 0;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        ackedId = g_systemState.sd.lastAckedRecordId;
        xSemaphoreGive(g_stateMutex);
    }
    uint32_t cursor = afterId > ackedId ? afterId : ackedId;
    if (limit == 0 || limit > SD_HISTORY_MAX_BATCH) {
        limit = SD_HISTORY_MAX_BATCH;
    }

    static char json[2048];
    int written = snprintf(json, sizeof(json), "{\"t\":\"hist_res\",\"records\":[");
    bool first = true;
    bool header = true;
    uint8_t count = 0;
    uint32_t lastId = cursor;
    static char line[320];
    size_t idx = 0;

    while (f.available() && count < limit) {
        char c = f.read();
        if (c == '\n') {
            line[idx] = '\0';
            if (!header && idx > 0) {
                static char work[320];
                strncpy(work, line, sizeof(work) - 1);
                work[sizeof(work) - 1] = '\0';
                char* fields[20] = {0};
                int fieldCount = 0;
                char* tok = strtok(work, ",");
                while (tok && fieldCount < 20) {
                    fields[fieldCount++] = tok;
                    tok = strtok(NULL, ",");
                }
                if (fieldCount >= 20) {
                    uint32_t id = (uint32_t)strtoul(fields[0], NULL, 10);
                    if (id > cursor) {
                        int n = snprintf(
                            json + written, sizeof(json) - written,
                            "%s{\"id\":%u,\"epoch\":%lu,\"date\":\"%s\",\"voltage\":%s,"
                            "\"main_current\":%s,\"main_power_w\":%s,\"main_energy_kwh\":%s,"
                            "\"digital_current\":%s,\"digital_power_w\":%s,\"digital_energy_kwh\":%s,"
                            "\"ac_current\":%s,\"ac_power_w\":%s,\"ac_energy_kwh\":%s,"
                            "\"runtimes\":[%s,%s,%s,%s,%s,%s,%s]}",
                            first ? "" : ",", id, strtoul(fields[1], NULL, 10), fields[2],
                            fields[3], fields[4], fields[5], fields[6], fields[7],
                            fields[8], fields[9], fields[10], fields[11], fields[12],
                            fields[13], fields[14], fields[15], fields[16], fields[17],
                            fields[18], fields[19]);
                        if (n > 0 && written + n < (int)sizeof(json) - 32) {
                            written += n;
                            first = false;
                            count++;
                            lastId = id;
                        } else {
                            break;
                        }
                    }
                }
            }
            header = false;
            idx = 0;
        } else if (c != '\r' && idx < sizeof(line) - 1) {
            line[idx++] = c;
        }
    }
    f.close();
    snprintf(json + written, sizeof(json) - written, "],\"last\":%u}", lastId);
    enqueueUartTx(json);
}

void formatLocalDateTime(uint32_t epoch, int8_t tzH, uint8_t tzM, char* out, size_t outSize) {
    uint32_t localEpoch = epoch + tzH * 3600 + tzM * 60;
    int y, m, d, hh, mm, ss;
    epochToDateTime(localEpoch, y, m, d, hh, mm, ss);
    snprintf(out, outSize, "%04d-%02d-%02d %02d:%02d:%02d", y, m, d, hh, mm, ss);
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
    UartTxItem* item = (UartTxItem*)pvPortMalloc(sizeof(UartTxItem));
    if (!item) {
        Serial.println("[Master] UART TX enqueue failed - out of heap");
        return;
    }
    strncpy(item->json, jsonStr, sizeof(item->json) - 1);
    item->json[sizeof(item->json) - 1] = '\0';
    xQueueSend(g_uartTxQueue, item, 0);
    vPortFree(item);
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
                uint8_t pktType = pkt.len >= 1 ? pkt.data[0] : 0;
                if ((pktType == 0x10 || pktType == 0x11) && pkt.len == sizeof(DigitalSlavePacket)) {
                    DigitalSlavePacket* p = (DigitalSlavePacket*)pkt.data;
                    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        bool wasOffline = !g_systemState.digital.online;
                        g_systemState.digital.rmsCurrent = p->rmsCurrent;
                        g_systemState.digital.relayState = p->relayState;
                        g_systemState.digital.switchState = p->switchState;
                        g_systemState.digital.locked = p->lockState != 0;
                        g_systemState.digital.rssi = pkt.rssi;
                        if (pktType == 0x11) {
                            g_systemState.digital.lastSeenTime = millis();
                            g_systemState.digital.online = true;
                        }
                        xSemaphoreGive(g_stateMutex);

                        if (pktType == 0x11 && wasOffline) {
                            char buf[64];
                            snprintf(buf, sizeof(buf), "{\"t\":\"on\",\"dev\":\"d1\"}");
                            enqueueUartTx(buf);
                        }
                    }
                } else if (pktType == 0x12 && pkt.len == sizeof(DigitalCmdAckPacket)) {
                    DigitalCmdAckPacket* p = (DigitalCmdAckPacket*)pkt.data;
                    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        g_systemState.digital.relayState = p->relayState;
                        g_systemState.digital.locked = (p->relayLockState != 0) || (p->masterLockState != 0);
                        g_systemState.masterLock = p->masterLockState != 0;
                        g_systemState.desiredDigitalLocked = p->relayLockState != 0;
                        g_systemState.digital.rssi = pkt.rssi;
                        g_systemState.digital.lastSeenTime = millis();
                        g_systemState.digital.online = true;
                        xSemaphoreGive(g_stateMutex);
                    }

                    char ackBuf[192];
                    snprintf(ackBuf, sizeof(ackBuf),
                             "{\"t\":\"cmd_ack\",\"tgt\":\"d1\",\"cmd_type\":%u,\"ok\":%s,\"reason\":%u,\"relay\":%u,\"relay_lock\":%u,\"master_lock\":%u,\"oc_lock\":%u}",
                             p->cmdType, p->accepted ? "true" : "false",
                             p->reason, p->relayState, p->relayLockState,
                             p->masterLockState, p->overcurrentLockState);
                    enqueueUartTx(ackBuf);
                    Serial.printf("[Master] Digital cmd_ack type=0x%02X ok=%s reason=%u\n",
                                  p->cmdType, p->accepted ? "YES" : "NO",
                                  p->reason);
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
                            g_systemState.pzem.sensorHealthy = !isnan(p->voltage) && !isnan(p->current) && !isnan(p->power);
                            g_systemState.pzem.rssi = pkt.rssi;
                            if (p->type == 0x21) {
                                g_systemState.pzem.lastSeenTime = millis();
                                g_systemState.pzem.online = true;
                            }
                            xSemaphoreGive(g_stateMutex);
                            
                            if (p->type == 0x21 && wasOffline) {
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
    static UartTxItem txItem;
    
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
                char* pRel = strstr(rxLine, "\"t\":\"rel\"");
                char* pCloud = strstr(rxLine, "\"t\":\"cloud\"");
                char* pLock = strstr(rxLine, "\"t\":\"lock\"");
                char* pFilesReq = strstr(rxLine, "\"t\":\"files_req\"");
                char* pReadReq = strstr(rxLine, "\"t\":\"read_req\"");
                char* pFactoryReset = strstr(rxLine, "\"t\":\"factory_reset\"");
                char* pClearLogs = strstr(rxLine, "\"t\":\"clear_logs\"");
                char* pHistReq = strstr(rxLine, "\"t\":\"hist_req\"");
                char* pHistAck = strstr(rxLine, "\"t\":\"hist_ack\"");
                
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
                            if (strcmp(cmd, "relay_on") == 0) {
                                setDesiredDigitalRelay(true);
                            }
                            else if (strcmp(cmd, "relay_off") == 0) {
                                setDesiredDigitalRelay(false);
                            }
                            else if (strcmp(cmd, "relay_lock") == 0) {
                                setDesiredDigitalLock(true);
                            }
                            else if (strcmp(cmd, "relay_unlock") == 0) {
                                setDesiredDigitalLock(false);
                            }
                            else if (strcmp(cmd, "reboot") == 0) {
                                targetMac = SLAVE1_MAC;
                                typeVal = 0x06;
                            }
                        } else if (isPzem) {
                            targetMac = SLAVE2_MAC;
                            if (strcmp(cmd, "reboot") == 0) typeVal = 0x06;
                            else if (strcmp(cmd, "energy_reset") == 0) typeVal = 0x07;
                        }
                        
                        if (typeVal != 0 && targetMac != NULL) {
                            sendEspNowCommand(targetMac, isD1 ? "d1" : "pzem", typeVal, true);
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
                            g_systemState.smartNestAcsCurrentA = iVal;
                            xSemaphoreGive(g_stateMutex);
                        }
                    }
                }
                else if (pRel) {
                    char* maskPtr = strstr(rxLine, "\"mask\":");
                    if (maskPtr) {
                        uint8_t maskVal = (uint8_t)strtoul(maskPtr + 7, NULL, 10);
                        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            g_systemState.smartNestRelayMask = maskVal & 0x3F;
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
                // Master Lock: {"t":"lock","val":true/false}
                else if (pLock) {
                    char* valPtr = strstr(rxLine, "\"val\":");
                    if (valPtr) {
                        bool lockVal = (strncmp(valPtr + 6, "true", 4) == 0);
                        Serial.printf("[LOCK] Lock command received from SmartNest: %s\n", lockVal ? "ON" : "OFF");
                        setDesiredMasterLock(lockVal);
                        Serial.printf("[LOCK] g_systemState.masterLock updated: %s\n", lockVal ? "ON" : "OFF");
                        
                        // Send lock_ack back to SmartNest
                        char ackBuf[64];
                        snprintf(ackBuf, sizeof(ackBuf), "{\"t\":\"lock_ack\",\"val\":%s}", lockVal ? "true" : "false");
                        enqueueUartTx(ackBuf);
                        Serial.println("[LOCK] lock_ack sent to SmartNest");
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
                // Clear SD Logs: {"t":"clear_logs"}
                else if (pClearLogs) {
                    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        g_systemState.clearLogsRequest = true;
                        xSemaphoreGive(g_stateMutex);
                    }
                    Serial.println("[Master] clear_logs received — queued for sdLoggingTask");
                }
                else if (pHistReq) {
                    char* afterPtr = strstr(rxLine, "\"after\":");
                    char* limitPtr = strstr(rxLine, "\"limit\":");
                    uint32_t afterVal = afterPtr ? (uint32_t)strtoul(afterPtr + 8, NULL, 10) : 0;
                    uint8_t limitVal = limitPtr ? (uint8_t)strtoul(limitPtr + 8, NULL, 10) : 6;
                    if (limitVal == 0 || limitVal > SD_HISTORY_MAX_BATCH) {
                        limitVal = SD_HISTORY_MAX_BATCH;
                    }
                    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        g_systemState.historyRequest = true;
                        g_systemState.historyAfterId = afterVal;
                        g_systemState.historyLimit = limitVal;
                        xSemaphoreGive(g_stateMutex);
                    }
                    Serial.printf("[Master] hist_req received after=%u limit=%u\n", afterVal, limitVal);
                }
                else if (pHistAck) {
                    char* lastPtr = strstr(rxLine, "\"last\":");
                    if (lastPtr) {
                        uint32_t lastVal = (uint32_t)strtoul(lastPtr + 7, NULL, 10);
                        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            g_systemState.historyAckRequest = true;
                            g_systemState.historyAckLastId = lastVal;
                            xSemaphoreGive(g_stateMutex);
                        }
                        Serial.printf("[Master] hist_ack received last=%u\n", lastVal);
                    }
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

    for (int i = 0; i < 6; i++) {
        bool active = (digitalRead(SWITCH_PINS[i]) == HIGH);
        pinStates[i] = active;
        lastDebouncedStates[i] = active;
        stableStartTimes[i] = millis();
    }
    
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
    uint32_t lastTxTime = 0;
    uint32_t lastCalcTime = millis();
    
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        uint32_t now = millis();
        float deltaSeconds = (now - lastCalcTime) / 1000.0f;
        if (deltaSeconds < 0.0f || deltaSeconds > 10.0f) {
            deltaSeconds = 1.0f;
        }
        lastCalcTime = now;
        float rmsCurrentVal = 0.0f;
        int rssiVal = 0;
        int pzemRssiVal = 0;
        uint8_t relayStateVal = 0;
        uint8_t switchStateVal = 0;
        bool lockedVal = false;
        bool digitalOnlineVal = false;
        bool pzemOnlineVal = false;
        bool pzemHealthyVal = false;
        float pzemCurrentVal = 0.0f;
        float pzemPowerVal = 0.0f;
        float pzemVoltageVal = 0.0f;
        float pzemEnergyVal = 0.0f;
        float smartNestLoadCurrent = 0.0f;
        bool masterLockVal = false;
        double mainEnergyWhVal = 0.0;
        double digitalEnergyWhVal = 0.0;
        uint32_t relayRuntimeVals[7] = {0};
        bool sdOkVal = false;
        uint64_t sdTotalVal = 0;
        uint64_t sdUsedVal = 0;
        bool gotMutex = false;
        
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            pzemVoltageVal = g_systemState.pzem.voltage;
            smartNestLoadCurrent = g_systemState.smartNestAcsCurrentA;
            
            pzemCurrentVal = g_systemState.pzem.current;
            pzemPowerVal = g_systemState.pzem.power;
            pzemEnergyVal = g_systemState.pzem.energy;
            rmsCurrentVal = g_systemState.digital.rmsCurrent;
            digitalOnlineVal = g_systemState.digital.online;
            pzemOnlineVal = g_systemState.pzem.online;
            pzemHealthyVal = g_systemState.pzem.sensorHealthy;
            rssiVal = g_systemState.digital.rssi;
            pzemRssiVal = g_systemState.pzem.rssi;
            relayStateVal = g_systemState.digital.relayState;
            switchStateVal = g_systemState.digital.switchState;
            lockedVal = g_systemState.digital.locked;
            masterLockVal = g_systemState.masterLock;
            sdOkVal = g_systemState.sd.cardPresent;
            sdTotalVal = g_systemState.sd.totalBytes;
            sdUsedVal = g_systemState.sd.usedBytes;

            float voltageForEnergy = (pzemOnlineVal && pzemHealthyVal && !isnan(pzemVoltageVal)) ? pzemVoltageVal : 0.0f;
            if (voltageForEnergy > 0.0f && deltaSeconds > 0.0f) {
                g_systemState.energy.mainEnergyWh += ((double)voltageForEnergy * (double)smartNestLoadCurrent * (double)deltaSeconds) / 3600.0;
                g_systemState.energy.digitalEnergyWh += ((double)voltageForEnergy * (double)rmsCurrentVal * (double)deltaSeconds) / 3600.0;
            }
            if (deltaSeconds > 0.0f) {
                uint32_t runtimeDelta = (uint32_t)(deltaSeconds + 0.5f);
                for (int i = 0; i < 6; i++) {
                    if (g_systemState.smartNestRelayMask & (1 << i)) {
                        g_systemState.energy.relayRuntimeSec[i] += runtimeDelta;
                    }
                }
                if (g_systemState.digital.relayState) {
                    g_systemState.energy.relayRuntimeSec[6] += runtimeDelta;
                }
            }
            mainEnergyWhVal = g_systemState.energy.mainEnergyWh;
            digitalEnergyWhVal = g_systemState.energy.digitalEnergyWh;
            for (int i = 0; i < 7; i++) {
                relayRuntimeVals[i] = g_systemState.energy.relayRuntimeSec[i];
            }
            
            gotMutex = true;
            xSemaphoreGive(g_stateMutex);
        }
        
        if (gotMutex) {
            if (now - lastTxTime >= 10000) {
                lastTxTime = now;
                
                bool isTimeValid = false;
                uint32_t curEpoch = 0;
                int8_t tzH = 5;
                uint8_t tzM = 30;
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    isTimeValid = g_systemState.time.timeValid;
                    if (isTimeValid) {
                        curEpoch = g_systemState.time.baseEpoch + (millis() - g_systemState.time.baseMillis) / 1000;
                        tzH = g_systemState.time.tzHours;
                        tzM = g_systemState.time.tzMins;
                    }
                    xSemaphoreGive(g_stateMutex);
                }
                
                if (isTimeValid && (smartNestLoadCurrent > 0.0f || rmsCurrentVal > 0.0f || mainEnergyWhVal > 0.0 || digitalEnergyWhVal > 0.0)) {
                    EnergyLogSample rec;
                    rec.epoch = curEpoch;
                    rec.tzHours = tzH;
                    rec.tzMins = tzM;
                    rec.voltage = pzemVoltageVal;
                    rec.mainCurrent = smartNestLoadCurrent;
                    rec.mainPowerW = pzemVoltageVal * smartNestLoadCurrent;
                    rec.mainEnergyWh = mainEnergyWhVal;
                    rec.digitalCurrent = rmsCurrentVal;
                    rec.digitalPowerW = pzemVoltageVal * rmsCurrentVal;
                    rec.digitalEnergyWh = digitalEnergyWhVal;
                    rec.acCurrent = pzemCurrentVal;
                    rec.acPowerW = pzemPowerVal;
                    rec.acEnergyKWh = pzemEnergyVal;
                    for (int i = 0; i < 7; i++) {
                        rec.relayRuntimeSec[i] = relayRuntimeVals[i];
                    }

                    if (xQueueSend(g_sdQueue, &rec, 0) != pdTRUE) {
                        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            g_systemState.sd.droppedRecords++;
                            xSemaphoreGive(g_stateMutex);
                        }
                        EnergyLogSample discard;
                        xQueueReceive(g_sdQueue, &discard, 0);
                        xQueueSend(g_sdQueue, &rec, 0);
                    }
                }
                
                char vStr[16] = "null";
                char piStr[16] = "null";
                char ppStr[16] = "null";
                char peStr[16] = "null";
                if (pzemOnlineVal && pzemHealthyVal && !isnan(pzemVoltageVal) && !isnan(pzemCurrentVal) && !isnan(pzemPowerVal) && !isnan(pzemEnergyVal)) {
                    snprintf(vStr, sizeof(vStr), "%.1f", pzemVoltageVal);
                    snprintf(piStr, sizeof(piStr), "%.3f", pzemCurrentVal);
                    snprintf(ppStr, sizeof(ppStr), "%.1f", pzemPowerVal);
                    snprintf(peStr, sizeof(peStr), "%.3f", pzemEnergyVal);
                }
                
                char tel[768];
                snprintf(tel, sizeof(tel),
                         "{\"t\":\"tel\",\"acs\":%.2f,\"v\":%s,\"pi\":%s,\"pp\":%s,\"pe\":%s,"
                         "\"me\":%.3f,\"de\":%.3f,"
                         "\"rt\":[%u,%u,%u,%u,%u,%u,%u],"
                         "\"d_on\":%s,\"p_on\":%s,\"p_health\":%s,\"d_rssi\":%d,\"p_rssi\":%d,"
                         "\"d_relay\":%d,\"d_sw\":%d,\"d_lock\":%s,"
                         "\"sd_ok\":%s,\"sd_total\":%llu,\"sd_used\":%llu,"
                         "\"m_lock\":%s}",
                         rmsCurrentVal, vStr, piStr, ppStr, peStr,
                         mainEnergyWhVal / 1000.0, digitalEnergyWhVal / 1000.0,
                         relayRuntimeVals[0], relayRuntimeVals[1], relayRuntimeVals[2],
                         relayRuntimeVals[3], relayRuntimeVals[4], relayRuntimeVals[5],
                         relayRuntimeVals[6],
                         digitalOnlineVal ? "true" : "false", pzemOnlineVal ? "true" : "false",
                         pzemHealthyVal ? "true" : "false",
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
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    bool sdOk = SD.begin(SD_CS);
    
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        g_systemState.sd.cardPresent = sdOk;
        g_systemState.sd.droppedRecords = 0;
        xSemaphoreGive(g_stateMutex);
    }
    
    if (sdOk) {
        SD.mkdir(SD_LOG_DIR);
        char energyLogPath[96];
        buildEnergyLogPath(energyLogPath, sizeof(energyLogPath));

        File logsDir = SD.open(SD_LOG_DIR);
        if (logsDir && logsDir.isDirectory()) {
            File entry = logsDir.openNextFile();
            while (entry) {
                const char* name = entry.name();
                if (!entry.isDirectory() && name &&
                    strcmp(name, SD_ENERGY_LOG_FILE) != 0 &&
                    strcmp(name, SD_SYNC_STATE_FILE) != 0) {
                    char stalePath[96];
                    buildLogFilePath(stalePath, sizeof(stalePath), name);
                    entry.close();
                    SD.remove(stalePath);
                    Serial.printf("[SD] Removed stale log file: %s\n", stalePath);
                } else {
                    entry.close();
                }
                entry = logsDir.openNextFile();
            }
            logsDir.close();
        }

        if (!SD.exists(energyLogPath)) {
            File newLog = SD.open(energyLogPath, FILE_WRITE);
            if (newLog) {
                newLog.println("record_id,epoch,date,voltage,main_current,main_power_w,main_energy_kwh,digital_current,digital_power_w,digital_energy_kwh,ac_current,ac_power_w,ac_energy_kwh,r1_on_s,r2_on_s,r3_on_s,r4_on_s,r5_on_s,r6_on_s,r7_on_s");
                newLog.close();
            }
        }
        char syncPath[96];
        buildSyncStatePath(syncPath, sizeof(syncPath));
        uint32_t maxId = scanMaxEnergyRecordId(energyLogPath);
        uint32_t ackedId = readUintFile(syncPath, 0);
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            g_systemState.sd.nextRecordId = maxId + 1;
            g_systemState.sd.lastAckedRecordId = ackedId;
            xSemaphoreGive(g_stateMutex);
        }
    }
    
    while (true) {
        EnergyLogSample rec;
        if (xQueueReceive(g_sdQueue, &rec, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (sdOk) {
                char energyLogPath[96];
                char dateBuf[24];
                buildEnergyLogPath(energyLogPath, sizeof(energyLogPath));
                formatLocalDateTime(rec.epoch, rec.tzHours, rec.tzMins, dateBuf, sizeof(dateBuf));

                File logFile = SD.open(energyLogPath, FILE_APPEND);
                if (!logFile) {
                    logFile = SD.open(energyLogPath, FILE_WRITE);
                    if (logFile) {
                        logFile.println("record_id,epoch,date,voltage,main_current,main_power_w,main_energy_kwh,digital_current,digital_power_w,digital_energy_kwh,ac_current,ac_power_w,ac_energy_kwh,r1_on_s,r2_on_s,r3_on_s,r4_on_s,r5_on_s,r6_on_s,r7_on_s");
                    }
                }

                if (logFile) {
                    uint32_t recId = 0;
                    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        recId = g_systemState.sd.nextRecordId++;
                        xSemaphoreGive(g_stateMutex);
                    }
                    if (recId == 0) {
                        recId = scanMaxEnergyRecordId(energyLogPath) + 1;
                    }
                    logFile.printf("%u,%u,%s,%.1f,%.3f,%.2f,%.6f,%.3f,%.2f,%.6f,%.3f,%.2f,%.6f,%u,%u,%u,%u,%u,%u,%u\n",
                                   recId, rec.epoch, dateBuf, rec.voltage,
                                   rec.mainCurrent, rec.mainPowerW, rec.mainEnergyWh / 1000.0,
                                   rec.digitalCurrent, rec.digitalPowerW, rec.digitalEnergyWh / 1000.0,
                                   rec.acCurrent, rec.acPowerW, rec.acEnergyKWh,
                                   rec.relayRuntimeSec[0], rec.relayRuntimeSec[1],
                                   rec.relayRuntimeSec[2], rec.relayRuntimeSec[3],
                                   rec.relayRuntimeSec[4], rec.relayRuntimeSec[5],
                                   rec.relayRuntimeSec[6]);
                    logFile.close();
                } else {
                    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        g_systemState.sd.droppedRecords++;
                        xSemaphoreGive(g_stateMutex);
                    }
                }
            }
        }

        bool wantFiles = false;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            wantFiles = g_systemState.filesRequest;
            if (wantFiles) g_systemState.filesRequest = false;
            xSemaphoreGive(g_stateMutex);
        }
        if (wantFiles && sdOk) {
            char energyLogPath[96];
            buildEnergyLogPath(energyLogPath, sizeof(energyLogPath));
            if (SD.exists(energyLogPath)) {
                enqueueUartTx("{\"t\":\"files_res\",\"files\":[\"energy_log.csv\"]}");
            } else {
                enqueueUartTx("{\"t\":\"files_res\",\"files\":[]}");
            }
        }

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
            char fullPath[96];
            buildLogFilePath(fullPath, sizeof(fullPath), readFname);
            if (strcmp(readFname, SD_ENERGY_LOG_FILE) != 0) {
                enqueueUartTx("{\"t\":\"read_res\",\"file\":\"energy_log.csv\",\"chunk\":0,\"total\":0,\"lines\":[]}");
            } else {
                File readFile = SD.open(fullPath, FILE_READ);
                if (readFile) {
                    int lineIndex = -1; // header is -1
                    int startLine = readChunkVal * 10;
                    int totalLines = 0;
                    static char line[256];
                    static char jsonBuf[2048];
                    line[0] = '\0';
                    int written = snprintf(jsonBuf, sizeof(jsonBuf),
                                           "{\"t\":\"read_res\",\"file\":\"%s\",\"chunk\":%d,\"lines\":[",
                                           SD_ENERGY_LOG_FILE, readChunkVal);
                    bool first = true;
                    size_t idx = 0;

                    while (readFile.available()) {
                        char c = readFile.read();
                        if (c == '\n') {
                            line[idx] = '\0';
                            if (lineIndex >= 0) {
                                if (lineIndex >= startLine && lineIndex < startLine + 10) {
                                    int n = snprintf(jsonBuf + written, sizeof(jsonBuf) - written,
                                                     "%s\"%s\"", first ? "" : ",", line);
                                    if (n > 0 && written + n < (int)sizeof(jsonBuf)) {
                                        written += n;
                                        first = false;
                                    }
                                }
                                totalLines++;
                            }
                            lineIndex++;
                            idx = 0;
                        } else if (c != '\r' && idx < sizeof(line) - 1) {
                            line[idx++] = c;
                        }
                    }
                    if (idx > 0 && lineIndex >= 0) {
                        line[idx] = '\0';
                        if (lineIndex >= startLine && lineIndex < startLine + 10) {
                            int n = snprintf(jsonBuf + written, sizeof(jsonBuf) - written,
                                             "%s\"%s\"", first ? "" : ",", line);
                            if (n > 0 && written + n < (int)sizeof(jsonBuf)) {
                                written += n;
                            }
                        }
                        totalLines++;
                    }
                    readFile.close();
                    snprintf(jsonBuf + written, sizeof(jsonBuf) - written,
                             "],\"total\":%d}", totalLines);
                    enqueueUartTx(jsonBuf);
                } else {
                    enqueueUartTx("{\"t\":\"read_res\",\"file\":\"energy_log.csv\",\"chunk\":0,\"total\":0,\"lines\":[]}");
                }
            }
        }

        bool wantHistory = false;
        uint32_t historyAfter = 0;
        uint8_t historyLimit = 0;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            wantHistory = g_systemState.historyRequest;
            if (wantHistory) {
                g_systemState.historyRequest = false;
                historyAfter = g_systemState.historyAfterId;
                historyLimit = g_systemState.historyLimit;
            }
            xSemaphoreGive(g_stateMutex);
        }
        if (wantHistory && sdOk) {
            sendHistoryResponse(historyAfter, historyLimit);
        }

        bool wantHistoryAck = false;
        uint32_t historyAckLast = 0;
        bool historyAckAdvanced = false;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            wantHistoryAck = g_systemState.historyAckRequest;
            if (wantHistoryAck) {
                g_systemState.historyAckRequest = false;
                historyAckLast = g_systemState.historyAckLastId;
                if (historyAckLast > g_systemState.sd.lastAckedRecordId) {
                    g_systemState.sd.lastAckedRecordId = historyAckLast;
                    historyAckAdvanced = true;
                }
            }
            xSemaphoreGive(g_stateMutex);
        }
        if (wantHistoryAck && historyAckAdvanced && sdOk) {
            char syncPath[96];
            buildSyncStatePath(syncPath, sizeof(syncPath));
            writeUintFile(syncPath, historyAckLast);
        }

        bool wantReset = false;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            wantReset = g_systemState.factoryResetRequest;
            if (wantReset) g_systemState.factoryResetRequest = false;
            xSemaphoreGive(g_stateMutex);
        }
        if (wantReset && sdOk) {
            Serial.println("[SD] FACTORY RESET — deleting /state.bin...");
            if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                memset(&g_systemState.energy, 0, sizeof(g_systemState.energy));
                g_systemState.sd.lastAckedRecordId = 0;
                g_systemState.sd.nextRecordId = 1;
                xSemaphoreGive(g_stateMutex);
            }
            char syncPath[96];
            buildSyncStatePath(syncPath, sizeof(syncPath));
            writeUintFile(syncPath, 0);
            Serial.println("[ENERGY] Main/digital energy counters and relay runtimes reset.");
        }

        bool wantClearLogs = false;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            wantClearLogs = g_systemState.clearLogsRequest;
            if (wantClearLogs) g_systemState.clearLogsRequest = false;
            xSemaphoreGive(g_stateMutex);
        }
        if (wantClearLogs && sdOk) {
            char energyLogPath[96];
            buildEnergyLogPath(energyLogPath, sizeof(energyLogPath));
            SD.remove(energyLogPath);
            File newLog = SD.open(energyLogPath, FILE_WRITE);
            if (newLog) {
                newLog.println("record_id,epoch,date,voltage,main_current,main_power_w,main_energy_kwh,digital_current,digital_power_w,digital_energy_kwh,ac_current,ac_power_w,ac_energy_kwh,r1_on_s,r2_on_s,r3_on_s,r4_on_s,r5_on_s,r6_on_s,r7_on_s");
                newLog.close();
            }
            char syncPath[96];
            buildSyncStatePath(syncPath, sizeof(syncPath));
            writeUintFile(syncPath, 0);
            if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                g_systemState.sd.lastAckedRecordId = 0;
                g_systemState.sd.nextRecordId = 1;
                xSemaphoreGive(g_stateMutex);
            }
            Serial.println("[SD] Energy log cleared.");
        }

        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (sdOk) {
                g_systemState.sd.totalBytes = SD.totalBytes();
                g_systemState.sd.usedBytes = SD.usedBytes();
            }
            xSemaphoreGive(g_stateMutex);
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
    g_sdQueue = xQueueCreate(30, sizeof(EnergyLogSample));
    g_switchQueue = xQueueCreate(10, sizeof(SwitchEvent));
    
    if (!g_stateMutex || !g_espNowQueue || !g_uartTxQueue || !g_sdQueue || !g_switchQueue) {
        Serial.println("[ERROR] Failed to create FreeRTOS primitives");
        delay(1000);
        ESP.restart();
    }
    
    // Initialize system state
    memset(&g_systemState, 0, sizeof(g_systemState));
    
    // WiFi Station mode before ESP-NOW to prevent boot looping
    WiFi.mode(WIFI_STA);
    esp_err_t macResult = esp_wifi_set_mac(WIFI_IF_STA, MASTER_MAC);
    if (macResult != ESP_OK) {
        Serial.printf("[ERROR] Failed to set Master STA MAC err=%d\n", (int)macResult);
    }
    WiFi.disconnect();
    Serial.print("[WIFI] Master STA MAC: ");
    Serial.println(WiFi.macAddress());
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
    xTaskCreatePinnedToCore(sdLoggingTask, "sdLoggingTask", 12288, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(healthTask, "healthTask", 2048, NULL, 1, NULL, 1);
    
    Serial.println("[SYSTEM] Master Initialization Successful. Tasks started.");
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}
