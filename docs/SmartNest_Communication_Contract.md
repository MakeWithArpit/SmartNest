# SmartNest Communication Contract

Version: current firmware state in this repository

This document describes the communication interfaces used by the SmartNest system:

- SmartNest ESP32: WiFi, MQTT, Serial Monitor command interface, UART link to Master, 6 relays.
- Master ESP32: UART bridge, ESP-NOW controller, SD logging, state recovery, 6 manual switch.
- Digital Board ESP32: relay, manual switch, ACS current sensing.
- PZEM ESP32: voltage/current/power/energy sensing.

Default MQTT base topic is `smartnest/SmartNest_001`. The base topic and MQTT client ID are firmware-fixed and read-only; the dashboard and `/api/mqtt` report them but do not allow changing them. `MQTT SET TOPIC <baseTopic>` and `MQTT SET CLIENT <clientId>` are rejected. In this document, `<base>` means the fixed MQTT base topic.

## Device Roles

| Device | Main responsibility | Communication |
|---|---|---|
| SmartNest ESP32 | WiFi, MQTT, local relay 1-6, Serial Monitor commands, UART link to Master | MQTT, UART2, Serial Monitor |
| Master ESP32 | ESP-NOW coordinator, SD logging, energy calculation, relay 7 telemetry bridge | UART2, ESP-NOW, SD |
| Digital Board ESP32 | Relay 7, digital manual switch, ACS current sensing, overcurrent lock | ESP-NOW |
| PZEM ESP32 | PZEM-004T telemetry and energy reset/reboot command handling | ESP-NOW |

Master time-source priority:

1. NTP epoch received from SmartNest over UART.
2. DS3231 RTC over I2C when NTP is not available.
3. Soft time from `millis()` after a valid NTP/RTC time was already established.
4. SD-restored estimated time only as a last resort.

The Master syncs DS3231 whenever valid NTP is received. DS3231 uses the ESP32 default I2C pins: SDA `GPIO 21`, SCL `GPIO 22`.

SmartNest ESP32 local sensors:

| Sensor | Pin | Data |
|---|---:|---|
| DHT11 | GPIO 23 / D23 | Temperature in Celsius and relative humidity |

## Relay Numbering

Serial Monitor numbering:

| User relay number | Device |
|---|---|
| `1` to `6` | SmartNest local relays |
| `7` | Digital Board relay |

MQTT numbering:

| MQTT relay index | Device |
|---|---|
| `0` to `5` | SmartNest local relays |
| `6` | Digital Board relay 7 |

## Digital Board ESP32

### ESP-NOW Data Sent To Master

The Digital Board sends a binary `DigitalSlavePacket`.

```cpp
struct DigitalSlavePacket {
  uint8_t type;
  float rmsCurrent;
  uint8_t relayState;
  uint8_t switchState;
  uint8_t lockState;
};
```

For command results, the Digital Board sends `DigitalCmdAckPacket`.

```cpp
struct DigitalCmdAckPacket {
  uint8_t type;                 // 0x12
  uint8_t cmdType;
  uint8_t accepted;
  uint8_t reason;
  uint8_t relayState;
  uint8_t relayLockState;
  uint8_t masterLockState;
  uint8_t overcurrentLockState;
};
```

| Field | Type | Meaning |
|---|---:|---|
| `type` | `uint8_t` | Packet type: `0x10` telemetry, `0x11` ACK reply |
| `rmsCurrent` | `float` | RMS current in amperes. Telemetry packet sends `0.0` when relay is OFF |
| `relayState` | `uint8_t` | `1` relay ON, `0` relay OFF |
| `switchState` | `uint8_t` | `1` manual switch active/LOW, `0` inactive |
| `lockState` | `uint8_t` | `1` relay/master lock active, `0` unlocked |

Send timing:

| Event | Packet type |
|---|---|
| Relay state changes | `0x10` |
| Every 30 seconds while connected | `0x10` |
| Master ACK ping received | `0x11` |

