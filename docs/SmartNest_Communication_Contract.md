# SmartNest Communication Contract

Version: current firmware state in this repository

This document describes the communication interfaces used by the SmartNest system:

- SmartNest ESP32: WiFi, MQTT, Serial Monitor command interface, UART link to Master.
- Master ESP32: UART bridge, ESP-NOW controller, SD logging, state recovery.
- Digital Board ESP32: relay 7, manual switch, ACS current sensing.
- PZEM ESP32: voltage/current/power/energy sensing.

Default MQTT base topic is `smartnest`, but it can be changed from the SmartNest Serial Monitor with `MQTT SET TOPIC <baseTopic>`. In this document, `<base>` means the configured MQTT base topic.

## Device Roles

| Device | Main responsibility | Communication |
|---|---|---|
| SmartNest ESP32 | WiFi, MQTT, local relay 1-6, Serial Monitor commands, UART link to Master | MQTT, UART2, Serial Monitor |
| Master ESP32 | ESP-NOW coordinator, SD logging, energy calculation, relay 7 recovery state | UART2, ESP-NOW, SD |
| Digital Board ESP32 | Relay 7, digital manual switch, ACS current sensing, overcurrent lock | ESP-NOW |
| PZEM ESP32 | PZEM-004T telemetry and energy reset/reboot command handling | ESP-NOW |

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
};
```

| Field | Type | Meaning |
|---|---:|---|
| `type` | `uint8_t` | Packet type: `0x10` telemetry, `0x11` ACK reply |
| `rmsCurrent` | `float` | RMS current in amperes. Telemetry packet sends `0.0` when relay is OFF |
| `relayState` | `uint8_t` | `1` relay ON, `0` relay OFF |
| `switchState` | `uint8_t` | `1` manual switch active/LOW, `0` inactive |

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
| `0x02` | `relay_on` | Turns relay ON unless `masterLock` or `overcurrentLock` is active |
| `0x03` | `relay_off` | Turns relay OFF |
| `0x04` | `masterLock ON` | Sets lock ON and turns relay OFF |
| `0x05` | `masterLock OFF` | Clears lock |
| `0x06` | `reboot` | Restarts the Digital Board ESP32 |

Protection behavior:

| Condition | Behavior |
|---|---|
| `currentAmperes >= 6.0A` | `overcurrentLock = true`, relay forced OFF |
| Overcurrent recovery | Lock clears after current stays below `5.5A` for 3 checks, about 600 ms |
| Relay ON command while locked | Ignored |

### Digital Board RGB LED Codes

Priority order is top to bottom.

| Color | Pattern | Process / State |
|---|---|---|
| White | Blink every 500 ms | Boot / no ACK received from Master yet |
| Red | Blink every 300 ms | Overcurrent lock active |
| Cyan | Solid | Master lock active |
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
};
```

| Field | Type | Meaning |
|---|---:|---|
| `type` | `uint8_t` | Packet type: `0x20` telemetry, `0x21` ACK reply |
| `voltage` | `float` | PZEM voltage |
| `current` | `float` | PZEM current |
| `power` | `float` | PZEM power |
| `energy` | `float` | PZEM energy register value |

Send timing:

| Event | Packet type |
|---|---|
| Every 15 seconds | `0x20` |
| Master ACK ping received | `0x21` |
| Energy reset success | `0x20` |

Sensor health behavior:

If any PZEM reading returns `NaN`, the PZEM board marks `pzemHealthy = false`, logs a warning, replaces invalid values with `0.0`, and still sends a packet so the Master knows the board is alive.

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
| `{"t":"hist_ack","batch":1,"upto":1710000000}` | Acknowledge historical batch upload |
| `{"t":"files_req"}` | Request SD log file list |
| `{"t":"read_req","file":"2026_06_19.dat","chunk":0}` | Read 10 SD log records from file chunk |
| `{"t":"factory_reset"}` | Reset saved energy/sync state on Master SD |
| `{"t":"clear_logs"}` | Delete SD log files |

State recovery note:

The Master stores recoverable control state in NVS before applying it to the Digital Board:

- `masterLock`
- Digital Board desired relay state
- Digital Board desired lock state

