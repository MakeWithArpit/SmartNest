#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <HardwareSerial.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <RTClib.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <math.h>
#include <ctype.h>

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
#define SD_ENERGY_TMP_FILE "energy_log.tmp"
#define SD_ENERGY_BAK_FILE "energy_log.bak"
#define SD_SYNC_STATE_FILE "sync_state.txt"
#define SD_ENERGY_STATE_FILE "energy_state.txt"
#define SD_ENERGY_STATE_TMP_FILE "energy_state.tmp"
#define SD_ENERGY_STATE_BAK_FILE "energy_state.bak"
#define SD_RECORD_ID_STATE_FILE "record_id_state.txt"
#define SD_RECORD_ID_TMP_FILE "record_id_state.tmp"
#define SD_RECORD_ID_BAK_FILE "record_id_state.bak"
#define SD_HISTORY_MAX_BATCH 10
#define ENERGY_DEFAULT_VOLTAGE 220.0f
#define UART_TX_JSON_BYTES 4096
#define SD_FILE_VIEW_LIMIT_BYTES 1024
// Time source freshness affects reporting only. Time remains valid for daily
// reset after this age and is reported as SOFT while baseEpoch+millis is used.
#define TIME_SOURCE_FRESH_MS 30000UL

const char ENERGY_LOG_HEADER[] =
    "record_id,epoch,date,voltage,main_current,main_power_w,main_energy_kwh,"
    "digital_current,digital_power_w,digital_energy_kwh,ac_current,ac_power_w,"
    "ac_energy_kwh,r1_on_s,r2_on_s,r3_on_s,r4_on_s,r5_on_s,r6_on_s,r7_on_s,"
    "time_source";

RTC_DS3231 g_rtc;
bool       g_rtcReady = false;
bool       g_sdReady = false;

// Hardware Structures & Centralized State
struct DigitalBoardState {
    float    rmsCurrent;
    uint8_t  relayState;   // 0 or 1
    uint8_t  switchState;  // 0 or 1
    bool     locked;       // Optimistic local lock flag
    int      rssi;
    uint32_t lastSeenTime;
    bool     online;
    uint32_t missedHeartbeats;
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
    uint32_t missedHeartbeats;
};

struct TimeState {
    uint32_t baseEpoch;
    uint32_t baseMillis;
    uint32_t sourceSetMillis;
    int8_t   tzHours;
    uint8_t  tzMins;
    bool     timeValid;
    bool     trustedBase;
    char     source[12];
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
    double acDayStartKWh;
    double acDailyEnergyKWh;
    double pzemRawEnergyKWh;
    uint32_t relayRuntimeSec[7];
    uint32_t dayKey;
    uint32_t lastEpoch;
    int8_t tzHours;
    uint8_t tzMins;
    bool dayKeyValid;
    bool acBaselineValid;
    bool pzemRawValid;
    bool loadedFromSd;
    char timeSource[12];
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
    bool  desiredDigitalRelay;
    bool  desiredDigitalLocked;
    // SD log access flags (set by uartTask on Core 0, consumed by sdLoggingTask on Core 1)
    bool  readRequest;
    char  readFilename[64];
    int   readChunk;
    bool  lastRecordRequest;
    bool  clearEnergyLogsRequest;

    bool  historyRequest;
    uint32_t historyAfterId;
    uint8_t historyLimit;
    bool  historyAckRequest;
    uint32_t historyAckLastId;
    bool  energyStateSaveRequest;
    char  resetReason[24];
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
    char     json[UART_TX_JSON_BYTES];
};
QueueHandle_t g_uartTxQueue = NULL;
SemaphoreHandle_t g_uartTxMutex = NULL;

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
    char timeSource[12];
    bool finalDayRow;
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
        g_systemState.digital.locked = locked;
        xSemaphoreGive(g_stateMutex);
    }

    sendDigitalCommand(locked ? 0x04 : 0x05);
    Serial.printf("[Master] Digital relay lock command forwarded: %s\n", locked ? "ON" : "OFF");
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
    uint8_t healthy;
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

void buildEnergyStatePath(char* out, size_t outSize) {
    buildLogFilePath(out, outSize, SD_ENERGY_STATE_FILE);
}

void buildEnergyStateTmpPath(char* out, size_t outSize) {
    buildLogFilePath(out, outSize, SD_ENERGY_STATE_TMP_FILE);
}

void buildEnergyStateBakPath(char* out, size_t outSize) {
    buildLogFilePath(out, outSize, SD_ENERGY_STATE_BAK_FILE);
}

void buildRecordIdStatePath(char* out, size_t outSize) {
    buildLogFilePath(out, outSize, SD_RECORD_ID_STATE_FILE);
}

void buildRecordIdTmpPath(char* out, size_t outSize) {
    buildLogFilePath(out, outSize, SD_RECORD_ID_TMP_FILE);
}

void buildRecordIdBakPath(char* out, size_t outSize) {
    buildLogFilePath(out, outSize, SD_RECORD_ID_BAK_FILE);
}

bool isValidEpoch(uint32_t epoch) {
    return epoch >= 1704067200UL && epoch <= 4102444800UL; // 2024-01-01..2100-01-01
}

bool isValidRtcDateTime(const DateTime &dt) {
    uint16_t y = dt.year();
    uint32_t epoch = dt.unixtime();
    return y >= 2024 && y <= 2099 && isValidEpoch(epoch);
}

void setTimeState(uint32_t epoch, int8_t tzH, uint8_t tzM, const char* source) {
    if (!isValidEpoch(epoch)) return;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        g_systemState.time.baseEpoch = epoch;
        g_systemState.time.baseMillis = millis();
        g_systemState.time.sourceSetMillis = millis();
        g_systemState.time.tzHours = tzH;
        g_systemState.time.tzMins = tzM;
        g_systemState.time.timeValid = true;
        g_systemState.time.trustedBase = strcmp(source, "ESTIMATED") != 0;
        strncpy(g_systemState.time.source, source, sizeof(g_systemState.time.source) - 1);
        g_systemState.time.source[sizeof(g_systemState.time.source) - 1] = '\0';
        xSemaphoreGive(g_stateMutex);
    }
}

void reportedTimeSource(const TimeState &time, char* out, size_t outSize) {
    if (!time.timeValid) {
        strncpy(out, "NONE", outSize - 1);
    } else if (!time.trustedBase || strcmp(time.source, "ESTIMATED") == 0) {
        strncpy(out, "ESTIMATED", outSize - 1);
    } else if ((strcmp(time.source, "NTP") == 0 || strcmp(time.source, "RTC") == 0) &&
               millis() - time.sourceSetMillis <= TIME_SOURCE_FRESH_MS) {
        strncpy(out, time.source, outSize - 1);
    } else {
        strncpy(out, "SOFT", outSize - 1);
    }
    out[outSize - 1] = '\0';
}

bool getCurrentTimeSnapshot(uint32_t &epoch, int8_t &tzH, uint8_t &tzM, char* source, size_t sourceSize) {
    bool ok = false;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (g_systemState.time.timeValid) {
            epoch = g_systemState.time.baseEpoch + (millis() - g_systemState.time.baseMillis) / 1000;
            tzH = g_systemState.time.tzHours;
            tzM = g_systemState.time.tzMins;
            reportedTimeSource(g_systemState.time, source, sourceSize);
            ok = isValidEpoch(epoch);
        }
        xSemaphoreGive(g_stateMutex);
    }
    return ok;
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

bool readUintFileIfValid(const char* path, uint32_t &valueOut) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    String s = f.readStringUntil('\n');
    f.close();
    s.trim();
    if (s.length() == 0) return false;
    for (size_t i = 0; i < s.length(); i++) {
        if (!isDigit(s[i])) return false;
    }
    valueOut = (uint32_t)strtoul(s.c_str(), NULL, 10);
    return true;
}

bool writeRecordIdStateAtomic(uint32_t value);