### ESP-NOW Commands Received From Master

The Digital Board receives a binary `CmdPacket`.

```cpp
struct CmdPacket {
  uint8_t type;
};
```

| Command type | Command | Behavior |
|---:|---|---|
| `0x01` | ACK ping | Updates master contact time and sends `DigitalSlavePacket` type `0x11` |
| `0x02` | `relay_on` | Turns relay ON unless `relayLock`, `masterLock`, or `overcurrentLock` is active |
| `0x03` | `relay_off` | Turns relay OFF |
| `0x04` | `relay_lock` | Locks Digital Board relay and turns it OFF |
| `0x05` | `relay_unlock` | Clears Digital Board relay lock |
| `0x06` | `reboot` | Restarts the Digital Board ESP32 |
| `0x08` | `masterLock ON` | Sets master lock ON and turns relay OFF |
| `0x09` | `masterLock OFF` | Clears master lock |

Protection behavior:

| Condition | Behavior |
|---|---|
| `currentAmperes >= 6.0A` | `overcurrentLock = true`, relay forced OFF |
| Overcurrent recovery | Lock clears after current stays below `5.5A` for 3 checks, about 600 ms |
| Relay ON command while locked | Ignored |
| Relay/lock/master-lock recovery | Digital Board stores its own relay, relay-lock, and master-lock state in local NVS memory |
| Command acknowledgement | Digital Board sends command result packet type `0x12` to Master after relay/lock/master-lock/reboot commands |

### Digital Board RGB LED Codes

Priority order is top to bottom.

| Color | Pattern | Process / State |
|---|---|---|
| White | Blink every 500 ms | Boot / no ACK received from Master yet |
| Red | Blink every 300 ms | Overcurrent lock active |
| Cyan | Solid | Master lock active |
| Yellow | Solid | Relay lock active |
| Green | Solid | Relay ON |
| Magenta | Blink every 1 second | No Master contact for more than 60 seconds |
| Blue | Solid | Normal connected state, relay OFF |

## PZEM ESP32

### ESP-NOW Data Sent To Master

The PZEM board sends a binary `PzemSlavePacket`.

```cpp
struct PzemSlavePacket {
  uint8_t type;
  float voltage;
  float current;
  float power;
  float energy;
  uint8_t healthy;
};
```

| Field | Type | Meaning |
|---|---:|---|
| `type` | `uint8_t` | Packet type: `0x20` telemetry, `0x21` ACK reply |
| `voltage` | `float` | PZEM voltage |
| `current` | `float` | PZEM current |
| `power` | `float` | PZEM power |
| `energy` | `float` | PZEM energy register value |
| `healthy` | `uint8_t` | `1` when all PZEM reads were valid, `0` after any read failure/NaN |

Send timing:

| Event | Packet type |
|---|---|
| Every 15 seconds | `0x20` |
| Master ACK ping received | `0x21` |
| Energy reset success | `0x20` |

Sensor health behavior:

If any PZEM reading returns `NaN`, the PZEM board marks `pzemHealthy = false`, logs a warning, sets `healthy = 0`, replaces invalid values with `0.0`, and still sends a packet so the Master knows the board is alive. Master treats the packet as unhealthy unless `healthy = 1` and voltage is in the valid 80V-280V range.

### ESP-NOW Commands Received From Master

The PZEM board receives the same binary `CmdPacket`.

| Command type | Command | Behavior |
|---:|---|---|
| `0x01` | ACK ping | Updates Master contact state and sends `PzemSlavePacket` type `0x21` |
| `0x06` | `reboot` | Schedules ESP restart after 1 second |
| `0x07` | `energy_reset` | Calls `pzem.resetEnergy()` and sends fresh telemetry if successful |

### PZEM RGB LED Codes

Priority order is top to bottom.