One-shot commands such as `reboot` and `energy_reset` are not persisted for replay.

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
  "d_wh": 10.0,
  "m_wh": 120.0,
  "l_wh": 500.0,
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
| `pi` | PZEM current; may be `null` |
| `pp` | PZEM power; may be `null` |
| `d_wh` | Daily energy in Wh |
| `m_wh` | Monthly energy in Wh |
| `l_wh` | Lifetime energy in Wh |
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

#### Slave Online/Offline Events

```json
{"t":"on","dev":"d1"}
{"t":"off","dev":"pzem"}
```

| Key | Values |
|---|---|
| `dev` | `d1`, `pzem` |

#### Historical Batch: `t = "hist"`

Master sends this when cloud is online and SD has unsynced records.

```json
{
  "t": "hist",
  "batch": 1,
  "recs": [
    {
      "epoch": 1710000000,
      "v": 230.0,
      "load": 1.25,
      "pi": 0.120,
      "powerVA": 287.5
    }
  ]
}
```

SmartNest publishes each record to MQTT and then replies:

```json
{"t":"hist_ack","batch":1,"upto":1710000000}
```

#### SD File List Response: `t = "files_res"`

```json
{"t":"files_res","files":["2026_06_19.dat","2026_06_20.dat"]}
```

#### SD File Read Response: `t = "read_res"`

```json
{
  "t": "read_res",
  "file": "2026_06_19.dat",
  "chunk": 0,
  "total": 25,
  "recs": [
    {
      "epoch": 1710000000,
      "v": 230.0,
      "load": 1.25,
      "pi": 0.120,
      "pva": 287.5
    }
  ]
}
```

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
| `SHUTDOWN ON` | Master shutdown ON: local relays OFF, relay 7 OFF and locked |
| `SHUTDOWN OFF` | Master shutdown OFF: relay 7 unlocked |
| `MASTERLOCK ON` | Global master lock ON |
| `MASTERLOCK OFF` | Global master lock OFF |
| `SLAVE D1 reboot` | Reboot Digital Board ESP32 |
| `SLAVE PZEM reboot` | Reboot PZEM ESP32 |
| `SLAVE PZEM energy_reset` | Reset PZEM energy |
| `LOGS LIST` | Request Master SD log file list |
| `LOGS VIEW <file> <chunk>` | Request 10 records from SD log file |
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
| `MQTT SET CLIENT <clientId>` | Set MQTT client ID |
| `MQTT SET USER <username>` | Set MQTT username |
| `MQTT SET PASS <password>` | Set MQTT password |
| `MQTT SET TOPIC <baseTopic>` | Set MQTT base topic |
| `MQTT SET KEEPALIVE <seconds>` | Set MQTT keepalive |
| `MQTT RESET` | Reset MQTT config to defaults |

### SmartNest `STATUS` JSON