uint32_t recoverRecordIdStateFile() {
    char mainPath[96], tmpPath[96], bakPath[96];
    buildRecordIdStatePath(mainPath, sizeof(mainPath));
    buildRecordIdTmpPath(tmpPath, sizeof(tmpPath));
    buildRecordIdBakPath(bakPath, sizeof(bakPath));

    uint32_t mainVal = 0, tmpVal = 0, bakVal = 0;
    bool hasMain = readUintFileIfValid(mainPath, mainVal);
    bool hasTmp = readUintFileIfValid(tmpPath, tmpVal);
    bool hasBak = readUintFileIfValid(bakPath, bakVal);
    uint32_t best = 0;
    if (hasMain && mainVal > best) best = mainVal;
    if (hasTmp && tmpVal > best) best = tmpVal;
    if (hasBak && bakVal > best) best = bakVal;

    if (best > 0) {
        writeRecordIdStateAtomic(best);
    }
    SD.remove(tmpPath);
    SD.remove(bakPath);
    return best;
}

bool writeRecordIdStateAtomic(uint32_t value) {
    char mainPath[96], tmpPath[96], bakPath[96];
    buildRecordIdStatePath(mainPath, sizeof(mainPath));
    buildRecordIdTmpPath(tmpPath, sizeof(tmpPath));
    buildRecordIdBakPath(bakPath, sizeof(bakPath));

    uint32_t current = 0;
    bool hasCurrent = readUintFileIfValid(mainPath, current);
    if (hasCurrent && current > value) {
        value = current;
    }

    SD.remove(tmpPath);
    File f = SD.open(tmpPath, FILE_WRITE);
    if (!f) {
        Serial.println("[SD] record_id_state tmp write failed");
        return false;
    }
    f.printf("%u\n", value);
    f.close();

    SD.remove(bakPath);
    if (SD.exists(mainPath) && !SD.rename(mainPath, bakPath)) {
        SD.remove(tmpPath);
        Serial.println("[SD] record_id_state backup rename failed");
        return false;
    }
    if (!SD.rename(tmpPath, mainPath)) {
        if (SD.exists(bakPath)) SD.rename(bakPath, mainPath);
        SD.remove(tmpPath);
        Serial.println("[SD] record_id_state commit failed");
        return false;
    }
    SD.remove(bakPath);
    return true;
}

bool initSdCardStorage() {
    Serial.printf("[SD] Init pins CS=%d SCK=%d MISO=%d MOSI=%d\n",
                  SD_CS, SD_SCK, SD_MISO, SD_MOSI);
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    delay(100);

    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    Serial.println("[SD] Trying default SD.begin...");
    bool ok = SD.begin(SD_CS);
    if (!ok) {
        Serial.println("[SD] Default init failed, retrying at 1 MHz");
        SD.end();
        delay(100);
        SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
        ok = SD.begin(SD_CS, SPI, 1000000);
    }

    if (ok) {
        Serial.printf("[SD] OK type=%u total=%llu used=%llu\n",
                      SD.cardType(),
                      (unsigned long long)SD.totalBytes(),
                      (unsigned long long)SD.usedBytes());
    } else {
        Serial.println("[SD] ERROR: SD.begin failed after default and 1 MHz retry");
    }
    return ok;
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

bool readEnergyStateFile(const char* path, EnergyMeterState &stateOut) {
    File f = SD.open(path, FILE_READ);
    if (!f) {
        return false;
    }
    memset(&stateOut, 0, sizeof(stateOut));
    strncpy(stateOut.timeSource, "ESTIMATED", sizeof(stateOut.timeSource) - 1);
    bool sawUsefulField = false;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        int eq = line.indexOf('=');
        if (eq <= 0) {
            continue;
        }
        String key = line.substring(0, eq);
        String val = line.substring(eq + 1);
        if (key == "dayKey") {
            stateOut.dayKey = (uint32_t)strtoul(val.c_str(), NULL, 10);
            stateOut.dayKeyValid = stateOut.dayKey > 0;
            sawUsefulField = true;
        } else if (key == "mainEnergyWh") {
            stateOut.mainEnergyWh = strtod(val.c_str(), NULL);
            sawUsefulField = true;
        } else if (key == "digitalEnergyWh") {
            stateOut.digitalEnergyWh = strtod(val.c_str(), NULL);
            sawUsefulField = true;
        } else if (key == "acDayStartKWh") {
            stateOut.acDayStartKWh = strtod(val.c_str(), NULL);
            stateOut.acBaselineValid = true;
            sawUsefulField = true;
        } else if (key == "acDailyEnergyKWh") {
            stateOut.acDailyEnergyKWh = strtod(val.c_str(), NULL);
            if (stateOut.acDailyEnergyKWh < 0.0) stateOut.acDailyEnergyKWh = 0.0;
            sawUsefulField = true;
        } else if (key == "pzemRawEnergyKWh") {
            stateOut.pzemRawEnergyKWh = strtod(val.c_str(), NULL);
            stateOut.pzemRawValid = stateOut.pzemRawEnergyKWh >= 0.0;
            sawUsefulField = true;
        } else if (key == "lastEpoch") {
            stateOut.lastEpoch = (uint32_t)strtoul(val.c_str(), NULL, 10);
            sawUsefulField = true;
        } else if (key == "tzHours") {
            stateOut.tzHours = (int8_t)strtol(val.c_str(), NULL, 10);
        } else if (key == "tzMins") {
            stateOut.tzMins = (uint8_t)strtoul(val.c_str(), NULL, 10);
        } else if (key == "timeSource") {
            strncpy(stateOut.timeSource, val.c_str(), sizeof(stateOut.timeSource) - 1);
            stateOut.timeSource[sizeof(stateOut.timeSource) - 1] = '\0';
        } else if (key.startsWith("r")) {
            int idx = key.substring(1).toInt();
            if (idx >= 1 && idx <= 7) {
                stateOut.relayRuntimeSec[idx - 1] = (uint32_t)strtoul(val.c_str(), NULL, 10);
                sawUsefulField = true;
            }
        }
    }
    f.close();
    if (!stateOut.acBaselineValid && stateOut.pzemRawValid) {
        stateOut.acDayStartKWh = stateOut.pzemRawEnergyKWh - stateOut.acDailyEnergyKWh;
        if (stateOut.acDayStartKWh < 0.0) stateOut.acDayStartKWh = stateOut.pzemRawEnergyKWh;
        stateOut.acBaselineValid = true;
    }
    return sawUsefulField;
}

void writeEnergyStateFile(const char* path) {
    EnergyMeterState snap;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    snap = g_systemState.energy;
    xSemaphoreGive(g_stateMutex);

    char tmpPath[96];
    char bakPath[96];
    buildEnergyStateTmpPath(tmpPath, sizeof(tmpPath));
    buildEnergyStateBakPath(bakPath, sizeof(bakPath));
    SD.remove(tmpPath);
    File f = SD.open(tmpPath, FILE_WRITE);
    if (!f) {
        Serial.println("[SD] energy_state write failed");
        return;
    }
    f.printf("dayKey=%u\n", snap.dayKey);
    f.printf("mainEnergyWh=%.6f\n", snap.mainEnergyWh);
    f.printf("digitalEnergyWh=%.6f\n", snap.digitalEnergyWh);
    f.printf("acDayStartKWh=%.6f\n", snap.acDayStartKWh);
    f.printf("acDailyEnergyKWh=%.6f\n", snap.acDailyEnergyKWh);
    f.printf("pzemRawEnergyKWh=%.6f\n", snap.pzemRawEnergyKWh);
    f.printf("lastEpoch=%u\n", snap.lastEpoch);
    f.printf("tzHours=%d\n", snap.tzHours);
    f.printf("tzMins=%u\n", snap.tzMins);
    f.printf("timeSource=%s\n", snap.timeSource[0] ? snap.timeSource : "ESTIMATED");
    for (int i = 0; i < 7; i++) {
        f.printf("r%d=%u\n", i + 1, snap.relayRuntimeSec[i]);
    }
    f.close();
    SD.remove(bakPath);
    if (SD.exists(path) && !SD.rename(path, bakPath)) {
        SD.remove(tmpPath);
        Serial.println("[SD] energy_state backup rename failed");
        return;
    }
    if (!SD.rename(tmpPath, path)) {
        if (SD.exists(bakPath)) SD.rename(bakPath, path);
        SD.remove(tmpPath);
        Serial.println("[SD] energy_state commit failed");
        return;
    }
    SD.remove(bakPath);
}