| Color | Pattern | Process / State |
|---|---|---|
| Red | Solid | Critical error: ESP-NOW not initialized, Master timeout over 60 seconds, or PZEM sensor unhealthy |
| Yellow | Solid for 1 second | A command/packet was received from Master |
| Blue | Solid | Boot / initialization / waiting for first Master contact |
| Green | Solid | Healthy state after first Master contact |

## Master ESP32 UART JSON Contract

SmartNest and Master communicate over UART2 at `115200` baud.

Master UART pins:

| Pin | Function |
|---|---|
| GPIO 16 | RX |
| GPIO 17 | TX |

### JSON Received By Master From SmartNest

| JSON | Purpose |
|---|---|
| `{"t":"cmd","tgt":"d1","cmd":"relay_on"}` | Request Digital Board relay ON |
| `{"t":"cmd","tgt":"d1","cmd":"relay_off"}` | Request Digital Board relay OFF |
| `{"t":"cmd","tgt":"d1","cmd":"relay_lock"}` | Lock Digital Board relay |
| `{"t":"cmd","tgt":"d1","cmd":"relay_unlock"}` | Unlock Digital Board relay |
| `{"t":"cmd","tgt":"d1","cmd":"reboot"}` | Reboot Digital Board |
| `{"t":"cmd","tgt":"pzem","cmd":"reboot"}` | Reboot PZEM ESP32 |
| `{"t":"cmd","tgt":"pzem","cmd":"energy_reset"}` | Reset PZEM energy register |
| `{"t":"lock","val":true}` | Master lock ON |
| `{"t":"lock","val":false}` | Master lock OFF |
| `{"t":"ntp","epoch":1710000000,"tz_h":5,"tz_m":30}` | Time sync |
| `{"t":"acs","i":1.23}` | SmartNest local ACS/load current |
| `{"t":"cloud","up":true}` | MQTT/cloud online status |
| `{"t":"files_req"}` | Request SD log file list |
| `{"t":"read_req","file":"energy_log.csv","chunk":0}` | Read 10 energy-log CSV rows from file chunk |
| `{"t":"hist_req","after":41,"limit":6}` | Request unsent history records after record id |
| `{"t":"hist_ack","last":51}` | Mark cloud-uploaded history records through id as synced |
| `{"t":"factory_reset"}` | Reset saved energy state on Master SD |
| `{"t":"clear_logs"}` | Delete SD log files |

State recovery note:

Recoverable control state is owned by the ESP that executes the command:

- Digital Board relay state, relay lock, and master lock are stored in the Digital Board ESP32 local NVS memory.
- SmartNest local relay and lock state stays on the SmartNest ESP32.
- Master ESP32 does not store relay/lock/master-lock commands on SD card or replay them from SD.

One-shot commands such as `reboot` and `energy_reset` are not persisted for replay.

Current-sensor ownership:

- SmartNest ESP32 ACS712 measures only the total current drawn by local relay 1-6 loads. SmartNest sends this value to Master as `{"t":"acs","i":1.23}`.
- Digital Board ACS712 measures only the Digital Board relay 7 load. It is sent to Master as Digital Board telemetry `rmsCurrent`.
- Master keeps both readings separate and integrates main-board and digital-board energy with PZEM voltage as the common voltage reference.

### JSON Sent By Master To SmartNest

#### Telemetry: `t = "tel"`

Sent every 10 seconds.

```json
{
  "t": "tel",
  "acs": 0.00,
  "v": 230.0,
  "pi": 0.123,
  "pp": 28.3,
  "pe": 1.235,
  "pe_raw": 101.235,
  "pe_start": 100.000,
  "tsrc": "NTP",
  "d_on": true,
  "p_on": true,
  "p_health": true,
  "d_rssi": -55,
  "p_rssi": -60,
  "d_relay": 1,
  "d_sw": 0,
  "d_lock": false,
  "sd_ok": true,
  "sd_total": 3965190144,
  "sd_used": 1048576,
  "m_lock": false
}
```