```json
{
  "relays": [false, false, false, false, false, false],
  "locks": [false, false, false, false, false, false],
  "shutdown": false,
  "current": 0.00,
  "rssi": -55,
  "ssid": "WiFiName",
  "acs": 0.00,
  "load": 0.00,
  "v": 230.0,
  "pw": 0.0,
  "daily": 0.000,
  "monthly": 0.000,
  "lifetime": 0.000,
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
  "mqtt_status": 2,
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
| Base topic | `smartnest` |

### MQTT Topics Published By SmartNest

#### Retained State Topics

Published after MQTT connect and after relevant commands.

| Topic | Payload | Retained | Meaning |
|---|---|---:|---|
| `<base>/relay/0/state` to `<base>/relay/5/state` | `true` / `false` | yes | SmartNest local relay state |
| `<base>/relay/6/state` | `true` / `false` | yes | Digital Board relay 7 state |
| `<base>/relay/0/locked` to `<base>/relay/5/locked` | `true` / `false` | yes | SmartNest local relay lock |
| `<base>/relay/6/locked` | `true` / `false` | yes | Digital Board relay lock |
| `<base>/shutdown` | `true` / `false` | yes | Master shutdown state |
| `<base>/slave/d1/online` | `true` / `false` | yes | Digital Board online |
| `<base>/slave/pzem/online` | `true` / `false` | yes | PZEM board online |

#### Live Telemetry Topics

Published on telemetry updates and approximately every `30` seconds while MQTT is connected.

| Topic | Payload example | Retained | Meaning |
|---|---|---:|---|
| `<base>/sensor/voltage` | `230.4` | no | PZEM voltage |
| `<base>/sensor/acs` | `0.52` | no | Digital Board ACS current |
| `<base>/sensor/load` | `1.25` | no | SmartNest local combined ACS/load current |
| `<base>/sensor/power` | `287.5` | no | PZEM power |
| `<base>/energy/daily` | `0.125` | no | Daily energy in kWh |
| `<base>/energy/monthly` | `4.250` | no | Monthly energy in kWh |
| `<base>/energy/lifetime` | `18.905` | no | Lifetime energy in kWh |
| `<base>/switch/6/state` | `true` | no | Digital Board manual switch state |
| `<base>/status` | JSON | no | Heartbeat/status |

`<base>/status` payload:

```json
{
  "uptime": 123456,
  "ssid": "WiFiName",
  "rssi": -55
}
```

#### Historical Data Topic

Current code publishes historical records to a hardcoded topic:

| Topic | Payload | Retained | Note |
|---|---|---:|---|
| `smartnest/history` | JSON record | no | This currently does not use configurable `<base>` |

Payload:

```json
{
  "epoch": 1710000000,
  "v": 230.0,
  "load": 1.25,
  "pi": 0.120,
  "powerVA": 287.5
}
```

### MQTT Command Topics Subscribed By SmartNest

SmartNest subscribes to these topics after MQTT connects.

| Topic | Payload | Action |
|---|---|---|
| `<base>/relay/0/set` to `<base>/relay/5/set` | `true` / `false` | Set local relay ON/OFF |
| `<base>/relay/6/set` | `true` / `false` | Set Digital Board relay ON/OFF |
| `<base>/relay/0/lock` to `<base>/relay/5/lock` | `true` / `false` | Lock/unlock local relay |
| `<base>/relay/6/lock` | `true` / `false` | Lock/unlock Digital Board relay |
| `<base>/cmd/shutdown` | `true` / `false` | Master shutdown ON/OFF |
| `<base>/cmd/slave/d1` | `reboot` | Reboot Digital Board |
| `<base>/cmd/slave/pzem` | `reboot` | Reboot PZEM ESP32 |
| `<base>/cmd/slave/pzem` | `energy_reset` | Reset PZEM energy register |

### MQTT Command Payload Examples

Turn local relay 1 ON:

```text
Topic: smartnest/relay/0/set
Payload: true
```

Turn relay 7 OFF:

```text
Topic: smartnest/relay/6/set
Payload: false
```

Lock relay 7:

```text
Topic: smartnest/relay/6/lock
Payload: true
```

Enable shutdown:

```text
Topic: smartnest/cmd/shutdown
Payload: true
```

Reboot PZEM ESP32:

```text
Topic: smartnest/cmd/slave/pzem
Payload: reboot
```

Reset PZEM energy:

```text
Topic: smartnest/cmd/slave/pzem
Payload: energy_reset
```

### Notes For Cloud Implementation

- Payload matching is strict in the current firmware for MQTT commands. Use lowercase `true`, `false`, `reboot`, and `energy_reset`.
- MQTT relay indexes are zero-based. UI labels should translate relay 1-7 to MQTT indexes 0-6.
- Retained topics should be used by the cloud dashboard for initial state hydration.
- Live telemetry topics are not retained, so the cloud should store incoming values if historical graphs are needed.
- `smartnest/history` is currently hardcoded. If the MQTT base topic is changed, live topics move to the new base, but history remains on `smartnest/history` until firmware is updated.
- `MASTERLOCK` is available through Serial Monitor and UART (`{"t":"lock","val":true/false}`), but there is no MQTT topic for master lock in the current firmware.
- PZEM voltage/current/power may be `null` in Master UART telemetry when the PZEM board is offline/unhealthy. MQTT publish code converts current SmartNest state to strings, so cloud should also tolerate `0.0` values.