const char* baseFileName(const char* name) {
    const char* slash = strrchr(name, '/');
    return slash ? slash + 1 : name;
}

bool isSafeLogFilename(const char* name) {
    if (!name || name[0] == '\0' || strlen(name) > 48) {
        return false;
    }
    if (strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) {
        return false;
    }
    for (const char* p = name; *p; p++) {
        bool ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.';
        if (!ok) return false;
    }
    return true;
}

const char* fileTypeForName(const char* name) {
    if (strcmp(name, SD_ENERGY_LOG_FILE) == 0) return "csv";
    if (strcmp(name, SD_SYNC_STATE_FILE) == 0) return "sync";
    if (strcmp(name, SD_ENERGY_STATE_FILE) == 0) return "state";
    if (strcmp(name, SD_ENERGY_BAK_FILE) == 0) return "backup";
    if (strcmp(name, SD_ENERGY_TMP_FILE) == 0) return "temp";
    const char* dot = strrchr(name, '.');
    if (dot && strcmp(dot, ".txt") == 0) return "text";
    return "unknown";
}

bool canViewFile(const char* name, uint32_t size) {
    if (!isSafeLogFilename(name)) return false;
    const char* dot = strrchr(name, '.');
    if (strcmp(name, SD_ENERGY_LOG_FILE) == 0) return false;
    if (strcmp(name, SD_SYNC_STATE_FILE) == 0 || strcmp(name, SD_ENERGY_STATE_FILE) == 0) {
        return size <= SD_FILE_VIEW_LIMIT_BYTES;
    }
    return dot && strcmp(dot, ".txt") == 0 && size <= SD_FILE_VIEW_LIMIT_BYTES;
}

bool canClearFile(const char* name) {
    if (!isSafeLogFilename(name)) return false;
    return strcmp(name, SD_ENERGY_LOG_FILE) == 0 ||
           strcmp(name, SD_SYNC_STATE_FILE) == 0 ||
           strcmp(name, SD_ENERGY_STATE_FILE) == 0;
}

void sanitizeFileNameForJson(const char* in, char* out, size_t outSize) {
    size_t j = 0;
    for (const char* p = in ? in : ""; *p && j < outSize - 1; p++) {
        char c = *p;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        out[j++] = ok ? c : '_';
    }
    out[j] = '\0';
}

void appendJsonEscaped(char* out, size_t outSize, int &written, const char* value) {
    if (written >= (int)outSize - 1) return;
    out[written++] = '"';
    for (const char* p = value ? value : ""; *p && written < (int)outSize - 3; p++) {
        char c = *p;
        if (c == '"' || c == '\\') {
            out[written++] = '\\';
            out[written++] = c;
        } else if (c == '\n') {
            out[written++] = '\\';
            out[written++] = 'n';
        } else if (c == '\r') {
            out[written++] = '\\';
            out[written++] = 'r';
        } else if ((uint8_t)c >= 32) {
            out[written++] = c;
        }
    }
    out[written++] = '"';
    out[written] = '\0';
}



void sendLastEnergyRecordSummaryResponse() {
    char path[96];
    buildEnergyLogPath(path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) {
        enqueueUartTx("{\"t\":\"last_record_res\",\"file\":\"energy_log.csv\",\"ok\":false,\"message\":\"energy_log.csv not found\"}");
        return;
    }
    static char lastRow[320];
    static char line[320];
    lastRow[0] = '\0';
    size_t idx = 0;
    bool firstLine = true;
    while (f.available()) {
        char c = f.read();
        if (c == '\n') {
            line[idx] = '\0';
            if (firstLine) {
                firstLine = false;
            } else if (idx > 0 && isdigit((unsigned char)line[0])) {
                strncpy(lastRow, line, sizeof(lastRow) - 1);
                lastRow[sizeof(lastRow) - 1] = '\0';
            }
            idx = 0;
        } else if (c != '\r' && idx < sizeof(line) - 1) {
            line[idx++] = c;
        }
    }
    if (idx > 0) {
        line[idx] = '\0';
        if (!firstLine && isdigit((unsigned char)line[0])) {
            strncpy(lastRow, line, sizeof(lastRow) - 1);
            lastRow[sizeof(lastRow) - 1] = '\0';
        }
    }
    f.close();
    
    if (lastRow[0] == '\0') {
        enqueueUartTx("{\"t\":\"last_record_res\",\"file\":\"energy_log.csv\",\"ok\":false,\"message\":\"energy_log.csv has no data rows\"}");
        return;
    }
    
    uint32_t recordId = 0;
    char dateStr[32] = {0};
    char* p1 = strchr(lastRow, ',');
    if (p1) {
        recordId = strtoul(lastRow, NULL, 10);
        char* p2 = strchr(p1 + 1, ',');
        if (p2) {
            char* p3 = strchr(p2 + 1, ',');
            if (p3) {
                size_t len = p3 - (p2 + 1);
                if (len > sizeof(dateStr) - 1) len = sizeof(dateStr) - 1;
                strncpy(dateStr, p2 + 1, len);
                dateStr[len] = '\0';
            } else {
                strncpy(dateStr, p2 + 1, sizeof(dateStr) - 1);
                dateStr[sizeof(dateStr) - 1] = '\0';
            }
        }
    }
    
    char json[384];
    snprintf(json, sizeof(json),
             "{\"t\":\"last_record_res\",\"file\":\"energy_log.csv\",\"ok\":true,\"record_id\":%u,\"date\":\"%s\"}",
             recordId, dateStr);
    enqueueUartTx(json);
}

void clearEnergyLogsSafely() {
    char energyLogPath[96];
    char tmpPath[96];
    buildEnergyLogPath(energyLogPath, sizeof(energyLogPath));
    buildLogFilePath(tmpPath, sizeof(tmpPath), SD_ENERGY_TMP_FILE);

    // 1. Reset g_sdQueue
    if (g_sdQueue) {
        xQueueReset(g_sdQueue);
    }

    // 2. Truncate / recreate energy_log.csv atomically
    SD.remove(tmpPath);
    File tmpFile = SD.open(tmpPath, FILE_WRITE);
    bool ok = false;
    const char* errorMsg = "";
    if (tmpFile) {
        tmpFile.println(ENERGY_LOG_HEADER);
        tmpFile.close();
        SD.remove(energyLogPath);
        if (SD.rename(tmpPath, energyLogPath)) {
            ok = true;
        } else {
            errorMsg = "Failed to rename temp log file";
        }
    } else {
        errorMsg = "Failed to create temp log file";
    }

    if (ok) {
        // 3. Reset record_id files
        char recordIdPath[96], recordIdTmp[96], recordIdBak[96];
        buildRecordIdStatePath(recordIdPath, sizeof(recordIdPath));
        buildRecordIdTmpPath(recordIdTmp, sizeof(recordIdTmp));
        buildRecordIdBakPath(recordIdBak, sizeof(recordIdBak));
        SD.remove(recordIdPath);
        SD.remove(recordIdTmp);
        SD.remove(recordIdBak);
        writeRecordIdStateAtomic(1);

        // 4. Reset sync file
        char syncPath[96];
        buildSyncStatePath(syncPath, sizeof(syncPath));
        SD.remove(syncPath);
        writeUintFile(syncPath, 0);

        // 5. Update g_systemState
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_systemState.sd.lastAckedRecordId = 0;
            g_systemState.sd.nextRecordId = 1;
            xSemaphoreGive(g_stateMutex);
        }

        enqueueUartTx("{\"t\":\"clear_energy_logs_res\",\"file\":\"energy_log.csv\",\"ok\":true,\"message\":\"energy logs cleared\"}");
        Serial.println("[SD] energy_log.csv cleared successfully");
    } else {
        char errBuf[128];
        snprintf(errBuf, sizeof(errBuf), "{\"t\":\"clear_energy_logs_res\",\"file\":\"energy_log.csv\",\"ok\":false,\"message\":\"%s\"}", errorMsg);
        enqueueUartTx(errBuf);
        Serial.printf("[SD] Clear energy logs failed: %s\n", errorMsg);
    }
}