| Key | Meaning |
|---|---|
| `t` | Message type: `tel` |
| `acs` | Digital Board ACS current in amperes |
| `v` | PZEM voltage; may be `null` if PZEM unhealthy/offline |
| `pi` | Air-conditioner current from PZEM; may be `null` |
| `pp` | Air-conditioner power from PZEM; may be `null` |
| `pe` | Air-conditioner daily energy in kWh, calculated in software as PZEM cumulative energy minus AC day-start energy; may be `null` |
| `pe_raw` | Raw PZEM cumulative energy register in kWh for debug/backend use |
| `pe_start` | PZEM cumulative energy captured as the current day's AC baseline |
| `tsrc` | Master time source: `NTP`, `RTC`, `SOFT`, `ESTIMATED`, or `NONE` |
| `d_on` | Digital Board online |
| `p_on` | PZEM board online |
| `p_health` | PZEM sensor health |
| `d_rssi` | Digital Board ESP-NOW RSSI |
| `p_rssi` | PZEM ESP-NOW RSSI |
| `d_relay` | Digital Board relay state: `1` ON, `0` OFF |
| `d_sw` | Digital Board switch state: `1` active, `0` inactive |
| `d_lock` | Digital Board lock state |
| `sd_ok` | Master SD card present/working |
| `sd_total` | SD total bytes |
| `sd_used` | SD used bytes |
| `m_lock` | Master lock state |

#### Manual Switch Event: `t = "sw"`

```json
{"t":"sw","idx":0,"s":1}
```

| Key | Meaning |
|---|---|
| `idx` | SmartNest local relay index `0` to `5` |
| `s` | Switch state: `1` ON, `0` OFF |

#### Lock Acknowledgement: `t = "lock_ack"`

```json
{"t":"lock_ack","val":true}
```

#### Digital Command Acknowledgement: `t = "cmd_ack"`

```json
{"t":"cmd_ack","tgt":"d1","cmd_type":2,"ok":false,"reason":1,"relay":0,"relay_lock":1,"master_lock":0,"oc_lock":0}
```

| Key | Meaning |
|---|---|
| `cmd_type` | ESP-NOW command type sent to Digital Board |
| `ok` | `true` if command was accepted |
| `reason` | `0` none, `1` relay lock, `2` master lock, `3` overcurrent lock |
| `relay` | Digital Board relay state after command |
| `relay_lock` | Digital Board relay lock state |
| `master_lock` | Digital Board master lock state |
| `oc_lock` | Digital Board overcurrent lock state |

#### Slave Online/Offline Events

```json
{"t":"on","dev":"d1"}
{"t":"off","dev":"pzem"}
```

| Key | Values |
|---|---|
| `dev` | `d1`, `pzem` |

#### SD File List Response: `t = "files_res"`

Master stores one CSV energy log under SD folder `/SmartNestLogs`.

```json
{"t":"files_res","files":["energy_log.csv"]}
```

#### SD File Read Response: `t = "read_res"`

```json
{
  "t": "read_res",
  "file": "energy_log.csv",
  "chunk": 0,
  "lines": [
    "1710000000,2024-03-09 21:30:00,230.1,1.250,287.63,0.120400,0.520,119.65,0.040150,12,0,44,0,0,0,18"
  ]
}
```

The CSV header is:

```csv
epoch,date,voltage,main_current,main_power_w,main_energy_kwh,digital_current,digital_power_w,digital_energy_kwh,r1_on_s,r2_on_s,r3_on_s,r4_on_s,r5_on_s,r6_on_s,r7_on_s
```

New rows use this expanded header:

```csv
record_id,epoch,date,voltage,main_current,main_power_w,main_energy_kwh,digital_current,digital_power_w,digital_energy_kwh,ac_current,ac_power_w,ac_energy_kwh,r1_on_s,r2_on_s,r3_on_s,r4_on_s,r5_on_s,r6_on_s,r7_on_s
```

Logging rules:

