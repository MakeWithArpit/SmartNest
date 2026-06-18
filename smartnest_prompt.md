SYSTEM CONTEXT:
You are an expert embedded systems firmware engineer specializing in ESP32 Arduino
development with FreeRTOS, ESP-NOW, UART, MQTT, and AsyncWebServer. Generate
production-quality firmware for a 4-device IoT system called SmartNest.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PART 1: SYSTEM OVERVIEW & HARDWARE INVENTORY
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

The system has 4 ESP32 boards:

BOARD 1 — DIGITAL BOARD SLAVE
  MAC: {0x14, 0x08, 0x08, 0xA4, 0x94, 0x1C}
  Hardware:
    - 1× Relay               : GPIO 15 (Active HIGH, starts OFF with HIGH)
    - 1× Manual Switch       : GPIO 2  (INPUT_PULLUP, active LOW)
    - 1× ACS712-30A sensor   : GPIO 34 (analog, 66 mV/A sensitivity)
    - RGB LED                : R=GPIO5, G=GPIO18, B=GPIO19
  Role: Measure ACS712 RMS current locally, control its own local relay, send
        telemetry to Master via ESP-NOW binary protocol. RGB LED shows system
        state. Does NOT control the 6 main SmartNest relays.

BOARD 2 — PZEM SLAVE
  MAC: {0x78, 0x21, 0x84, 0x9C, 0x98, 0x4C}
  Hardware:
    - PZEM-004T V3.0         : Serial2, RX=GPIO16, TX=GPIO17
    - RGB LED                : R=GPIO5, G=GPIO18, B=GPIO19
  Role: Read PZEM-004T voltage/current/power/energy, send to Master via ESP-NOW
        binary protocol. Support energy register reset and remote reboot.

BOARD 3 — MASTER ESP32
  MAC: {0x88, 0x57, 0x21, 0xB1, 0xD3, 0x74}
  Hardware:
    - 6× Manual Switches     : GPIOs 13, 15, 19, 4, 32, 18  (INPUT_PULLDOWN,
                               active HIGH — these switched from SmartNest board)
    - UART to Internet ESP32 : TX=GPIO17 → SmartNest RX=GPIO16
                               RX=GPIO16 → SmartNest TX=GPIO17
                               Baud: 115200, 8N1
    - SD Card (SPI)          : CS=GPIO5, MOSI=GPIO23, MISO=GPIO19, SCK=GPIO18
                               (choose non-conflicting pins per your PCB wiring)
  Role:
    - Communicate with both slaves via ESP-NOW (binary protocol only, no JSON)
    - Poll 6 manual switches with debounce; send switch events to Internet ESP32
      via UART so it can control its 6 relays accordingly
    - Receive ACS712 current from Digital Slave, PZEM data from PZEM Slave
    - Receive the COMBINED ACS712 current (all 6 relays) from Internet ESP32
      via UART ("acs" message) — this combined value is what feeds the energy
      calculation below, NOT the Digital Slave's local ACS712 reading
    - Calculate apparent energy: Power(VA) = V_pzem × I_internetACS712
      (I_internetACS712 = combined current of all 6 relays, reported by the
      Internet ESP32's own ACS712 sensor. The Digital Slave's ACS712 current
      is only for that slave's own local relay and is reported separately in
      telemetry — it does NOT feed this energy calculation.)
    - Accumulate daily/monthly/lifetime energy in Wh
    - Log to SD card using date-based binary files
    - Persist energy counters to /state.bin (5-min interval + 0.02 kWh threshold
      + brownout trigger)
    - Run fully on FreeRTOS (no blocking in loop())
    - Send telemetry to Internet ESP32 via UART (JSON, newline-delimited)
    - Receive slave commands from Internet ESP32 via UART; forward via ESP-NOW
    - Receive NTP time sync from Internet ESP32 via UART every 5 minutes
    - Detect slave offline (30s timeout), report to Internet ESP32 via UART
    - No WiFi. No Serial monitor commands. No ArduinoJson on ESP-NOW path.