uint32_t localDayKey(uint32_t epoch, int8_t tzH, uint8_t tzM) {
    uint32_t localEpoch = epoch + tzH * 3600 + tzM * 60;
    return localEpoch / 86400UL;
}

const char *resetReasonToString(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_EXT: return "EXT";
        case ESP_RST_SW: return "SW";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        default: return "UNKNOWN";
    }
}

bool purgeAckedEnergyRows(uint32_t lastAckedId) {
    char energyLogPath[96];
    char tmpPath[96];
    char bakPath[96];
    buildEnergyLogPath(energyLogPath, sizeof(energyLogPath));
    buildLogFilePath(tmpPath, sizeof(tmpPath), SD_ENERGY_TMP_FILE);
    buildLogFilePath(bakPath, sizeof(bakPath), SD_ENERGY_BAK_FILE);

    if (!SD.exists(energyLogPath)) {
        File fresh = SD.open(energyLogPath, FILE_WRITE);
        if (fresh) {
            fresh.println(ENERGY_LOG_HEADER);
            fresh.close();
            return true;
        }
        Serial.println("[SD] Purge skipped: cannot create energy log");
        return false;
    }

    SD.remove(tmpPath);
    File src = SD.open(energyLogPath, FILE_READ);
    File dst = SD.open(tmpPath, FILE_WRITE);
    if (!src || !dst) {
        if (src) src.close();
        if (dst) dst.close();
        SD.remove(tmpPath);
        Serial.println("[SD] Purge failed: open error");
        return false;
    }

    dst.println(ENERGY_LOG_HEADER);
    bool header = true;
    uint32_t kept = 0;
    uint32_t removed = 0;
    static char line[320];
    size_t idx = 0;

    while (src.available()) {
        char c = src.read();
        if (c == '\n') {
            line[idx] = '\0';
            if (!header && idx > 0) {
                uint32_t id = (uint32_t)strtoul(line, NULL, 10);
                if (id > lastAckedId) {
                    dst.println(line);
                    kept++;
                } else {
                    removed++;
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
        if (id > lastAckedId) {
            dst.println(line);
            kept++;
        } else {
            removed++;
        }
    }
    src.close();
    dst.close();

    SD.remove(bakPath);
    if (!SD.rename(energyLogPath, bakPath)) {
        SD.remove(tmpPath);
        Serial.println("[SD] Purge failed: backup rename error");
        return false;
    }
    if (!SD.rename(tmpPath, energyLogPath)) {
        SD.rename(bakPath, energyLogPath);
        SD.remove(tmpPath);
        Serial.println("[SD] Purge failed: temp rename error, old log restored");
        return false;
    }
    SD.remove(bakPath);
    Serial.printf("[SD] Purged acked rows <=%u removed=%u kept=%u\n",
                  lastAckedId, removed, kept);
    return true;
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
                char* fields[24] = {0};
                int fieldCount = 0;
                char* tok = strtok(work, ",");
                while (tok && fieldCount < 24) {
                    fields[fieldCount++] = tok;
                    tok = strtok(NULL, ",");
                }
                if (fieldCount >= 20) {
                    uint32_t id = (uint32_t)strtoul(fields[0], NULL, 10);
                    if (id > cursor) {
                        const char* timeSource = fieldCount >= 21 ? fields[20] : "";
                        int n = snprintf(
                            json + written, sizeof(json) - written,
                            "%s{\"id\":%u,\"epoch\":%lu,\"date\":\"%s\",\"voltage\":%s,"
                            "\"main_current\":%s,\"main_power_w\":%s,\"main_energy_kwh\":%s,"
                            "\"digital_current\":%s,\"digital_power_w\":%s,\"digital_energy_kwh\":%s,"
                            "\"ac_current\":%s,\"ac_power_w\":%s,\"ac_energy_kwh\":%s,"
                            "\"runtimes_sec\":[%s,%s,%s,%s,%s,%s,%s],\"time_source\":\"%s\"}",
                            first ? "" : ",", id, strtoul(fields[1], NULL, 10), fields[2],
                            fields[3], fields[4], fields[5], fields[6], fields[7],
                            fields[8], fields[9], fields[10], fields[11], fields[12],
                            fields[13], fields[14], fields[15], fields[16], fields[17],
                            fields[18], fields[19], timeSource);
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
    if (!g_uartTxQueue) {
        return;
    }
    if (g_uartTxMutex && xSemaphoreTake(g_uartTxMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        Serial.println("[Master] UART TX enqueue skipped - busy");
        return;
    }

    static UartTxItem item;
    strncpy(item.json, jsonStr, sizeof(item.json) - 1);
    item.json[sizeof(item.json) - 1] = '\0';
    BaseType_t ok = xQueueSend(g_uartTxQueue, &item, 0);

    if (g_uartTxMutex) {
        xSemaphoreGive(g_uartTxMutex);
    }
    if (ok != pdTRUE) {
        Serial.println("[Master] UART TX queue full - message dropped");
    }
}

// Core 0 Tasks

void espNowTask(void* pvParameters) {
    EspNowPacket pkt;
    uint32_t lastPingTime = millis();
    
    while (true) {
        // Run ACK ping checks every 5 seconds for both devices
        uint32_t now = millis();
        if (now - lastPingTime >= 5000) {
            lastPingTime = now;
            CmdPacket pingPkt;
            pingPkt.type = 0x01;
            
            esp_now_send(SLAVE1_MAC, (uint8_t*)&pingPkt, sizeof(pingPkt));
            if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                g_systemState.digital.missedHeartbeats++;
                xSemaphoreGive(g_stateMutex);
            }
            
            esp_now_send(SLAVE2_MAC, (uint8_t*)&pingPkt, sizeof(pingPkt));
            if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                g_systemState.pzem.missedHeartbeats++;
                xSemaphoreGive(g_stateMutex);
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
                        g_systemState.digital.rmsCurrent = p->rmsCurrent;
                        g_systemState.digital.relayState = p->relayState;
                        g_systemState.digital.switchState = p->switchState;
                        g_systemState.digital.locked = p->lockState != 0;
                        g_systemState.digital.rssi = pkt.rssi;
                        g_systemState.digital.lastSeenTime = millis();
                        g_systemState.digital.online = true;
                        g_systemState.digital.missedHeartbeats = 0;
                        xSemaphoreGive(g_stateMutex);
                    }
                } else if (pktType == 0x12 && pkt.len == sizeof(DigitalCmdAckPacket)) {
                    DigitalCmdAckPacket* p = (DigitalCmdAckPacket*)pkt.data;
                    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        g_systemState.digital.relayState = p->relayState;
                        g_systemState.digital.locked = p->relayLockState != 0;
                        g_systemState.desiredDigitalLocked = p->relayLockState != 0;
                        g_systemState.digital.rssi = pkt.rssi;
                        g_systemState.digital.lastSeenTime = millis();
                        g_systemState.digital.online = true;
                        g_systemState.digital.missedHeartbeats = 0;
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
                const int legacyPzemPacketSize = 1 + (int)(sizeof(float) * 4);
                if (pkt.len == sizeof(PzemSlavePacket) || pkt.len == legacyPzemPacketSize) {
                    PzemSlavePacket* p = (PzemSlavePacket*)pkt.data;
                    if (p->type == 0x20 || p->type == 0x21) {
                        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            g_systemState.pzem.voltage = p->voltage;
                            g_systemState.pzem.current = p->current;
                            g_systemState.pzem.power = p->power;
                            g_systemState.pzem.energy = p->energy;
                            bool packetHealthy = (pkt.len == sizeof(PzemSlavePacket)) ? (p->healthy != 0) : true;
                            bool voltageValid = !isnan(p->voltage) && p->voltage >= 80.0f && p->voltage <= 280.0f;
                            g_systemState.pzem.sensorHealthy =
                                packetHealthy && voltageValid && !isnan(p->current) && !isnan(p->power);
                            g_systemState.pzem.rssi = pkt.rssi;
                            g_systemState.pzem.lastSeenTime = millis();
                            g_systemState.pzem.online = true;
                            g_systemState.pzem.missedHeartbeats = 0;
                            xSemaphoreGive(g_stateMutex);
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
                char* pReadReq = strstr(rxLine, "\"t\":\"read_req\"");
                char* pLastRecordReq = strstr(rxLine, "\"t\":\"last_record_req\"");
                char* pClearEnergyLogsReq = strstr(rxLine, "\"t\":\"clear_energy_logs_req\"");
                char* pSystemRebootReq = strstr(rxLine, "\"t\":\"system_reboot_req\"");

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
                        if (!isValidEpoch(newEpoch)) {
                            Serial.printf("[NTP] Rejected invalid epoch=%u\n", newEpoch);
                            rxIndex = 0;
                            continue;
                        }
                        
                        uint32_t currentEpoch = 0;
                        bool hardStep = false;
                        bool gotMutex = false;
                        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            currentEpoch = g_systemState.time.baseEpoch + (millis() - g_systemState.time.baseMillis) / 1000;
                            uint32_t diff = (newEpoch > currentEpoch) ? (newEpoch - currentEpoch) : (currentEpoch - newEpoch);
                            hardStep = !g_systemState.time.timeValid || (diff >= 2);
                            
                            g_systemState.time.baseEpoch = newEpoch;
                            g_systemState.time.baseMillis = millis();
                            g_systemState.time.sourceSetMillis = millis();
                            g_systemState.time.tzHours = tzH;
                            g_systemState.time.tzMins = tzM;
                            g_systemState.time.timeValid = true;
                            g_systemState.time.trustedBase = true;
                            strncpy(g_systemState.time.source, "NTP", sizeof(g_systemState.time.source) - 1);
                            g_systemState.time.source[sizeof(g_systemState.time.source) - 1] = '\0';
                            gotMutex = true;
                            xSemaphoreGive(g_stateMutex);
                        }
                        
                        if (gotMutex) {
                            if (g_rtcReady) {
                                g_rtc.adjust(DateTime(newEpoch));
                            }
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
                // Legacy SmartNest master lock alias: apply to Digital Board relay lock only.
                else if (pLock) {
                    char* valPtr = strstr(rxLine, "\"val\":");
                    if (valPtr) {
                        bool lockVal = (strncmp(valPtr + 6, "true", 4) == 0);
                        Serial.printf("[LOCK] Legacy lock command received from SmartNest: %s\n", lockVal ? "ON" : "OFF");
                        setDesiredDigitalLock(lockVal);
                        
                        char ackBuf[64];
                        snprintf(ackBuf, sizeof(ackBuf), "{\"t\":\"lock_ack\",\"val\":%s}", lockVal ? "true" : "false");
                        enqueueUartTx(ackBuf);
                        Serial.println("[LOCK] legacy lock_ack sent to SmartNest");
                    }
                }
                else if (pLastRecordReq) {
                    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        g_systemState.lastRecordRequest = true;
                        xSemaphoreGive(g_stateMutex);
                    }
                    Serial.println("[Master] last_record_req received");
                }
                else if (pClearEnergyLogsReq) {
                    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        g_systemState.clearEnergyLogsRequest = true;
                        xSemaphoreGive(g_stateMutex);
                    }
                    Serial.println("[Master] clear_energy_logs_req received");
                }
                else if (pSystemRebootReq) {
                    enqueueUartTx("{\"t\":\"system_reboot_ack\",\"ok\":true}");
                    sendEspNowCommand(SLAVE1_MAC, "d1", 0x06, true);
                    sendEspNowCommand(SLAVE2_MAC, "pzem", 0x06, true);
                    Serial.println("[Master] system_reboot_req accepted");
                    delay(500);
                    ESP.restart();
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

bool queueEnergySample(const EnergyLogSample &sample, bool priority) {
    if (xQueueSend(g_sdQueue, &sample, 0) == pdTRUE) {
        return true;
    }
    if (!priority) {
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            g_systemState.sd.droppedRecords++;
            xSemaphoreGive(g_stateMutex);
        }
        EnergyLogSample discard;
        if (xQueueReceive(g_sdQueue, &discard, 0) == pdTRUE && !discard.finalDayRow) {
            return xQueueSend(g_sdQueue, &sample, 0) == pdTRUE;
        }
        if (discard.finalDayRow) {
            xQueueSendToFront(g_sdQueue, &discard, 0);
        }
        return false;
    }

    EnergyLogSample buffered[30];
    int count = 0;
    bool removedNonFinal = false;
    EnergyLogSample tmp;
    while (xQueueReceive(g_sdQueue, &tmp, 0) == pdTRUE && count < 30) {
        if (!removedNonFinal && !tmp.finalDayRow) {
            removedNonFinal = true;
            continue;
        }
        buffered[count++] = tmp;
    }
    for (int i = 0; i < count; i++) {
        xQueueSend(g_sdQueue, &buffered[i], 0);
    }
    if (!removedNonFinal) {
        Serial.println("[ENERGY] Day-end final row preserved; queue contains only final rows");
        return false;
    }
    bool ok = xQueueSend(g_sdQueue, &sample, 0) == pdTRUE;
    if (!ok) {
        Serial.println("[ENERGY] Day-end final row enqueue failed after making space");
    }
    return ok;
}

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
        float energyVoltageVal = 0.0f;
        float pzemEnergyVal = 0.0f;
        float smartNestLoadCurrent = 0.0f;
        double mainEnergyWhVal = 0.0;
        double digitalEnergyWhVal = 0.0;
        double acDailyEnergyKWhVal = 0.0;
        double pzemRawEnergyKWhVal = 0.0;
        double acDayStartKWhVal = 0.0;
        uint32_t relayRuntimeVals[7] = {0};
        bool sdOkVal = false;
        bool voltageEstimatedVal = false;
        bool isTimeValid = false;
        uint32_t curEpoch = 0;
        int8_t tzH = 5;
        uint8_t tzM = 30;
        bool queueDayEndSample = false;
        EnergyLogSample dayEndSample;
        memset(&dayEndSample, 0, sizeof(dayEndSample));
        uint64_t sdTotalVal = 0;
        uint64_t sdUsedVal = 0;
        char resetReasonVal[24] = "";
        char timeSourceVal[12] = "NONE";
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
            isTimeValid = g_systemState.time.timeValid;
            if (isTimeValid) {
                curEpoch = g_systemState.time.baseEpoch + (millis() - g_systemState.time.baseMillis) / 1000;
                tzH = g_systemState.time.tzHours;
                tzM = g_systemState.time.tzMins;
                reportedTimeSource(g_systemState.time, timeSourceVal, sizeof(timeSourceVal));
            } else if (g_systemState.energy.lastEpoch > 0) {
                strncpy(timeSourceVal, "ESTIMATED", sizeof(timeSourceVal) - 1);
            }
            rssiVal = g_systemState.digital.rssi;
            pzemRssiVal = g_systemState.pzem.rssi;
            relayStateVal = g_systemState.digital.relayState;
            switchStateVal = g_systemState.digital.switchState;
            lockedVal = g_systemState.digital.locked;
            sdOkVal = g_systemState.sd.cardPresent;
            sdTotalVal = g_systemState.sd.totalBytes;
            sdUsedVal = g_systemState.sd.usedBytes;
            strncpy(resetReasonVal, g_systemState.resetReason, sizeof(resetReasonVal) - 1);

            bool voltageValid = pzemOnlineVal && pzemHealthyVal && !isnan(pzemVoltageVal) &&
                                pzemVoltageVal >= 80.0f && pzemVoltageVal <= 280.0f;
            float voltageForEnergy = voltageValid ? pzemVoltageVal : ENERGY_DEFAULT_VOLTAGE;
            energyVoltageVal = voltageForEnergy;
            voltageEstimatedVal = !voltageValid;
            bool pzemRawValid = pzemOnlineVal && pzemHealthyVal && !isnan(pzemEnergyVal) && pzemEnergyVal >= 0.0f;
            if (pzemRawValid) {
                g_systemState.energy.pzemRawEnergyKWh = pzemEnergyVal;
                g_systemState.energy.pzemRawValid = true;
                if (!g_systemState.energy.acBaselineValid) {
                    g_systemState.energy.acDayStartKWh = pzemEnergyVal;
                    g_systemState.energy.acBaselineValid = true;
                }
            }
            if (g_systemState.energy.acBaselineValid && g_systemState.energy.pzemRawValid) {
                double delta = g_systemState.energy.pzemRawEnergyKWh - g_systemState.energy.acDayStartKWh;
                g_systemState.energy.acDailyEnergyKWh = delta > 0.0 ? delta : 0.0;
            }
            if (deltaSeconds > 0.0f) {
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
            if (isTimeValid) {
                uint32_t currentDayKey = localDayKey(curEpoch, tzH, tzM);
                if (!g_systemState.energy.dayKeyValid) {
                    if (g_systemState.energy.loadedFromSd && g_systemState.energy.dayKey == currentDayKey) {
                        Serial.printf("[ENERGY] Restored same-day SD state dayKey=%u\n", currentDayKey);
                    } else if (g_systemState.energy.loadedFromSd && g_systemState.energy.dayKey < currentDayKey) {
                        dayEndSample.epoch = isValidEpoch(g_systemState.energy.lastEpoch) ? g_systemState.energy.lastEpoch : (curEpoch > 0 ? curEpoch - 1 : curEpoch);
                        dayEndSample.tzHours = g_systemState.energy.tzHours ? g_systemState.energy.tzHours : tzH;
                        dayEndSample.tzMins = g_systemState.energy.tzMins ? g_systemState.energy.tzMins : tzM;
                        dayEndSample.voltage = pzemVoltageVal;
                        dayEndSample.mainCurrent = smartNestLoadCurrent;
                        dayEndSample.mainPowerW = voltageForEnergy * smartNestLoadCurrent;
                        dayEndSample.mainEnergyWh = g_systemState.energy.mainEnergyWh;
                        dayEndSample.digitalCurrent = rmsCurrentVal;
                        dayEndSample.digitalPowerW = voltageForEnergy * rmsCurrentVal;
                        dayEndSample.digitalEnergyWh = g_systemState.energy.digitalEnergyWh;
                        dayEndSample.acCurrent = pzemCurrentVal;
                        dayEndSample.acPowerW = pzemPowerVal;
                        dayEndSample.acEnergyKWh = g_systemState.energy.acDailyEnergyKWh;
                        for (int i = 0; i < 7; i++) {
                            dayEndSample.relayRuntimeSec[i] = g_systemState.energy.relayRuntimeSec[i];
                        }
                        strncpy(dayEndSample.timeSource, "ESTIMATED", sizeof(dayEndSample.timeSource) - 1);
                        dayEndSample.finalDayRow = true;
                        queueDayEndSample = true;
                        double lastRaw = g_systemState.energy.pzemRawEnergyKWh;
                        bool hadRaw = g_systemState.energy.pzemRawValid;
                        memset(&g_systemState.energy, 0, sizeof(g_systemState.energy));
                        g_systemState.energy.pzemRawEnergyKWh = pzemRawValid ? pzemEnergyVal : lastRaw;
                        g_systemState.energy.pzemRawValid = pzemRawValid || hadRaw;
                        g_systemState.energy.acDayStartKWh = g_systemState.energy.pzemRawEnergyKWh;
                        g_systemState.energy.acBaselineValid = g_systemState.energy.pzemRawValid;
                        Serial.printf("[ENERGY] Saved state was older day; starting fresh dayKey=%u\n", currentDayKey);
                    }
                    g_systemState.energy.dayKey = currentDayKey;
                    g_systemState.energy.dayKeyValid = true;
                    g_systemState.energyStateSaveRequest = true;
                } else if (currentDayKey != g_systemState.energy.dayKey) {
                    double lastRaw = g_systemState.energy.pzemRawEnergyKWh;
                    bool hadRaw = g_systemState.energy.pzemRawValid;
                    dayEndSample.epoch = curEpoch > 0 ? curEpoch - 1 : curEpoch;
                    dayEndSample.tzHours = tzH;
                    dayEndSample.tzMins = tzM;
                    dayEndSample.voltage = pzemVoltageVal;
                    dayEndSample.mainCurrent = smartNestLoadCurrent;
                    dayEndSample.mainPowerW = voltageForEnergy * smartNestLoadCurrent;
                    dayEndSample.mainEnergyWh = g_systemState.energy.mainEnergyWh;
                    dayEndSample.digitalCurrent = rmsCurrentVal;
                    dayEndSample.digitalPowerW = voltageForEnergy * rmsCurrentVal;
                    dayEndSample.digitalEnergyWh = g_systemState.energy.digitalEnergyWh;
                    dayEndSample.acCurrent = pzemCurrentVal;
                    dayEndSample.acPowerW = pzemPowerVal;
                    dayEndSample.acEnergyKWh = g_systemState.energy.acDailyEnergyKWh;
                    for (int i = 0; i < 7; i++) {
                        dayEndSample.relayRuntimeSec[i] = g_systemState.energy.relayRuntimeSec[i];
                    }
                    strncpy(dayEndSample.timeSource, timeSourceVal, sizeof(dayEndSample.timeSource) - 1);
                    dayEndSample.finalDayRow = true;
                    queueDayEndSample = true;
                    memset(&g_systemState.energy, 0, sizeof(g_systemState.energy));
                    g_systemState.energy.pzemRawEnergyKWh = pzemRawValid ? pzemEnergyVal : lastRaw;
                    g_systemState.energy.pzemRawValid = pzemRawValid || hadRaw;
                    g_systemState.energy.acDayStartKWh = g_systemState.energy.pzemRawEnergyKWh;
                    g_systemState.energy.acBaselineValid = g_systemState.energy.pzemRawValid;
                    g_systemState.energy.dayKey = currentDayKey;
                    g_systemState.energy.dayKeyValid = true;
                    g_systemState.energyStateSaveRequest = true;
                    Serial.printf("[ENERGY] Local day changed; daily counters reset oldDay=%u newDay=%u\n",
                                  dayEndSample.epoch, currentDayKey);
                }
            }
            if (isTimeValid) {
                g_systemState.energy.lastEpoch = curEpoch;
                g_systemState.energy.tzHours = tzH;
                g_systemState.energy.tzMins = tzM;
                strncpy(g_systemState.energy.timeSource, timeSourceVal, sizeof(g_systemState.energy.timeSource) - 1);
                g_systemState.energy.timeSource[sizeof(g_systemState.energy.timeSource) - 1] = '\0';
            }
            mainEnergyWhVal = g_systemState.energy.mainEnergyWh;
            digitalEnergyWhVal = g_systemState.energy.digitalEnergyWh;
            acDailyEnergyKWhVal = g_systemState.energy.acDailyEnergyKWh;
            pzemRawEnergyKWhVal = g_systemState.energy.pzemRawEnergyKWh;
            acDayStartKWhVal = g_systemState.energy.acDayStartKWh;
            for (int i = 0; i < 7; i++) {
                relayRuntimeVals[i] = g_systemState.energy.relayRuntimeSec[i];
            }
            
            gotMutex = true;
            xSemaphoreGive(g_stateMutex);
        }
        
        if (gotMutex) {
            if (queueDayEndSample) {
                if (!queueEnergySample(dayEndSample, true)) {
                    Serial.println("[ENERGY] Day-end SD final row could not be queued without dropping another final row");
                }
            }
            if (now - lastTxTime >= 10000) {
                lastTxTime = now;

                bool acUsage = pzemCurrentVal > 0.0f || pzemPowerVal > 0.0f || acDailyEnergyKWhVal > 0.0 ||
                               (pzemRawEnergyKWhVal > acDayStartKWhVal);
                if (isTimeValid && (smartNestLoadCurrent > 0.0f || rmsCurrentVal > 0.0f ||
                                    mainEnergyWhVal > 0.0 || digitalEnergyWhVal > 0.0 ||
                                    acUsage)) {
                    EnergyLogSample rec;
                    rec.epoch = curEpoch;
                    rec.tzHours = tzH;
                    rec.tzMins = tzM;
                    rec.voltage = pzemVoltageVal;
                    rec.mainCurrent = smartNestLoadCurrent;
                    rec.mainPowerW = energyVoltageVal * smartNestLoadCurrent;
                    rec.mainEnergyWh = mainEnergyWhVal;
                    rec.digitalCurrent = rmsCurrentVal;
                    rec.digitalPowerW = energyVoltageVal * rmsCurrentVal;
                    rec.digitalEnergyWh = digitalEnergyWhVal;
                    rec.acCurrent = pzemCurrentVal;
                    rec.acPowerW = pzemPowerVal;
                    rec.acEnergyKWh = acDailyEnergyKWhVal;
                    for (int i = 0; i < 7; i++) {
                        rec.relayRuntimeSec[i] = relayRuntimeVals[i];
                    }
                    strncpy(rec.timeSource, timeSourceVal, sizeof(rec.timeSource) - 1);
                    rec.finalDayRow = false;

                    queueEnergySample(rec, false);
                }
                
                char vStr[16] = "null";
                char piStr[16] = "null";
                char ppStr[16] = "null";
                char peStr[16] = "0.000";
                char peRawStr[16] = "null";
                char peStartStr[16] = "null";
                snprintf(peStr, sizeof(peStr), "%.3f", acDailyEnergyKWhVal);
                snprintf(peRawStr, sizeof(peRawStr), "%.3f", pzemRawEnergyKWhVal);
                snprintf(peStartStr, sizeof(peStartStr), "%.3f", acDayStartKWhVal);
                if (pzemOnlineVal && pzemHealthyVal && !isnan(pzemVoltageVal) &&
                    pzemVoltageVal >= 80.0f && pzemVoltageVal <= 280.0f &&
                    !isnan(pzemCurrentVal) && !isnan(pzemPowerVal) && !isnan(pzemEnergyVal)) {
                    snprintf(vStr, sizeof(vStr), "%.1f", pzemVoltageVal);
                    snprintf(piStr, sizeof(piStr), "%.3f", pzemCurrentVal);
                    snprintf(ppStr, sizeof(ppStr), "%.1f", pzemPowerVal);
                }
                
                static char tel[900];
                snprintf(tel, sizeof(tel),
                         "{\"t\":\"tel\",\"acs\":%.2f,\"v\":%s,\"ev\":%.1f,\"ve\":%s,\"pi\":%s,\"pp\":%s,\"pe\":%s,"
                         "\"pe_raw\":%s,\"pe_start\":%s,\"me\":%.3f,\"de\":%.3f,"
                         "\"rt\":[%u,%u,%u,%u,%u,%u,%u],"
                         "\"d_on\":%s,\"p_on\":%s,\"p_health\":%s,\"d_rssi\":%d,\"p_rssi\":%d,"
                         "\"d_relay\":%d,\"d_sw\":%d,\"d_lock\":%s,"
                         "\"sd_ok\":%s,\"sd_total\":%llu,\"sd_used\":%llu,"
                         "\"reset_reason\":\"%s\",\"tsrc\":\"%s\",\"m_lock\":false}",
                         rmsCurrentVal, vStr, energyVoltageVal, voltageEstimatedVal ? "true" : "false",
                         piStr, ppStr, peStr, peRawStr, peStartStr,
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
                         resetReasonVal, timeSourceVal);
                
                enqueueUartTx(tel);
            }
        }
    }
}

void sdLoggingTask(void* pvParameters) {
    bool sdOk = g_sdReady;
    uint32_t lastEnergyStateSaveMs = 0;
    
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        g_systemState.sd.cardPresent = sdOk;
        g_systemState.sd.droppedRecords = 0;
        xSemaphoreGive(g_stateMutex);
    }
    
    if (sdOk) {
        SD.mkdir(SD_LOG_DIR);
        char energyLogPath[96];
        char tmpPath[96];
        char bakPath[96];
        buildEnergyLogPath(energyLogPath, sizeof(energyLogPath));
        buildLogFilePath(tmpPath, sizeof(tmpPath), SD_ENERGY_TMP_FILE);
        buildLogFilePath(bakPath, sizeof(bakPath), SD_ENERGY_BAK_FILE);

        if (!SD.exists(energyLogPath) && SD.exists(bakPath)) {
            if (SD.rename(bakPath, energyLogPath)) {
                Serial.println("[SD] Recovered energy log from backup");
            }
        }
        if (SD.exists(energyLogPath) && SD.exists(bakPath)) {
            SD.remove(bakPath);
        }
        SD.remove(tmpPath);

        if (!SD.exists(energyLogPath)) {
            File newLog = SD.open(energyLogPath, FILE_WRITE);
            if (newLog) {
                newLog.println(ENERGY_LOG_HEADER);
                newLog.close();
            }
        }
        char syncPath[96];
        buildSyncStatePath(syncPath, sizeof(syncPath));
        char recordIdPath[96];
        buildRecordIdStatePath(recordIdPath, sizeof(recordIdPath));
        char energyStatePath[96];
        buildEnergyStatePath(energyStatePath, sizeof(energyStatePath));
        EnergyMeterState savedEnergy;
        bool hasSavedEnergy = readEnergyStateFile(energyStatePath, savedEnergy);
        uint32_t maxId = scanMaxEnergyRecordId(energyLogPath);
        uint32_t ackedId = readUintFile(syncPath, 0);
        uint32_t highWaterId = recoverRecordIdStateFile();
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            uint32_t nextBase = maxId;
            if (ackedId > nextBase) nextBase = ackedId;
            if (highWaterId > nextBase) nextBase = highWaterId;
            g_systemState.sd.nextRecordId = nextBase + 1;
            g_systemState.sd.lastAckedRecordId = ackedId;
            if (hasSavedEnergy) {
                g_systemState.energy = savedEnergy;
                g_systemState.energy.loadedFromSd = true;
                g_systemState.energy.dayKeyValid = false;
                if (!g_systemState.time.timeValid && isValidEpoch(savedEnergy.lastEpoch)) {
                    g_systemState.time.baseEpoch = savedEnergy.lastEpoch;
                    g_systemState.time.baseMillis = millis();
                    g_systemState.time.sourceSetMillis = millis();
                    g_systemState.time.tzHours = savedEnergy.tzHours;
                    g_systemState.time.tzMins = savedEnergy.tzMins;
                    g_systemState.time.timeValid = true;
                    g_systemState.time.trustedBase = false;
                    strncpy(g_systemState.time.source, "ESTIMATED", sizeof(g_systemState.time.source) - 1);
                    g_systemState.time.source[sizeof(g_systemState.time.source) - 1] = '\0';
                }
                Serial.printf("[SD] Loaded energy_state dayKey=%u mainWh=%.3f digitalWh=%.3f\n",
                              savedEnergy.dayKey, savedEnergy.mainEnergyWh,
                              savedEnergy.digitalEnergyWh);
            }
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
                        logFile.println(ENERGY_LOG_HEADER);
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
                    logFile.printf("%u,%u,%s,%.1f,%.3f,%.2f,%.6f,%.3f,%.2f,%.6f,%.3f,%.2f,%.6f,%u,%u,%u,%u,%u,%u,%u,%s\n",
                                   recId, rec.epoch, dateBuf, rec.voltage,
                                   rec.mainCurrent, rec.mainPowerW, rec.mainEnergyWh / 1000.0,
                                   rec.digitalCurrent, rec.digitalPowerW, rec.digitalEnergyWh / 1000.0,
                                   rec.acCurrent, rec.acPowerW, rec.acEnergyKWh,
                                   rec.relayRuntimeSec[0], rec.relayRuntimeSec[1],
                                   rec.relayRuntimeSec[2], rec.relayRuntimeSec[3],
                                   rec.relayRuntimeSec[4], rec.relayRuntimeSec[5],
                                   rec.relayRuntimeSec[6],
                                   rec.timeSource[0] ? rec.timeSource : "ESTIMATED");
                    logFile.close();
                    char recordIdPath[96];
                    buildRecordIdStatePath(recordIdPath, sizeof(recordIdPath));
                    writeRecordIdStateAtomic(recId);
                    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        g_systemState.energyStateSaveRequest = true;
                        xSemaphoreGive(g_stateMutex);
                    }
                } else {
                    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        g_systemState.sd.droppedRecords++;
                        xSemaphoreGive(g_stateMutex);
                    }
                }
            }
        }

        bool wantLastRecord = false;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            wantLastRecord = g_systemState.lastRecordRequest;
            if (wantLastRecord) g_systemState.lastRecordRequest = false;
            xSemaphoreGive(g_stateMutex);
        }
        if (wantLastRecord) {
            if (sdOk) {
                sendLastEnergyRecordSummaryResponse();
            } else {
                enqueueUartTx("{\"t\":\"last_record_res\",\"file\":\"energy_log.csv\",\"ok\":false,\"message\":\"SD card not ready\"}");
            }
        }

        bool wantClearLogs = false;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            wantClearLogs = g_systemState.clearEnergyLogsRequest;
            if (wantClearLogs) g_systemState.clearEnergyLogsRequest = false;
            xSemaphoreGive(g_stateMutex);
        }
        if (wantClearLogs) {
            if (sdOk) {
                clearEnergyLogsSafely();
            } else {
                enqueueUartTx("{\"t\":\"clear_energy_logs_res\",\"file\":\"energy_log.csv\",\"ok\":false,\"message\":\"SD card not ready\"}");
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
            if (purgeAckedEnergyRows(historyAckLast)) {
                char energyLogPath[96];
                buildEnergyLogPath(energyLogPath, sizeof(energyLogPath));
                uint32_t maxId = scanMaxEnergyRecordId(energyLogPath);
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    uint32_t minNext = (maxId > historyAckLast ? maxId : historyAckLast) + 1;
                    if (g_systemState.sd.nextRecordId < minNext) {
                        g_systemState.sd.nextRecordId = minNext;
                    }
                    xSemaphoreGive(g_stateMutex);
                }
            }
        }



        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (sdOk) {
                g_systemState.sd.totalBytes = SD.totalBytes();
                g_systemState.sd.usedBytes = SD.usedBytes();
            }
            xSemaphoreGive(g_stateMutex);
        }

        bool saveEnergyState = false;
        uint32_t nowMs = millis();
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (g_systemState.energyStateSaveRequest || nowMs - lastEnergyStateSaveMs >= 60000UL) {
                g_systemState.energyStateSaveRequest = false;
                saveEnergyState = true;
            }
            xSemaphoreGive(g_stateMutex);
        }
        if (saveEnergyState && sdOk) {
            char energyStatePath[96];
            buildEnergyStatePath(energyStatePath, sizeof(energyStatePath));
            writeEnergyStateFile(energyStatePath);
            lastEnergyStateSaveMs = nowMs;
        }
    }
}