| Field | Meaning |
|---|---|
| `epoch` | UTC timestamp |
| `date` | Local date/time string |
| `main_current` | SmartNest local relay 1-6 ACS current |
| `main_power_w` | Main board calculated power: PZEM voltage x main current |
| `main_energy_kwh` | Integrated main board energy |
| `digital_current` | Digital Board relay 7 ACS current |
| `digital_power_w` | Digital board calculated power: PZEM voltage x digital current |
| `digital_energy_kwh` | Integrated digital board energy |
| `r1_on_s` to `r7_on_s` | Relay ON runtime counters in seconds |
| `record_id` | Monotonic SD record id used by MQTT history sync |
| `ac_current`, `ac_power_w`, `ac_energy_kwh` | PZEM air-conditioner current, power, and daily AC energy. Daily AC energy is calculated from the raw cumulative PZEM register; the firmware does not reset the PZEM register automatically at day change |

Master updates energy every second and writes SD snapshots every 10 seconds when there is measured main, digital, or AC usage, or accumulated daily energy. PZEM voltage is used as the common voltage reference. On local day change, Master queues a final previous-day row to SD before clearing daily main/digital energy, relay runtimes, and moving the AC day-start baseline to the latest raw PZEM cumulative value.

## SmartNest Serial Monitor Commands

Serial baud: `115200`.

| Command | Purpose |
|---|---|
| `HELP` | Print command list |
| `STATUS` | Print complete SmartNest status JSON |
| `RELAY <1-7> ON` | Turn relay ON |
| `RELAY <1-7> OFF` | Turn relay OFF |
| `RELAY <1-7> TOGGLE` | Toggle relay |
| `LOCK <1-7> ON` | Lock relay |
| `LOCK <1-7> OFF` | Unlock relay |
| `MASTERLOCK ON` | Global master lock ON |
| `MASTERLOCK OFF` | Global master lock OFF |
| `SLAVE D1 reboot` | Reboot Digital Board ESP32 |
| `SLAVE PZEM reboot` | Reboot PZEM ESP32 |
| `SLAVE PZEM energy_reset` | Reset PZEM energy |
| `SD INFO` | Print latest SD status, total bytes, used bytes, free bytes, and used percent |
| `LOGS LIST` | Request Master SD log file list |
| `LOGS VIEW energy_log.csv <chunk>` | Request 10 rows from SD energy log |
| `RESET WIFI` | Clear SmartNest WiFi credentials and restart |
| `RESET MQTT` | Reset MQTT configuration |
| `RESET ENERGY` | Request Master energy reset |
| `RESET LOGS` | Request Master SD log deletion |
| `RESET FULL` | Master factory reset + clear logs + SmartNest WiFi/MQTT reset |
| `MQTT SHOW` | Print current MQTT config |
| `MQTT ENABLE ON` | Enable MQTT |
| `MQTT ENABLE OFF` | Disable MQTT |
| `MQTT SET BROKER <host>` | Set MQTT broker hostname/IP |
| `MQTT SET PORT <port>` | Set MQTT broker port |
| `MQTT SET CLIENT <clientId>` | Rejected: MQTT client ID is fixed/read-only |
| `MQTT SET USER <username>` | Set MQTT username |
| `MQTT SET PASS <password>` | Set MQTT password |
| `MQTT SET TOPIC <baseTopic>` | Rejected: MQTT base topic is fixed/read-only |
| `MQTT SET KEEPALIVE <seconds>` | Set MQTT keepalive |
| `MQTT RESET` | Reset MQTT config to defaults |

## SmartNest Local Dashboard

When SmartNest is connected to Wi-Fi, it starts a minimal authenticated dashboard on port `80`.

Access:

```text
http://smart-nest.local/
```

If mDNS is unavailable, use the IP printed by provisioning or shown in router DHCP clients.

Dashboard routes:

| Route | Purpose |
|---|---|
| `/` | Dashboard UI |
| `/api/status` | Current SmartNest status JSON |
| `/api/command` | POST `cmd=<serial command>` to run the same command path as Serial Monitor |
| `/api/sd` | Current SD status text |
| `/api/mqtt` | GET current MQTT config, POST updated MQTT config |
| `/api/logs/list` | Request Master SD log file list |
| `/api/logs/clear` | Request Master SD current log clear |
| `/api/logs/energy.csv` | Download `energy_log.csv` through Master UART chunk reads |