BOARD 4 — INTERNET ESP32 (SmartNest.ino)
  Hardware:
    - 6× Relays              : GPIO 26, 27, 14, 23, 25, 33  (Active HIGH)
    - 1× ACS712-30A sensor   : GPIO per existing ACS712_PIN define (wiring
                               unchanged) — measures the COMBINED current of
                               all 6 relay loads, i.e. the actual house load
    - UART to Master         : RX=GPIO16 ← Master TX
                               TX=GPIO17 → Master RX
                               Baud: 115200, 8N1
    - Reset Button           : GPIO 0 (hold 3s = WiFi credential wipe)
  Role:
    - Host local web dashboard at smart-nest.local over WebSocket (real-time)
    - WiFi provisioning (captive portal AP mode when not configured)
    - mDNS hostname: "smart-nest" → accessible as smart-nest.local
    - Serve 6 relay control with per-relay lock/unlock and master shutdown,
      PLUS a 7th "virtual" relay representing the Digital Board Slave's relay
      (reached via Master/ESP-NOW over UART, not a local GPIO)
    - Relay state is controlled by: dashboard OR switch events from Master via UART
    - Read its own ACS712 sensor (combined 6-relay current) and send it to
      Master via UART every 10 seconds as {"t":"acs","i":X.XX} — this is the
      value Master uses for energy calculation
    - Receive telemetry from Master via UART (energy, slave status, PZEM data,
      and the Digital Board's relay/switch/lock state)
    - Send slave commands to Master via UART when requested from dashboard
    - Connect to MQTT broker and publish telemetry; subscribe for remote commands
    - NTP sync every 5 minutes; send epoch to Master via UART
    - NO manual switches on this board (removed — they moved to Master)

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PART 2: BINARY ESP-NOW PROTOCOL SPECIFICATION
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

ALL ESP-NOW packets are binary structs with __attribute__((packed)). No JSON
on the ESP-NOW path. No ArduinoJson on master or slaves.

--- COMMAND PACKET (Master → Any Slave, 1 byte) ---
struct CmdPacket {
    uint8_t type;
};
Type IDs:
  0x01 = ACK ping (both slaves respond with their data packet)
  0x02 = Relay ON          (Digital Slave only)
  0x03 = Relay OFF         (Digital Slave only)
  0x04 = Relay Lock        (Digital Slave only — sets masterLock=true)
  0x05 = Relay Unlock      (Digital Slave only — sets masterLock=false)
  0x06 = Reboot            (both slaves)
  0x07 = Energy Reset      (PZEM Slave only)

NOTE: Types 0x02–0x05 are also used when the dashboard's 7th relay card
(Digital Board Slave) is toggled or locked from the Internet ESP32 — Master
simply forwards the existing UART "cmd" message
({"tgt":"d1","cmd":"relay_on"|"relay_off"|"relay_lock"|"relay_unlock"}) into
the matching CmdPacket type. No new ESP-NOW packet types are introduced for
the 7th-relay feature.

--- DIGITAL SLAVE DATA PACKET (Digital Slave → Master, 7 bytes) ---
struct DigitalSlavePacket {
    uint8_t type;        // 0x10 = periodic data, 0x11 = ACK reply
    float   rmsCurrent;  // ACS712 smoothed RMS current in Amperes (IEEE 754)
    uint8_t relayState;  // 0 = OFF, 1 = ON
    uint8_t switchState; // 0 = released, 1 = pressed (local switch on board)
};
Total: 7 bytes.
NOTE: this packet does NOT carry the masterLock state — Master tracks lock
state itself optimistically (see Part 5, FILE A, DigitalBoardState.locked).

--- PZEM SLAVE DATA PACKET (PZEM Slave → Master, 17 bytes) ---
struct PzemSlavePacket {
    uint8_t type;    // 0x20 = periodic data, 0x21 = ACK reply
    float   voltage; // AC Line Voltage in Volts
    float   current; // PZEM current in Amperes
    float   power;   // Active Power in Watts (from PZEM internal calc)
    float   energy;  // Cumulative Energy register in Wh
};
Total: 17 bytes.

MASTER VALIDATES packet type IDs before processing. Unknown type = silently drop.
Master sends ACK ping (0x01) every 10 seconds to each slave (staggered by 5s).
If master receives no packet from a slave for 30 seconds → slave marked offline.
On offline detection → master sends offline report to Internet ESP32 via UART.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PART 3: UART PROTOCOL — MASTER ↔ INTERNET ESP32
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Format: Compact JSON, '\n'-terminated, 115200 baud, 8N1.
Use ArduinoJson only on Internet ESP32 UART side for parsing.
Master uses manual JSON string building/parsing (no library) to keep it
lightweight — this applies to every message type below, including "acs".

--- MASTER → INTERNET ESP32 ---

(A) Telemetry packet — sent every 10 seconds:
{
  "t":"tel",
  "acs":1.23,          // Digital Slave's own ACS712 RMS current (A) — that
                       // slave's local relay only. NOT used for energy calc.
  "v":230.5,           // PZEM voltage (V)
  "pi":0.95,           // PZEM current (A)
  "pp":218.9,          // PZEM power (W)
  "d_wh":150.3,        // Daily energy (Wh)
  "m_wh":4210.7,       // Monthly energy (Wh)
  "l_wh":127345.2,     // Lifetime energy (Wh)
  "d_on":true,         // Digital Slave online flag
  "p_on":true,         // PZEM Slave online flag
  "d_rssi":-58,        // Digital Slave RSSI (dBm)
  "p_rssi":-62,        // PZEM Slave RSSI (dBm)
  "d_relay":1,         // Digital Slave relay state (0/1)
  "d_sw":0,            // Digital Slave physical switch state (0/1)
  "d_lock":false       // Digital Slave masterLock state (Master-tracked)
}

(B) Switch event — sent ONLY on state change, immediately:
{
  "t":"sw",
  "idx":2,             // Switch/Relay index (0–5)
  "s":1                // 1=pressed/ON, 0=released/OFF
}

(C) Offline alert — sent immediately when slave goes offline:
{
  "t":"off",
  "dev":"d1"           // "d1" = Digital Slave, "pzem" = PZEM Slave
}
(D) Slave back online:
{
  "t":"on",
  "dev":"d1"
}

(E) History backfill batch — sent only when cloudOnline==true and backlog
    exists; max 10 records per message:
{
  "t":"hist",
  "batch":12,
  "recs":[
    {"epoch":1734567880,"v":230.1,"load":0.95,"pi":0.93,"powerVA":214.0},
    {"epoch":1734567890,"v":230.4,"load":0.97,"pi":0.95,"powerVA":218.9}
  ]
}

--- INTERNET ESP32 → MASTER ---

(A) Slave command (from dashboard or MQTT cloud):
{
  "t":"cmd",
  "tgt":"d1",          // "d1" or "pzem"
  "cmd":"relay_on"     // relay_on, relay_off, relay_lock, relay_unlock, reboot,
                       // energy_reset (energy_reset only for pzem target)
}

(B) NTP time sync (sent every 5 minutes):
{
  "t":"ntp",
  "epoch":1734567890,  // Unix epoch (UTC)
  "tz_h":5,            // Timezone offset hours (IST = 5)
  "tz_m":30            // Timezone offset minutes (IST = 30)
}

(C) Combined ACS712 current — sent every 10 seconds, synced with the
    Internet ESP32's own telemetry cycle:
{
  "t":"acs",
  "i":2.35             // Internet ESP32's own ACS712 current (A) — combined
                       // current of all 6 relays. This is the value Master
                       // uses for energy calculation (V_pzem × this value).
}

(D) Cloud connectivity transition — sent immediately on MQTT connect/disconnect:
{
  "t":"cloud",
  "up":true            // true = MQTT connected (reconnect-success block mein);
                       // false = MQTT disconnected (taaki Master turant
                       // backfill rok de)
}

(E) History batch acknowledgement — sent after all records in a "hist" batch
    are successfully published to MQTT:
{
  "t":"hist_ack",
  "batch":12,          // batch number echoed from the "hist" message
  "upto":1734567890    // epoch of the last record that was safely published
}

Master time computation on-demand:
  currentEpoch = baseEpoch + (millis() - baseMillis) / 1000
  (unsigned subtraction handles millis() rollover correctly)
  If new sync differs < 2s from computed → smooth adjust.
  If difference >= 2s → hard step to new epoch.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PART 4: MQTT SPECIFICATION (INTERNET ESP32 ONLY)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Library: PubSubClient (by Nick O'Leary). Run MQTT logic inside a dedicated
FreeRTOS task (mqttTask) pinned to Core 0. PubSubClient.loop() called every
50ms inside the task.

Config defines (in SmartNest.ino):
  #define MQTT_ENABLED       false            // Set true to activate
  #define MQTT_BROKER_HOST   "broker.hivemq.com"
  #define MQTT_BROKER_PORT   1883
  #define MQTT_CLIENT_ID     "SmartNest_001"
  #define MQTT_USERNAME      ""               // empty = no auth
  #define MQTT_PASSWORD      ""
  #define MQTT_KEEPALIVE_S   60
  #define MQTT_BASE_TOPIC    "smartnest"

PUBLISH TOPICS (SmartNest → Broker):
  All retained=true unless marked otherwise.

  smartnest/relay/0/state   → "true" or "false"  (per relay, retained)
  smartnest/relay/1/state   → ...
  smartnest/relay/2/state   → ...
  smartnest/relay/3/state   → ...
  smartnest/relay/4/state   → ...
  smartnest/relay/5/state   → ...
  smartnest/relay/0/locked  → "true" or "false"  (per relay, retained)
  ...
  smartnest/relay/6/state   → "true" or "false"  (Digital Board relay, retained)
  smartnest/relay/6/locked  → "true" or "false"  (Digital Board lock, retained)
  smartnest/switch/6/state  → "true" or "false"  (Digital Board physical
                                                    switch, NOT retained)
  smartnest/shutdown        → "true" or "false"  (retained)
  smartnest/sensor/voltage  → "230.5"            (not retained)
  smartnest/sensor/acs      → "1.23"             (Digital Slave's ACS712
                                                    current, not retained)
  smartnest/sensor/load     → "2.35"             (Internet ESP32's own
                                                    ACS712 — combined 6-relay
                                                    load, not retained)
  smartnest/sensor/power    → "218.9"            (not retained)
  smartnest/energy/daily    → "0.150"            (kWh, not retained)
  smartnest/energy/monthly  → "4.210"            (kWh, not retained)
  smartnest/energy/lifetime → "127.345"          (kWh, not retained)
  smartnest/slave/d1/online → "true" or "false"  (retained)
  smartnest/slave/pzem/online → "true" or "false" (retained)
  smartnest/status          → full JSON payload  (not retained, heartbeat)
  smartnest/history         → {"epoch":...,"v":...,"load":...,"pi":...,"powerVA":...}
                              (not retained; one reading per message; published
                              only during SD→cloud backfill via "hist" batches;
                              "load" = internetAcsCurrentA, matching the
                              smartnest/sensor/load convention)

  Telemetry (non-retained topics) published every HEARTBEAT_MS = 30000ms
  OR immediately on any relay/lock/shutdown state change.
  Retained topics republished immediately on state change.

SUBSCRIBE TOPICS (Broker → SmartNest, on connect):
  smartnest/relay/+/set     → payload "true"/"false". For idx 0–5:
                               setRelayState(idx, state). For idx==6: forward
                               to Master via UART cmd (relay_on/relay_off)
                               instead of touching local GPIO.
  smartnest/relay/+/lock    → payload "true"/"false". For idx 0–5:
                               setLockState(idx, state). For idx==6: forward
                               to Master via UART cmd (relay_lock/relay_unlock).
  smartnest/cmd/shutdown    → payload "true"/"false" → masterShutdown
  smartnest/cmd/slave/d1    → payload: relay_on, relay_off, relay_lock,
                               relay_unlock, reboot → send UART cmd to Master
  smartnest/cmd/slave/pzem  → payload: reboot, energy_reset → send UART to Master

MQTT CALLBACK LOGIC:
  Parse topic to extract relay index from "smartnest/relay/N/set" using
  substring between 3rd and 4th '/' character. If idx is 0–5, call
  setRelayState()/setLockState() as before. If idx == 6, do NOT touch any
  local array — instead build the equivalent UART "cmd" JSON
  ({"t":"cmd","tgt":"d1","cmd":"relay_on"/"relay_off"/"relay_lock"/
  "relay_unlock"}) and enqueue it to UartCmdQueue.
  After any state change triggered by MQTT: push WebSocket update to dashboard.
  After any relay/lock/shutdown change: re-publish corresponding retained topic.

MQTT RECONNECT LOGIC (inside mqttTask):
  If WiFi connected and MQTT disconnected:
    Attempt reconnect every 5 seconds (non-blocking via millis() timer).
    On connect: subscribe to all command topics.
    On connect: publish all retained relay/lock/shutdown states immediately,
    INCLUDING relay 6's state/locked (so cloud stays in sync after reconnect).

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PART 5: FILE-BY-FILE IMPLEMENTATION REQUIREMENTS
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

════════════════════════════════════════
FILE A: master.ino  (FULL REWRITE)
════════════════════════════════════════

LIBRARIES:
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
  No ArduinoJson. No JSON on ESP-NOW path.

CENTRALIZED STATE (implement exactly as specified):

  struct DigitalBoardState {
      float    rmsCurrent;
      uint8_t  relayState;   // 0 or 1 (local relay on slave board)
      uint8_t  switchState;  // 0 or 1 (local switch on slave board)
      bool     locked;       // masterLock state, tracked by Master itself —
                              // set/cleared in uartTask when forwarding
                              // relay_lock/relay_unlock to "d1". The 7-byte
                              // DigitalSlavePacket does NOT echo lock state
                              // back, so this is an optimistic local flag,
                              // not a confirmed-by-slave value.
      int      rssi;
      uint32_t lastSeenTime;
      bool     online;
  };

  struct PzemBoardState {
      float    voltage;
      float    current;
      float    power;
      float    energy;       // PZEM cumulative register (Wh from device)
      int      rssi;
      uint32_t lastSeenTime;
      bool     online;
  };

  struct EnergyState {
      double   activePowerVA;    // V_pzem * I_internetACS712 (combined load)
      double   dailyEnergy;      // Wh, resets midnight
      double   monthlyEnergy;    // Wh, resets 1st of month
      double   lifetimeEnergy;   // Wh, persistent
      uint32_t lastCalcTime;
      bool     pendingSave;      // true when save needed
      double   lastSavedLifetime; // track 0.02kWh threshold
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
      uint8_t  syncStatus;   // 0=Idle, 1=Syncing, 2=Error
      bool     cardPresent;
      uint32_t lastSyncedEpoch;  // in-RAM copy of /sync.bin; epoch up to which
                                  // records have been confirmed published to cloud
      uint32_t lastLoggedEpoch;  // epoch of the most recently SD-logged record
  };

  struct SystemData {
      DigitalBoardState digital;   // digital.rmsCurrent = Digital Slave's OWN
                                    // ACS712 (its local relay only)
      PzemBoardState    pzem;
      EnergyState       energy;
      TimeState         time;
      SdCardState       sd;
      float internetAcsCurrentA;   // Internet ESP32's own ACS712 — combined
                                    // current of all 6 relays. THIS is what
                                    // feeds the energy calculation, not
                                    // digital.rmsCurrent.
      bool  cloudOnline;           // updated from Internet ESP32's "cloud"
                                    // message; true = MQTT broker reachable
  };

  SemaphoreHandle_t g_stateMutex;
  SystemData        g_systemState;

QUEUE DEFINITIONS:
  // Raw ESP-NOW RX buffer
  struct EspNowPacket {
      uint8_t  srcMac[6];
      uint8_t  data[32];   // max binary packet size
      uint8_t  len;
      int      rssi;
  };
  QueueHandle_t g_espNowQueue;    // length 10, item = EspNowPacket

  // Outgoing UART messages to Internet ESP32
  struct UartTxItem {
      char     json[768];          // pre-built JSON string.
                                   // 768 required for "hist" backfill batches:
                                   // 10 records × ~70B each + wrapper ≈ 730B.
                                   // Short messages (tel/cmd/ntp/cloud/acs) are
                                   // well under 200B — the extra space is unused
                                   // stack in those items, not runtime overhead.
                                   // Queue RAM: 15 × 768B ≈ 11.5KB, negligible
                                   // on ESP32's 320KB DRAM.
  };
  QueueHandle_t g_uartTxQueue;    // length 15, item = UartTxItem

  // SD log records
  struct __attribute__((packed)) SdLogRecord {
      uint32_t epoch;        // 4 bytes
      float    voltage;      // 4 bytes
      float    acsCurrent;   // 4 bytes  (= internetAcsCurrentA, combined load)
      float    pzemCurrent;  // 4 bytes
      float    powerVA;      // 4 bytes
      uint8_t  relayStates;  // 1 byte   (bitfield, future expansion)
  };                         // sizeof(SdLogRecord) = 21 bytes packed.
                             // __attribute__((packed)) mandatory: without it
                             // the trailing uint8_t gets 3 bytes of padding
                             // (24 bytes total) which corrupts file seeks
                             // when backfill reads records by offset.
  QueueHandle_t g_sdQueue;        // length 30, item = SdLogRecord

  // High-priority switch events
  struct SwitchEvent {
      uint8_t idx;   // 0–5
      bool    state; // true=ON, false=OFF
  };
  QueueHandle_t g_switchQueue;    // length 10, item = SwitchEvent

  // Backfill synchronisation — signalled by uartTask when hist_ack arrives;
  // consumed by sdLoggingTask backfill loop. Binary semaphore (not counting):
  // if ack arrives while sdLoggingTask is building the next batch the give()
  // is simply latched and taken on the next xSemaphoreTake() call.
  SemaphoreHandle_t g_histAckSem;

OVERFLOW POLICY:
  - g_espNowQueue / g_uartTxQueue: xQueueSend with 0 timeout. On full: drop,
    no increment needed (telemetry only). Relay command retries handled separately.
  - g_sdQueue: on full → increment g_systemState.sd.droppedRecords, drop oldest
    by doing xQueueReceive discard then re-enqueue new item.
  - g_switchQueue: xQueueSend with 0 timeout. Drop on full (switch will retrigger).

MAC ADDRESSES:
  const uint8_t SLAVE1_MAC[] = {0x14,0x08,0x08,0xA4,0x94,0x1C}; // Digital
  const uint8_t SLAVE2_MAC[] = {0x78,0x21,0x84,0x9C,0x98,0x4C}; // PZEM

SWITCH PINS:
  const int SWITCH_PINS[6] = {13, 15, 19, 4, 32, 18};
  All INPUT_PULLDOWN (active HIGH).
  Debounce: 50ms, check every 10ms in switchPollTask.
  On debounced change: enqueue SwitchEvent to g_switchQueue.

FREERTOS TASK LAYOUT:

  Core 0 tasks:
  ┌─────────────────────────────────────────────────────────────────┐
  │ Task: espNowTask      Priority: 3 (High)   Stack: 4096           │
  │   - Dequeues EspNowPacket from g_espNowQueue                     │
  │   - Validates srcMac against SLAVE1_MAC / SLAVE2_MAC             │
  │   - Casts data buffer to correct struct (DigitalSlavePacket or   │
  │     PzemSlavePacket) based on type byte and len validation       │
  │   - Updates g_systemState under mutex: rmsCurrent, relayState,   │
  │     switchState, rssi, lastSeenTime, online (does NOT touch      │
  │     digital.locked — see uartTask for that)                      │
  │   - Sends ACK ping (CmdPacket type=0x01) every 10s to each       │
  │     slave (staggered: Digital at 0s, PZEM at 5s offset)          │
  └─────────────────────────────────────────────────────────────────┘
  ┌─────────────────────────────────────────────────────────────────┐
  │ Task: uartTask        Priority: 2 (Medium) Stack: 4096            │
  │   - TX: dequeues UartTxItem from g_uartTxQueue, writes to UART   │
  │   - RX: accumulates chars into line buffer, on '\n': parses t:   │
  │       "cmd" → extract tgt+cmd, send CmdPacket via ESP-NOW to     │
  │               correct slave. If tgt=="d1" AND cmd is             │
  │               "relay_lock"/"relay_unlock" → also set             │
  │               g_systemState.digital.locked = true/false under   │
  │               mutex (optimistic — slave doesn't echo it back)    │
  │       "ntp" → update TimeState                                   │
  │       "acs" → extract i (float) → update                        │
  │               g_systemState.internetAcsCurrentA under mutex      │
  └─────────────────────────────────────────────────────────────────┘
  ┌─────────────────────────────────────────────────────────────────┐
  │ Task: switchPollTask  Priority: 3 (High)   Stack: 2048            │
  │   - Reads all 6 SWITCH_PINS every 10ms with 50ms debounce        │
  │   - On confirmed state change: enqueue SwitchEvent               │
  │   - Also dequeues from g_switchQueue and builds UART TX JSON:    │
  │     {"t":"sw","idx":N,"s":1} → enqueue to g_uartTxQueue          │
  └─────────────────────────────────────────────────────────────────┘

  Core 1 tasks:
  ┌─────────────────────────────────────────────────────────────────┐
  │ Task: energyTimeTask  Priority: 2 (Medium) Stack: 4096            │
  │   - Runs every 1 second                                          │
  │   - Reads g_systemState.pzem.voltage AND                         │
  │     g_systemState.internetAcsCurrentA under mutex (NOT           │
  │     g_systemState.digital.rmsCurrent — that's only the Digital   │
  │     Slave's own local relay current)                             │
  │   - Calculates activePowerVA = voltage × internetAcsCurrentA     │
  │   - Computes deltaWh = activePowerVA × deltaMs / 3600000.0       │
  │   - Accumulates dailyEnergy, monthlyEnergy, lifetimeEnergy       │
  │   - If timeValid: compute local time from epoch, check midnight  │
  │     reset (dailyEnergy = 0) and 1st-of-month reset (monthly=0)   │
  │   - Marks energy.pendingSave = true if change occurred           │
  │   - If (lifetimeEnergy - lastSavedLifetime) > 20.0 (Wh = 0.02kWh) │
  │     → enqueue SD save immediately                                │
  │   - Every 5 minutes: if pendingSave → enqueue SD save            │
  │   - Every 10 seconds: build telemetry JSON (including d_relay,   │
  │     d_sw, d_lock read from g_systemState.digital) and enqueue    │
  │     to g_uartTxQueue                                             │
  │   - Every 10 seconds (exact same tick as telemetry TX above):    │
  │     build SdLogRecord {epoch, voltage, acsCurrent=              │
  │     internetAcsCurrentA, pzemCurrent, powerVA}, enqueue to       │
  │     g_sdQueue, update g_systemState.sd.lastLoggedEpoch = epoch   │
  │     under mutex. Every "tel" UART message has an exact           │
  │     corresponding SD record — these are the records backfill uses.│
  └─────────────────────────────────────────────────────────────────┘
  ┌─────────────────────────────────────────────────────────────────┐
  │ Task: sdLoggingTask   Priority: 1 (Low)    Stack: 8192            │
  │                                                                   │
  │ BOOT INIT (runs once before entering main loop):                  │
  │   - Open /state.bin → restore energy counters (see SD CARD       │
  │     STATE RECOVERY ON BOOT below)                                 │
  │   - Open /sync.bin (4 bytes, uint32_t) → restore                 │
  │     g_systemState.sd.lastSyncedEpoch; if missing/corrupt → 0     │
  │                                                                   │
  │ MAIN LOOP (runs continuously — this task is the SOLE owner of    │
  │ all SD operations: /state.bin, /logs/*.dat, /sync.bin):           │
  │                                                                   │
  │  ① Log incoming records:                                          │
  │     - xQueueReceive g_sdQueue with 100ms timeout                  │
  │     - On record: build filename "/logs/YYYY_MM_DD.dat" from       │
  │       epoch; append sizeof(SdLogRecord) = 21 bytes (packed)       │
  │     - On SD error: increment droppedRecords, do NOT stall         │
  │                                                                   │
  │  ② Persist energy state:                                          │
  │     - When triggered by energyTimeTask (via g_sdQueue special     │
  │       save-request item or flag): write /state.bin                │
  │       (dailyEnergy, monthlyEnergy, lifetimeEnergy, epoch)         │
  │                                                                   │
  │  ③ Backfill (one batch per MAIN LOOP pass — ① always has priority): │
  │     - Check cloudOnline AND (lastSyncedEpoch < lastLoggedEpoch)   │
  │       under mutex; if either false → skip this pass entirely       │
  │     - Open /logs/YYYY_MM_DD.dat containing epoch                  │
  │       (lastSyncedEpoch + 1); seek to first record with epoch >    │
  │       lastSyncedEpoch; read up to 10 such records                 │
  │     - Build {"t":"hist","batch":N,"recs":[...]} into a local      │
  │       char buf[768]; enqueue to g_uartTxQueue (0 timeout; if      │
  │       full, skip this pass — ① will run first, then retry)        │
  │     - Block on xSemaphoreTake(g_histAckSem, 5000ms):             │
  │         · Ack arrived: read lastSyncedEpoch from g_systemState    │
  │           under mutex; write /sync.bin (4-byte uint32 overwrite); │
  │           if caught up → syncStatus=0; else syncStatus=1          │
  │         · Timeout (5s): retry same batch, up to 3 times           │
  │         · 3 retries exhausted → syncStatus=2 (Error);             │
  │           vTaskDelay(30s); then resume (raw logs always intact)    │
  │     - After ack OR after retries exhausted: RETURN to MAIN LOOP   │
  │       top. ① drains g_sdQueue before the next batch is attempted. │
  │       "Loop immediately for next batch" is FORBIDDEN — live        │
  │       records must not be starved during long catch-up periods.   │
  └─────────────────────────────────────────────────────────────────┘
  ┌─────────────────────────────────────────────────────────────────┐
  │ Task: healthTask      Priority: 1 (Low)    Stack: 2048            │
  │   - Runs every 5 seconds                                          │
  │   - For each slave: if millis()-lastSeenTime > 30000 AND online   │
  │     → set online=false, enqueue offline UART alert:               │
  │     {"t":"off","dev":"d1"} or {"t":"off","dev":"pzem"}            │
  │   - If slave comes back online (detected in espNowTask when       │
  │     packet received after offline state): enqueue:                │
  │     {"t":"on","dev":"d1"} to g_uartTxQueue                       │
  └─────────────────────────────────────────────────────────────────┘

ESP-NOW RECEIVE CALLBACK (ISR-safe):
  void onReceive(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
      EspNowPacket pkt;
      memcpy(pkt.srcMac, info->src_addr, 6);
      memcpy(pkt.data, data, min(len, 32));
      pkt.len = len;
      pkt.rssi = info->rx_ctrl->rssi;
      xQueueSendFromISR(g_espNowQueue, &pkt, NULL);
  }

PACKET VALIDATION IN espNowTask:
  For Digital Slave packet: validate len == sizeof(DigitalSlavePacket) (7 bytes)
                            AND type byte is 0x10 or 0x11
  For PZEM Slave packet:    validate len == sizeof(PzemSlavePacket) (17 bytes)
                            AND type byte is 0x20 or 0x21
  On mismatch: drop silently.

UART HARDWARE:
  HardwareSerial MasterUart(2);  // UART2
  MasterUart.begin(115200, SERIAL_8N1, 16, 17);  // RX=16, TX=17
  (Master GPIO17 → SmartNest GPIO16, Master GPIO16 ← SmartNest GPIO17)

UART RX LINE PARSER (in uartTask RX side):
  Accumulate chars into char rxBuf[256] until '\n'.
  On '\n': parse t field manually:
    if t=="ntp" → extract epoch, tz_h, tz_m → update TimeState under mutex
    if t=="cmd" → extract tgt, cmd → build CmdPacket, esp_now_send to correct
                  MAC. If tgt=="d1" and cmd=="relay_lock"/"relay_unlock",
                  also update g_systemState.digital.locked under mutex.
    if t=="acs" → extract i (float, manually parsed — no library) → update
                  g_systemState.internetAcsCurrentA under mutex
    if t=="cloud" → extract up (bool) → update g_systemState.cloudOnline under
                    mutex. If up==false, the backfill loop inside sdLoggingTask
                    will automatically pause on its next cloudOnline check.
    if t=="hist_ack" → extract upto (uint32_t) → update
                       g_systemState.sd.lastSyncedEpoch = upto under mutex;
                       then xSemaphoreGive(g_histAckSem) to wake sdLoggingTask.
                       Do NOT touch SD from uartTask — sdLoggingTask is the
                       sole SD owner and will write /sync.bin on its next loop.

SD CARD STATE RECOVERY ON BOOT:
  In sdLoggingTask init: attempt to open /state.bin.
  Binary layout: [double dailyEnergy][double monthlyEnergy][double lifetimeEnergy]
                 [uint32_t savedEpoch]  (total 28 bytes)
  If open succeeds AND savedEpoch is within 1 day of current epoch (if time valid):
    restore dailyEnergy, monthlyEnergy, lifetimeEnergy.
  Else if savedEpoch is different day: restore only monthlyEnergy, lifetimeEnergy.

  Also in sdLoggingTask init: attempt to open /sync.bin (4 bytes, uint32_t).
  If open succeeds AND file size == 4: read into g_systemState.sd.lastSyncedEpoch.
  If missing, zero-size, or read error: set g_systemState.sd.lastSyncedEpoch = 0
  (sdLoggingTask backfill loop will start from the beginning of SD logs).
  /sync.bin is a persistent pointer only — it is overwritten on every hist_ack
  (by sdLoggingTask, never by uartTask). The raw log files are never deleted
  or modified based on sync state.

WIFI INIT:
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  // Then esp_now_init() — this order avoids boot loop.

setup() only creates queues, mutex, g_histAckSem (binary semaphore, initially
not given), registers ESP-NOW callbacks, registers both slave peers, and
launches all 6 FreeRTOS tasks. loop() calls vTaskDelay(portMAX_DELAY).

════════════════════════════════════════
FILE B: SmartNest.ino  (MODIFICATION — keep existing + changes below)
════════════════════════════════════════

WHAT TO KEEP UNCHANGED:
  - All relay hardware logic: RELAY_PINS[], setRelayState(), toggleRelay(),
    updateRelayHardware(), relayEventQueue, relayStates[], dashboardStates[],
    lockedStates[], masterShutdown, /api/relay, /api/lock, /api/shutdown
    endpoints (the body of /api/relay and /api/lock gets a small ADDITION
    below for index 6 — see "MODIFIED EXISTING ENDPOINTS")
  - WiFi provisioning: wifiManagerInit(), captive portal, /scan, /connect,
    /connect-status, provision_html.h — no changes
  - mDNS: MDNS.begin("smart-nest") → accessible as smart-nest.local — keep
  - wifiReconnectTask — keep
  - resetButtonTask (GPIO0, hold 3s) — keep
  - NVS Preferences for WiFi credentials — keep
  - Firmware version, NVS namespace, MDNS hostname defines — keep
  - ACS712 hardware logic on this board — KEEP AS-IS, do NOT remove:
      #define ACS712_PIN, ACS712_SENSITIVITY, ACS712_DIVIDER_RATIO,
      ACS712_RMS_SAMPLES, ACS712_READ_INTERVAL_MS, ACS712_DEADBAND_A,
      ACS712_AUTO_CALIBRATE, ACS712_ZERO_POINT_MV, acs712ZeroMv variable
      void currentSensorInit()
      float readCurrent()
      void currentSensorTask()
      xTaskCreatePinnedToCore(currentSensorTask, ...) in setup()
      sysState.currentAmps (existing field) — this is the Internet ESP32's
      own ACS712 reading, i.e. the COMBINED current of all 6 relays. It is
      used both for local dashboard display ("Combined Load") and is sent to
      Master every 10 seconds for energy calculation.

WHAT TO REMOVE FROM SmartNest.ino:
  - DELETE: struct UartData and sysState.uartData
  - DELETE: void cloudClientTask() and all HTTPClient / WiFiClientSecure code
  - DELETE: #define CLOUD_ENABLED, CLOUD_URL, HTTP_TIMEOUT_MS
  - DELETE: void parseCommands(), String buildPayload()
  - DELETE: checkAndSendRelayUpdates() and lastSentRelayStates[]

  (The ACS712 sensor code — defines, currentSensorInit(), readCurrent(),
  currentSensorTask(), and its setup() task launch — is NOT deleted. It
  stays exactly as it is; see "WHAT TO KEEP UNCHANGED" above. Also DELETE
  the old switch-pin block as before: #define SWITCH_1_PIN..SWITCH_6_PIN,
  const int SWITCH_PINS[NUM_RELAYS], bool lastSwitchState[NUM_RELAYS], the
  switch pinMode() calls inside relaySwitchInit(), and the
  digitalRead(SWITCH_PINS[i]) logic inside relaySwitchTask() — keep only
  relay GPIO init and relay hardware state in that function.)

WHAT TO ADD/MODIFY in SystemState struct:
  struct SystemState {
      // ── KEEP (existing) ──
      bool  relayStates[NUM_RELAYS];      // index 0–5, Internet ESP32 GPIO
      bool  dashboardStates[NUM_RELAYS];
      bool  masterShutdown;
      bool  lockedStates[NUM_RELAYS];
      bool  wifiConnected;
      int   wifiRSSI;
      char  wifiSSID[33];
      unsigned long lastChangeMs;
      float currentAmps;          // EXISTING — Internet ESP32's own ACS712,
                                   // combined current of all 6 relays. Set
                                   // by currentSensorTask(). Sent to Master
                                   // every 10s as {"t":"acs","i":X.XX}.

      // ── ADD (telemetry from Master via UART) ──
      float  acsCurrentA;        // Digital Slave's ACS712 current (relayed
                                  // through Master's "tel" packet — NOT the
                                  // combined value; that's currentAmps above)
      float  pzemVoltage;        // PZEM voltage (V)
      float  pzemCurrentA;       // PZEM current (A)
      float  pzemPowerW;         // PZEM apparent power (W)
      double energyDailyWh;      // Daily energy from master (Wh)
      double energyMonthlyWh;    // Monthly energy from master (Wh)
      double energyLifetimeWh;   // Lifetime energy from master (Wh)
      bool   digitalSlaveOnline;
      bool   pzemSlaveOnline;
      int    digitalSlaveRSSI;
      int    pzemSlaveRSSI;

      // ── ADD (7th relay — Digital Board Slave, via Master/ESP-NOW) ──
      bool   digitalRelayState;   // Digital Board relay's current state
                                   // Source: DigitalSlavePacket.relayState
                                   //   → Master ESP-NOW RX → UART "tel" (d_relay)
      bool   digitalRelayLocked;  // masterLock flag on Digital Slave
                                   // controlled via /api/slave-cmd or
                                   // /api/relay & /api/lock with relay==6;
                                   // reflected back via "tel" (d_lock)
      bool   digitalSwitchState;  // Digital Board's physical switch state
                                   // Source: DigitalSlavePacket.switchState
                                   //   → Master → UART "tel" (d_sw)
  };

UART TASK REPLACEMENT (uartCommTask):
  Keep the HardwareSerial UartSerial(2) on pins 16/17.
  TX side: sends two kinds of messages to Master, both via UartCmdQueue
  (String, length 5):
    1. Slave commands ({"t":"cmd","tgt":...,"cmd":...})
       - Triggered by /api/slave-cmd, MQTT command topics, or by
         /api/relay & /api/lock when relayIdx==6 (forwarded to "d1" as
         relay_on/relay_off/relay_lock/relay_unlock)
    2. Combined ACS712 current ({"t":"acs","i":X.XX})
       - Sent every 10 seconds, synced with the existing telemetry cadence,
         reading sysState.currentAmps (this board's own ACS712 reading)
  RX side: replace parseIncomingData() entirely with new parser:
    Parse t field:
      "tel" → extract and store in sysState under mutex:
                acsCurrentA, pzemVoltage, pzemCurrentA, pzemPowerW,
                energyDailyWh (d_wh), energyMonthlyWh (m_wh),
                energyLifetimeWh (l_wh), digitalSlaveOnline (d_on),
                pzemSlaveOnline (p_on), digitalSlaveRSSI (d_rssi),
                pzemSlaveRSSI (p_rssi), digitalRelayState (d_relay),
                digitalSwitchState (d_sw), digitalRelayLocked (d_lock)
              → call pushWsUpdate() to push to dashboard WebSocket clients
              → if MQTT connected: publish sensor/energy/slave/relay-6 topics
      "sw"  → extract idx (0–5) and s (0 or 1)
              → call setRelayState(idx, s==1)
              (Master switch controls SmartNest relay directly)
      "off" → extract dev ("d1"/"pzem"), set corresponding SlaveOnline = false
              → call pushWsUpdate()
              → if MQTT: publish smartnest/slave/d1/online "false" retained
      "on"  → set corresponding SlaveOnline = true → pushWsUpdate()
      "hist" → extract batch (int) and recs array; for each record publish
               {"epoch":...,"v":...,"load":...,"pi":...,"powerVA":...} to
               smartnest/history (not retained, one message per record).
               "load" maps to the internetAcsCurrentA field in the SdLogRecord
               (combined 6-relay current) — consistent with smartnest/sensor/load.
               Use StaticJsonDocument<800> (or larger) when parsing incoming
               "hist" messages — the 10-record payload can reach ~730 bytes.
               ALL records in the batch must be published successfully before
               sending hist_ack. If ANY publish fails, do NOT send hist_ack
               (sdLoggingTask backfill loop will retry after 5s timeout). On
               full batch success, build and enqueue to UartCmdQueue:
               {"t":"hist_ack","batch":N,"upto":lastRecordEpoch}

NTP TASK (new, runs on Core 0):
  Name: ntpSyncTask, Priority 1, Stack 4096, Core 0
  Every 5 minutes (when WiFi connected):
    configTime(19800, 0, "pool.ntp.org");  // IST = UTC+5:30
    Wait up to 5s for time to be set.
    On success: extract epoch, build UART JSON:
      {"t":"ntp","epoch":NNNN,"tz_h":5,"tz_m":30}
    Enqueue to UartCmdQueue to send to Master.

WEBSOCKET (add to SmartNest.ino):
  Library: ESPAsyncWebServer already included → use AsyncWebSocket.
  #include <AsyncWebSocket.h>
  AsyncWebSocket ws("/ws");

  In dashboardInit(): dashServer.addHandler(&ws);

  void pushWsUpdate() {
      if (ws.count() == 0) return;
      String payload = buildStatusJSON();
      ws.textAll(payload);
  }

  Call pushWsUpdate() from:
    - uartCommTask when "tel" or "off" or "on" received
    - setRelayState() after hardware update
    - /api/lock handler after lock state change
    - /api/shutdown handler after shutdown state change
    - mqttTask when command changes relay/lock/shutdown state

MODIFIED buildStatusJSON():
  Add to existing JSON output (after existing fields):
  json += ",\"acs\":"       + String(sysState.acsCurrentA, 2);   // Digital Board
  json += ",\"load\":"      + String(sysState.currentAmps, 2);   // Combined 6-relay load
  json += ",\"v\":"         + String(sysState.pzemVoltage, 1);
  json += ",\"pw\":"        + String(sysState.pzemPowerW, 1);
  json += ",\"daily\":"     + String(sysState.energyDailyWh / 1000.0, 3);
  json += ",\"monthly\":"   + String(sysState.energyMonthlyWh / 1000.0, 3);
  json += ",\"lifetime\":"  + String(sysState.energyLifetimeWh / 1000.0, 3);
  json += ",\"d_on\":"      + String(sysState.digitalSlaveOnline ? "true":"false");
  json += ",\"p_on\":"      + String(sysState.pzemSlaveOnline ? "true":"false");
  json += ",\"d_rssi\":"    + String(sysState.digitalSlaveRSSI);
  json += ",\"p_rssi\":"    + String(sysState.pzemSlaveRSSI);
  json += ",\"d_relay\":"   + String(sysState.digitalRelayState ? "true" : "false");
  json += ",\"d_lock\":"    + String(sysState.digitalRelayLocked ? "true" : "false");
  json += ",\"d_sw\":"      + String(sysState.digitalSwitchState ? "true" : "false");

MODIFIED EXISTING ENDPOINTS — /api/relay and /api/lock (handle 7th relay):

  /api/relay (existing handler, add an else-if branch):
  if (relayIdx >= 0 && relayIdx < NUM_RELAYS) {
      // existing logic — direct GPIO relay, unchanged
      setRelayState(relayIdx, state);

  } else if (relayIdx == 6) {
      // 7th relay — Digital Board Slave (no local GPIO)
      String cmd = state ? "relay_on" : "relay_off";
      enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"" + cmd + "\"}");
      // sysState.digitalRelayState updates from the NEXT "tel" packet
      // (d_relay field) — no optimistic local write here.
  }

  /api/lock (existing handler, add an else-if branch):
  if (relayIdx >= 0 && relayIdx < NUM_RELAYS) {
      // existing logic, unchanged
      ...
  } else if (relayIdx == 6) {
      String cmd = state ? "relay_lock" : "relay_unlock";
      enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"" + cmd + "\"}");
      // sysState.digitalRelayLocked updates from the NEXT "tel" packet
      // (d_lock field).
  }

ADD new API endpoint /api/slave-cmd (POST):
  Body: {"target":"d1","cmd":"reboot"}
  Handler: build UART JSON {"t":"cmd","tgt":"d1","cmd":"reboot"}, enqueue to
  UartCmdQueue. Respond: {"success":true}
  Valid targets: "d1" (cmds: relay_on, relay_off, relay_lock, relay_unlock, reboot)
                 "pzem" (cmds: reboot, energy_reset)

MQTT TASK (new, replaces cloudClientTask):
  Name: mqttTask, Priority 1, Stack 8192, Core 0
  Guard with #if MQTT_ENABLED

  WiFiClient mqttWifiClient;
  PubSubClient mqttClient(mqttWifiClient);

  void mqttCallback(char* topic, byte* payload, unsigned int length):
    String t = String(topic);
    String p = String((char*)payload, length);
    p.trim();

    // Relay set: smartnest/relay/N/set
    if (t.startsWith(MQTT_BASE_TOPIC "/relay/") && t.endsWith("/set")) {
        int idx = t.substring(strlen(MQTT_BASE_TOPIC) + 7,
                              t.length() - 4).toInt();
        if (idx >= 0 && idx < NUM_RELAYS) {
            setRelayState(idx, p == "true");
        } else if (idx == 6) {
            enqueueUartCmd(String("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"") +
                           (p == "true" ? "relay_on" : "relay_off") + "\"}");
        }
    }
    // Relay lock: smartnest/relay/N/lock
    if (t.startsWith(MQTT_BASE_TOPIC "/relay/") && t.endsWith("/lock")) {
        int idx = t.substring(strlen(MQTT_BASE_TOPIC) + 7,
                              t.length() - 5).toInt();
        if (idx >= 0 && idx < NUM_RELAYS) {
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                sysState.lockedStates[idx] = (p == "true");
                xSemaphoreGive(stateMutex);
            }
            updateRelayHardware();
        } else if (idx == 6) {
            enqueueUartCmd(String("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"") +
                           (p == "true" ? "relay_lock" : "relay_unlock") + "\"}");
        }
    }
    // Master shutdown
    if (t == MQTT_BASE_TOPIC "/cmd/shutdown") {
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            sysState.masterShutdown = (p == "true");
            xSemaphoreGive(stateMutex);
        }
        updateRelayHardware();
    }
    // Slave commands
    if (t == MQTT_BASE_TOPIC "/cmd/slave/d1")
        enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"d1\",\"cmd\":\"" + p + "\"}");
    if (t == MQTT_BASE_TOPIC "/cmd/slave/pzem")
        enqueueUartCmd("{\"t\":\"cmd\",\"tgt\":\"pzem\",\"cmd\":\"" + p + "\"}");
    // After any state change: push WebSocket update
    pushWsUpdate();
    // Republish changed retained topic immediately

  void mqttTask(void* pvParameters):
    mqttClient.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setKeepAlive(MQTT_KEEPALIVE_S);

    uint32_t lastReconnectAttempt = 0;
    uint32_t lastHeartbeat = 0;
    bool wasMqttConnected = false;  // track previous connection state to detect
                                    // true→false (disconnect) and false→true
                                    // (reconnect) transitions

    while (true) {
        if (WiFi.status() == WL_CONNECTED) {
            if (!mqttClient.connected()) {
                if (wasMqttConnected) {
                    // Just disconnected — notify Master to pause backfill
                    enqueueUartCmd("{\"t\":\"cloud\",\"up\":false}");
                    wasMqttConnected = false;
                }
                if (millis() - lastReconnectAttempt > 5000) {
                    lastReconnectAttempt = millis();
                    bool ok;
                    if (strlen(MQTT_USERNAME) > 0)
                        ok = mqttClient.connect(MQTT_CLIENT_ID,
                                                MQTT_USERNAME, MQTT_PASSWORD);
                    else
                        ok = mqttClient.connect(MQTT_CLIENT_ID);
                    if (ok) {
                        // Subscribe
                        mqttClient.subscribe(MQTT_BASE_TOPIC "/relay/+/set");
                        mqttClient.subscribe(MQTT_BASE_TOPIC "/relay/+/lock");
                        mqttClient.subscribe(MQTT_BASE_TOPIC "/cmd/shutdown");
                        mqttClient.subscribe(MQTT_BASE_TOPIC "/cmd/slave/d1");
                        mqttClient.subscribe(MQTT_BASE_TOPIC "/cmd/slave/pzem");
                        // Publish all retained states immediately on reconnect
                        publishAllRetainedStates();
                        // Notify Master that cloud is available — triggers backfill
                        enqueueUartCmd("{\"t\":\"cloud\",\"up\":true}");
                        wasMqttConnected = true;
                    }
                }
            } else {
                mqttClient.loop();
                if (millis() - lastHeartbeat > HEARTBEAT_MS) {
                    lastHeartbeat = millis();
                    publishTelemetry();  // publish sensor/energy/slave topics
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

  void publishAllRetainedStates():
    For each relay i (0–5): publish "smartnest/relay/N/state" retained
    For each relay i (0–5): publish "smartnest/relay/N/locked" retained
    Publish "smartnest/relay/6/state" retained (sysState.digitalRelayState)
    Publish "smartnest/relay/6/locked" retained (sysState.digitalRelayLocked)
    Publish "smartnest/shutdown" retained

  void publishTelemetry():
    (take mutex, read sysState, release mutex)
    Publish non-retained sensor + energy + slave topics, including:
      smartnest/sensor/acs   = sysState.acsCurrentA   (Digital Board)
      smartnest/sensor/load  = sysState.currentAmps   (combined 6-relay load)
      smartnest/switch/6/state = sysState.digitalSwitchState

════════════════════════════════════════
FILE C: slave_digital_board.ino  (ESP-NOW PROTOCOL CHANGES ONLY)
════════════════════════════════════════

KEEP UNCHANGED — these functions/features are NOT to be modified:
  - sampleCurrentNonBlocking() — 1kHz non-blocking RMS sampler using micros()
  - processCurrentMeasurements() — 200ms RMS window with deadband 0.15A
  - checkOvercurrentProtection() — 6A trip, 5.5A clear hysteresis, 3-hit counter
  - checkManualSwitch() — 50ms debounce, calls relayOn()/relayOff()
  - relayOn() / relayOff() — masterLock + overcurrentLock guards
  - updateLEDState() — RGB LED priority state machine (7 states)
  - Setup() ACS712 calibration (1000 samples, stored to zeroMilliVolts)
  - Loop() timing logic (200ms current process, 50ms switch, 30s telemetry)
  - masterLock and overcurrentLock boolean flags
  - All pin defines (RELAY_PIN 15, CURRENT_SENSOR_PIN 34, MANUAL_SWITCH_PIN 2,
    RED_LED_PIN 5, GREEN_LED_PIN 18, BLUE_LED_PIN 19)

CHANGE ONLY — ESP-NOW transmit:

  Replace sendSlaveData() with:
  struct __attribute__((packed)) DigitalSlavePacket {
      uint8_t type;
      float   rmsCurrent;
      uint8_t relayState;
      uint8_t switchState;
  };
  void sendSlaveData() {
      DigitalSlavePacket pkt;
      pkt.type        = 0x10;
      pkt.rmsCurrent  = relayCondition ? currentAmperes : 0.0f;
      pkt.relayState  = relayCondition ? 1 : 0;
      pkt.switchState = (digitalRead(MANUAL_SWITCH_PIN) == LOW) ? 1 : 0;
                        // LOW because INPUT_PULLUP + active LOW switch
      esp_now_send(masterMacAddress, (uint8_t*)&pkt, sizeof(pkt));
  }

  Replace sendAckReply() with:
  void sendAckReply() {
      DigitalSlavePacket pkt;
      pkt.type        = 0x11;
      pkt.rmsCurrent  = currentAmperes;
      pkt.relayState  = relayCondition ? 1 : 0;
      pkt.switchState = 0;
      esp_now_send(masterMacAddress, (uint8_t*)&pkt, sizeof(pkt));
  }

CHANGE ONLY — ESP-NOW receive (inside loop(), messageReceived block):
  Replace the entire deserializeJson() block with:
  struct __attribute__((packed)) CmdPacket { uint8_t type; };
  if (receiveLength >= 1) {
      CmdPacket* cmd = (CmdPacket*)receiveBuffer;
      switch (cmd->type) {
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

REMOVE from slave_digital_board.ino:
  - #include <ArduinoJson.h>
  - All JsonDocument / StaticJsonDocument usage
  - sendMasterCommand() if it exists (it doesn't in this file — just ensure
    no JSON encoding anywhere)

════════════════════════════════════════
FILE D: slave_pzem.ino  (ESP-NOW PROTOCOL CHANGES ONLY)
════════════════════════════════════════

KEEP UNCHANGED:
  - readPzemAndSend() PZEM sensor read logic (pzem.voltage(), etc.)
  - pzemHealthy flag and NaN check
  - RGB LED priority state machine (updateRGBLED)
  - Non-blocking reboot scheduling (rebootRequested, rebootTime)
  - Periodic telemetry timer (TELEMETRY_INTERVAL_MS = 15000ms)
  - firstContactMade, lastMasterContactTime tracking
  - All pin defines (PIN_LED_R 5, PIN_LED_G 18, PIN_LED_B 19)
  - PZEM on Serial2 (RX=16, TX=17)
  - MAC override: esp_wifi_set_mac(WIFI_IF_STA, localSlaveMac)
  - All constants (TELEMETRY_INTERVAL_MS, MASTER_TIMEOUT_MS, etc.)

CHANGE ONLY — inside readPzemAndSend(), replace JSON tx with binary:
  struct __attribute__((packed)) PzemSlavePacket {
      uint8_t type;
      float   voltage;
      float   current;
      float   power;
      float   energy;
  };
  void sendPzemPacket(uint8_t pktType) {
      float v = pzem.voltage();
      float i = pzem.current();
      float p = pzem.power();
      float e = pzem.energy();
      if (isnan(v) || isnan(i) || isnan(p) || isnan(e)) {
          pzemHealthy = false;
          return;
      }
      pzemHealthy = true;
      PzemSlavePacket pkt;
      pkt.type    = pktType;
      pkt.voltage = v;
      pkt.current = i;
      pkt.power   = p;
      pkt.energy  = e;
      if (espNowInitialized)
          esp_now_send(masterMAC, (uint8_t*)&pkt, sizeof(pkt));
  }
  // In periodic timer: sendPzemPacket(0x20);
  // In ACK reply:      sendPzemPacket(0x21);
  // In energy_reset (after reset): sendPzemPacket(0x20);

CHANGE ONLY — inside handleIncomingPackets(), replace JSON parse with binary:
  struct __attribute__((packed)) CmdPacket { uint8_t type; };
  if (rxBuffer.len >= 1) {
      lastMasterContactTime = millis();
      firstContactMade = true;
      CmdPacket* cmd = (CmdPacket*)rxBuffer.data;
      yellowActiveUntilTime = millis() + YELLOW_BLINK_DURATION_MS;
      switch (cmd->type) {
          case 0x01:  // ACK ping
              sendPzemPacket(0x21);
              break;
          case 0x06:  // Reboot
              rebootRequested = true;
              rebootTime = millis() + 1000;
              break;
          case 0x07:  // Energy reset
              if (pzem.resetEnergy()) {
                  pzemHealthy = true;
                  sendPzemPacket(0x20);
              } else {
                  pzemHealthy = false;
              }
              break;
      }
  }
  // Note: lastMasterContactTime and firstContactMade are now set for EVERY
  // valid packet (type 0x01, 0x06, 0x07), not just ACK type.

REMOVE from slave_pzem.ino:
  - #include <ArduinoJson.h>
  - All JsonDocument / StaticJsonDocument usage
  - const char* type parameter in readPzemAndSend() → replace with pktType uint8_t

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PART 6: dashboard_html.h  (MODIFY)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Keep the existing visual design (dark theme, CSS variables, all existing card
styles). Make these functional changes:

1. REPLACE HTTP polling with WebSocket:
   const ws = new WebSocket(`ws://${location.hostname}/ws`);
   ws.onopen    = () => { /* connected */ };
   ws.onmessage = (event) => applyState(JSON.parse(event.data));
   ws.onerror   = () => {};
   ws.onclose   = () => setTimeout(() => location.reload(), 3000);
   REMOVE: fetchStatus() function and setInterval(fetchStatus, 1500).

2. ADD Energy Monitor card (insert before relay grid card):
   A card with three stat boxes in a row:
     - "DAILY"   → d.daily.toFixed(3) + " kWh"
     - "MONTHLY" → d.monthly.toFixed(3) + " kWh"
     - "LIFETIME"→ d.lifetime.toFixed(2) + " kWh"
   Below that, a readings row (inline, smaller text):
     - "Voltage: X.X V"        ← d.v
     - "Power: X.X W"          ← d.pw
     - "Combined Load: X.XX A" ← d.load  (Internet ESP32's own ACS712,
                                          all 6 relays combined — actual
                                          house load, used in energy calc)
     - "Digital Board: X.XX A" ← d.acs   (Digital Slave's own ACS712,
                                          relayed via Master telemetry)

3. ADD Slave Status card (insert after Energy card):
   Two rows showing slave online/offline status with colored dots:
     - Green dot + "Digital Board  ONLINE  -XX dBm"
     - Red dot   + "Digital Board  OFFLINE  --"
   Update from d.d_on, d.d_rssi, d.p_on, d.p_rssi in applyState().

4. ADD Slave Commands section inside the main card (after relay grid):
   Two small buttons:
     - "Reboot Digital Slave" → POST /api/slave-cmd {target:"d1",cmd:"reboot"}
     - "Reset PZEM Energy"   → POST /api/slave-cmd {target:"pzem",cmd:"energy_reset"}
   Show a brief status message below buttons on response.

5. ADD 7th relay card to the relay grid (Digital Board Slave):
   Render relay cards for index 0–5 exactly as before, PLUS one extra card
   for index 6:
     ┌─────────────────────────────────────────────┐
     │  Digital Board Relay          [Lock] [Toggle]│
     │  Switch: PRESSED / RELEASED  (read-only)     │
     │  Status: Active ON / Inactive                │
     └─────────────────────────────────────────────┘
   - Toggle → fetch('/api/relay', {relay:6, state:...})
   - Lock   → fetch('/api/lock',  {relay:6, state:...})
   - Switch indicator (d.d_sw) is read-only — no click handler
   - Card state is driven by the same applyState(d) function as the other
     6 cards, reading d.d_relay (toggle), d.d_lock (lock icon), and d.d_sw
     (switch indicator)

6. UPDATE applyState(d) to handle new JSON fields:
   d.acs, d.load, d.v, d.pw, d.daily, d.monthly, d.lifetime,
   d.d_on, d.p_on, d.d_rssi, d.p_rssi, d.d_relay, d.d_lock, d.d_sw
   (existing: d.relays, d.locks, d.shutdown, d.ssid, d.rssi, d.uptime — keep)

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PART 7: REQUIRED LIBRARIES SUMMARY
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

master.ino:
  esp_now.h, WiFi.h, HardwareSerial.h, SD.h, SPI.h, freertos/*
  NO ArduinoJson. NO external library beyond ESP32 Arduino core + SD.

SmartNest.ino:
  ESPAsyncWebServer + AsyncTCP  (async HTTP + WebSocket server)
  AsyncWebSocket.h              (included in ESPAsyncWebServer)
  PubSubClient.h                (MQTT client by Nick O'Leary)
  ArduinoJson.h                 (UART RX parsing only)
  ESPmDNS.h, DNSServer.h, Preferences.h, WiFi.h
  HardwareSerial.h, freertos/*

slave_digital_board.ino:
  WiFi.h, esp_now.h
  NO ArduinoJson

slave_pzem.ino:
  WiFi.h, esp_now.h, esp_wifi.h, PZEM004Tv30.h
  NO ArduinoJson

provision_html.h: NO CHANGES.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PART 8: HARD CONSTRAINTS — DO NOT VIOLATE
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

1. Master ESP32: WiFi.mode(WIFI_STA) + WiFi.disconnect() MUST be called
   BEFORE esp_now_init(). Reversing this order causes boot loops.

2. ESP-NOW callbacks are ISR context. NEVER call Serial.print(), delay(),
   or any FreeRTOS blocking call inside onReceive(). Use xQueueSendFromISR().

3. All access to g_systemState on Master and sysState on SmartNest MUST be
   guarded with xSemaphoreTake() / xSemaphoreGive(). Use pdMS_TO_TICKS(10)
   timeout on the take. Never hold mutex across any blocking operation.

4. SD card operations on Master can block 10–100ms per write. They MUST run
   exclusively on Core 1 in sdLoggingTask. Never call SD.open() from Core 0.

5. PubSubClient.loop() MUST be called frequently (every 50ms) to maintain
   MQTT keepalive. Run it in the dedicated mqttTask only.

6. SmartNest: Do NOT use WiFi.softAP() and WiFi.mode(WIFI_STA) simultaneously
   after provisioning. After provisioning the AP is shut down; mode becomes STA only.

7. AsyncWebSocket: ws.cleanupClients() MUST be called periodically inside
   the main loop or a low-priority task to free disconnected client memory.
   Add: if(!isProvisioningMode) ws.cleanupClients(); in loop().

8. Binary packet structs: ALL must have __attribute__((packed)) to prevent
   compiler from inserting padding bytes that would corrupt sizes.

9. MQTT topic parsing: Do NOT use indexOf() for relay index extraction if
   topic is "smartnest/relay/10/set" (two-digit index). Use proper substring
   split at the correct '/' positions.

10. Energy calculation uses double precision. Do NOT downcast to float until
    display time. Accumulated Wh over months can exceed float precision range.

11. Relay index 6 is VIRTUAL — it represents the Digital Board Slave's relay,
    reached via Master/ESP-NOW over UART, NOT a local GPIO. Every place that
    loops over or parses relay indices (the /api/relay and /api/lock
    handlers, the MQTT callback's substring parsing, the dashboard relay
    grid render loop, buildStatusJSON) must treat index 6 as a special
    UART-forwarded case and must NEVER include it in any NUM_RELAYS-sized
    array or loop bound.

12. The energy calculation's current input is the Internet ESP32's OWN
    ACS712 reading (internetAcsCurrentA / sysState.currentAmps — combined
    6-relay load), received via the UART "acs" message. It is NEVER the
    Digital Slave's rmsCurrent / acsCurrentA — that value is informational
    telemetry only (that slave's own local relay) and must not be summed
    into, substituted for, or averaged with the Internet ESP32's reading.

13. The backfill loop inside sdLoggingTask keeps exactly one batch "in-flight"
    at a time. It MUST NOT enqueue a second "hist" message until the
    hist_ack semaphore (g_histAckSem) is signalled for the previous batch
    (or all 3 retries are exhausted). This prevents UART congestion with
    live telemetry (tel/sw messages). Because backfill runs inside
    sdLoggingTask, it shares that task's Core-1 execution naturally and
    never competes with Core-0 tasks (espNowTask, uartTask) for SD access.

14. SD-logged raw data is NEVER deleted, purged, or truncated as a result
    of sync failures or any error state. /sync.bin stores only a forward
    pointer (lastSyncedEpoch) and is written exclusively by sdLoggingTask.
    In Error state (syncStatus=2) the raw .dat log files remain fully
    intact. When cloud connectivity recovers, the backfill loop in
    sdLoggingTask resumes automatically from lastSyncedEpoch+1 without
    any manual intervention or data loss.