void healthTask(void* pvParameters) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        
        uint32_t now = millis();
        
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (g_systemState.digital.online && (now - g_systemState.digital.lastSeenTime > 15000)) {
                g_systemState.digital.online = false;
            }
            xSemaphoreGive(g_stateMutex);
        }
        
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (g_systemState.pzem.online && (now - g_systemState.pzem.lastSeenTime > 15000)) {
                g_systemState.pzem.online = false;
            }
            xSemaphoreGive(g_stateMutex);
        }

        bool dOnline = false;
        int dRssi = 0;
        uint32_t dLastSeen = 0;
        uint32_t dMissed = 0;
        
        bool pOnline = false;
        int pRssi = 0;
        uint32_t pLastSeen = 0;
        uint32_t pMissed = 0;
        bool pHealthy = false;

        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            dOnline = g_systemState.digital.online;
            dRssi = g_systemState.digital.rssi;
            dLastSeen = g_systemState.digital.lastSeenTime;
            dMissed = g_systemState.digital.missedHeartbeats;

            pOnline = g_systemState.pzem.online;
            pRssi = g_systemState.pzem.rssi;
            pLastSeen = g_systemState.pzem.lastSeenTime;
            pMissed = g_systemState.pzem.missedHeartbeats;
            pHealthy = g_systemState.pzem.sensorHealthy;
            xSemaphoreGive(g_stateMutex);
        }

        uint32_t dAge = (dLastSeen > 0) ? (now - dLastSeen) / 1000 : now / 1000;
        const char* dReason = dOnline ? "packet_received" : ((dLastSeen > 0) ? "heartbeat_timeout" : "no_contact");

        uint32_t pAge = (pLastSeen > 0) ? (now - pLastSeen) / 1000 : now / 1000;
        const char* pReason = pOnline ? "packet_received" : ((pLastSeen > 0) ? "heartbeat_timeout" : "no_contact");

        char statusJson[384];
        snprintf(statusJson, sizeof(statusJson),
                 "{\"t\":\"slave_status\","
                 "\"digital_board\":{\"online\":%s,\"rssi\":%d,\"last_seen_age_sec\":%u,\"missed_heartbeats\":%u,\"reason\":\"%s\"},"
                 "\"pzem\":{\"online\":%s,\"rssi\":%d,\"last_seen_age_sec\":%u,\"missed_heartbeats\":%u,\"reason\":\"%s\",\"pzem_health\":%s}}",
                 dOnline ? "true" : "false", dRssi, dAge, dMissed, dReason,
                 pOnline ? "true" : "false", pRssi, pAge, pMissed, pReason, pHealthy ? "true" : "false");
        
        enqueueUartTx(statusJson);
    }
}