The CSV download fetches `energy_log.csv` from the Master SD card in 10-row chunks. The firmware caps one download at `100` chunks to avoid large RAM allocation on the SmartNest ESP32.

Dashboard login uses a short-lived session token stored in browser `sessionStorage`. Closing the tab removes the browser-side token, and the dashboard also sends a logout request during page unload.

### SmartNest `STATUS` JSON

```json
{
  "relays": [false, false, false, false, false, false],
  "locks": [false, false, false, false, false, false],
  "current": 0.00,
  "rssi": -55,
  "ssid": "WiFiName",
  "acs": 0.00,
  "load": 0.00,
  "voltage": 230.0,
  "energy_voltage": 230.0,
  "voltage_estimated": false,
  "ac_current": 0.123,
  "ac_power": 28.3,
  "ac_energy": 1.235,
  "pzem_energy_cumulative": 101.235,
  "ac_day_start_energy": 100.000,
  "time_source": "NTP",
  "main_energy": 0.120,
  "digital_energy": 0.040,
  "relay_runtime": [12, 0, 44, 0, 0, 0, 18],
  "d_on": true,
  "p_on": true,
  "d_rssi": -55,
  "p_rssi": -60,
  "d_relay": false,
  "d_lock": false,
  "d_sw": false,
  "m_lock": false,
  "sd_ok": true,
  "sd_total": 3965190144,
  "sd_used": 1048576,
  "p_health": true,
  "temp_c": 28.4,
  "humidity": 61.0,
  "dht_ok": true,
  "mqtt_status": 2,
  "reset_reason": "POWERON",
  "uptime": 123456,
  "time": "2026-06-19 22:30:00"
}
```

`mqtt_status` values:

| Value | Meaning |
|---:|---|
| `0` | Disabled |
| `1` | Connecting |
| `2` | Connected |
| `3` | Error |

## MQTT Contract For Cloud

Default connection:

| Setting | Default |
|---|---|
| Broker | `broker.hivemq.com` |
| Port | `1883` |
| Client ID | `SmartNest_001` |
| Username | empty |
| Password | empty |
| Keepalive | `60` seconds |
| Base topic | `smartnest/SmartNest_001` |

Client ID and base topic are read-only firmware identity fields.

Readonly topic map:

Publish:

- `<base>/live/sensors`
- `<base>/live/relays`
- `<base>/live/status`
- `<base>/history/batch`
- `<base>/cmd/ack`

Subscribe:

- `<base>/cmd/request`
- `<base>/history/ack`

### MQTT Topics Published By SmartNest

The MQTT contract is now split into three sections:

- `live`: current readings and state, no ACK required.
- `cmd`: cloud commands and SmartNest command acknowledgements.
- `history`: SD-backed records intended for cloud database storage.

Older relay/sensor/slave compatibility topics are removed. Use only the JSON live, command, and history topics below.

#### Live Topics

| Topic | Payload | Retained | Meaning |
|---|---|---:|---|
| `<base>/live/status` | JSON | no | Uptime, WiFi, MQTT, SD, slave, and sensor health |
| `<base>/live/sensors` | JSON | no | Voltage, currents, power, energy, temperature, humidity |
| `<base>/live/relays` | JSON | no | Relay states, locks, master lock, digital switch, runtimes |

`<base>/live/sensors` payload:

```json
{
  "voltage": 230.1,
  "energy_voltage": 230.1,
  "main_current": 1.25,
  "digital_current": 0.52,
  "ac_current": 1.234,
  "ac_power": 287.5,
  "ac_energy_kwh": 18.905,
  "pzem_cumulative_energy_kwh": 101.235,
  "ac_day_start_kwh": 100.000,
  "main_energy_kwh": 0.120,
  "digital_energy_kwh": 0.040,
  "temperature_c": 28.4,
  "humidity_pct": 61.0
}
```