// Setup
void setup() {
    Serial.begin(115200);
    Serial.println("\n[SYSTEM] Master Initializing...");
    const char *resetReason = resetReasonToString(esp_reset_reason());
    Serial.printf("[RESET] Master reset_reason=%s\n", resetReason);
    
    // Create mutex & queues
    g_stateMutex = xSemaphoreCreateMutex();
    g_uartTxMutex = xSemaphoreCreateMutex();
    g_espNowQueue = xQueueCreate(10, sizeof(EspNowPacket));
    g_uartTxQueue = xQueueCreate(8, sizeof(UartTxItem));
    g_sdQueue = xQueueCreate(30, sizeof(EnergyLogSample));
    g_switchQueue = xQueueCreate(10, sizeof(SwitchEvent));
    
    if (!g_stateMutex || !g_uartTxMutex || !g_espNowQueue || !g_uartTxQueue || !g_sdQueue || !g_switchQueue) {
        Serial.println("[ERROR] Failed to create FreeRTOS primitives");
        delay(1000);
        ESP.restart();
    }
    
    // Initialize system state
    memset(&g_systemState, 0, sizeof(g_systemState));
    strncpy(g_systemState.resetReason, resetReason, sizeof(g_systemState.resetReason) - 1);
    g_systemState.time.tzHours = 5;
    g_systemState.time.tzMins = 30;
    strncpy(g_systemState.time.source, "NONE", sizeof(g_systemState.time.source) - 1);

    g_sdReady = initSdCardStorage();
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        g_systemState.sd.cardPresent = g_sdReady;
        g_systemState.sd.droppedRecords = 0;
        xSemaphoreGive(g_stateMutex);
    }

    Wire.begin();
    if (g_rtc.begin()) {
        DateTime rtcNow = g_rtc.now();
        if (!g_rtc.lostPower() && isValidRtcDateTime(rtcNow)) {
            g_rtcReady = true;
            setTimeState(rtcNow.unixtime(), 5, 30, "RTC");
            Serial.printf("[RTC] DS3231 valid epoch=%u\n", rtcNow.unixtime());
        } else {
            g_rtcReady = true;
            Serial.println("[RTC] DS3231 present but time invalid/lost power");
        }
    } else {
        Serial.println("[RTC] DS3231 not detected");
    }
    
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
    xTaskCreatePinnedToCore(energyTimeTask, "energyTimeTask", 8192, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(sdLoggingTask, "sdLoggingTask", 12288, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(healthTask, "healthTask", 2048, NULL, 1, NULL, 1);
    
    Serial.println("[SYSTEM] Master Initialization Successful. Tasks started.");
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}