`<base>/live/status` payload:

```json
{
  "uptime": 123456,
  "ssid": "WiFiName",
  "rssi": -55,
  "mqtt_status": 2,
  "sd_ok": true,
  "sd_total": 3965190144,
  "sd_used": 1048576,
  "digital_online": true,
  "pzem_online": true,
  "pzem_health": true,
  "dht_ok": true,
  "voltage_estimated": false,
  "time_source": "NTP",
  "reset_reason": "POWERON"
}
```

#### Command Topics

| Topic | Payload | Direction | Meaning |
|---|---|---|---|
| `<base>/cmd/request` | JSON | Cloud -> SmartNest | Cloud command with `cmd_id` |
| `<base>/cmd/ack` | JSON | SmartNest -> Cloud | Result for a command |

Command request example:

```json
{"cmd_id":"abc123","type":"relay_set","relay":1,"state":true}
```

Supported command `type` values:

| Type | Required fields | Meaning |
|---|---|---|
| `relay_set` | `relay`, `state` | Set relay 1-7 ON/OFF |
| `relay_toggle` | `relay` | Toggle relay 1-7 |
| `relay_lock` | `relay`, `locked` | Lock/unlock relay 1-7 |
| `master_lock` | `state` | Set master lock |
| `slave_reboot` | `target` | Reboot `digital`, `d1`, or `pzem` |
| `pzem_energy_reset` | none | Reset PZEM energy register |

ACK example:

```json
{"cmd_id":"abc123","type":"relay_set","ok":true,"reason":"done","relay":1,"state":true,"locked":false}
```

Common failure reasons: `invalid_payload`, `invalid_relay`, `locked`, `master_locked`, `overcurrent_locked`, `busy`, `timeout`, `unsupported`.

#### History Topics

| Topic | Payload | Direction | Meaning |
|---|---|---|---|
| `<base>/history/batch` | JSON | SmartNest -> Cloud | SD-backed records for database storage |
| `<base>/history/ack` | JSON | Cloud -> SmartNest | Batch acknowledgement |

History batch example:

```json
{
  "batch_id": "SmartNest_001-51-123456",
  "device": "SmartNest_001",
  "records": [
    {
      "id": 51,
      "epoch": 1781971200,
      "date": "2026-06-20 22:10:00",
      "voltage": 230.1,
      "main_current": 1.250,
      "main_power_w": 287.63,
      "main_energy_kwh": 0.120400,
      "digital_current": 0.520,
      "digital_power_w": 119.65,
      "digital_energy_kwh": 0.040150,
      "ac_current": 1.234,
      "ac_power_w": 287.50,
      "ac_energy_kwh": 18.905000,
      "runtimes_sec": [12, 0, 44, 0, 0, 0, 18]
    }
  ]
}
```

Cloud ACK example:

```json
{"batch_id":"SmartNest_001-51-123456","ok":true,"last_id":51}
```

SmartNest requests history from Master over UART, publishes a batch, and only advances Master sync state after cloud ACK.

#### Removed Compatibility Topics

The firmware must not publish or subscribe old retained relay state, scalar sensor, slave online, switch state, status, or old command topics. Removed examples include `<base>/sensor/#`, `<base>/relay/#`, `<base>/slave/#`, `<base>/switch/6/state`, `<base>/status`, and `<base>/cmd/slave/#`.

### Notes For Cloud Implementation

- Use JSON command payloads on `<base>/cmd/request`; relay numbers are 1-7.
- Live telemetry topics are not retained, so the cloud should store incoming values if historical graphs are needed.
- `master_lock` is a JSON command type, not a separate MQTT topic.
- PZEM voltage/current/power may be `null` in Master UART telemetry when the PZEM board is offline/unhealthy. MQTT publish code converts current SmartNest state to strings, so cloud should also tolerate `0.0` values.
