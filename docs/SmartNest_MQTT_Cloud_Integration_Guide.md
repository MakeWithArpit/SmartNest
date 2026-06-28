# SmartNest MQTT & Cloud Integration Guide

This guide is prepared for cloud, backend, and MQTT integration developers. It details the MQTT protocol, message schemas, topic architecture, and communication workflows implemented in the SmartNest Internet ESP32 firmware.

> [!IMPORTANT]
> This document is strictly focused on MQTT interface interactions. It does not cover Serial Monitor, local provisioning menus, or internal UART/ESP-NOW communication channels.

---

## 1. SmartNest MQTT Overview

The SmartNest ESP32 utilizes MQTT as its primary cloud integration interface. This layer enables the following capabilities:
- **Live Sensor Telemetry**: Volatile real-time electrical metrics (voltage, current, power) and environmental data.
- **Relay State Publishing**: State updates, locks, and switch statuses for local and remote relays.
- **Device Status Publishing**: Overall system health metrics including network statistics, uptime, reset triggers, and storage status.
- **Slave Condition Publishing**: RSSI, communication age, and online states of slave boards.
- **Command Control**: Accepting remote request payloads to alter relay outputs or trigger reboots.
- **Command Acknowledgement**: A feedback loop confirming command success, failure, or validation errors.
- **History Batch Upload**: Buffer-safe upload of SD-card-stored hourly/daily energy logs to prevent data loss.
- **History Batch Acknowledgement**: An ACK flow allowing the Master ESP32 to safely prune sync tables after cloud persistence is confirmed.

---

## 2. MQTT Configuration

The MQTT client configuration uses the following defaults in the firmware:

| Setting | Default Value | Description |
| :--- | :--- | :--- |
| **MQTT Enabled** | `true` | Enables/disables the MQTT client task at boot. |
| **Broker** | `broker.hivemq.com` | HiveMQ public broker used for testing/development. |
| **Port** | `1883` | TCP port for non-encrypted MQTT communication. |
| **Client ID** | `SmartNest_001` | Monotonic identifier for the client connection. |
| **Username** | *(empty)* | Optional username parameter for basic authentication. |
| **Password** | *(empty)* | Optional password parameter for basic authentication. |
| **Keepalive** | `60` seconds | Connection health timeout (ping heartbeat interval). |
| **Base Topic** | `smartnest/SmartNest_001` | The parent namespace for all topics. |

> [!NOTE]
> The keepalive setting (60 seconds) determines how frequently the client sends a ping to the broker to check connection viability. It is independent of the telemetry update intervals.

---

## 3. Topic Map

The Base Topic pattern is `smartnest/<device_id>`. For the default firmware, this is `smartnest/SmartNest_001`.

| Topic | Direction | Purpose | Retain |
| :--- | :--- | :--- | :--- |
| `smartnest/SmartNest_001/live/sensors` | Publish | Real-time sensor metrics and calculated energy counts. | `false` |
| `smartnest/SmartNest_001/live/relays` | Publish | Current relay states, lock configurations, switch states, and runtimes. | `true` |
| `smartnest/SmartNest_001/live/status` | Publish | Device, network, MQTT, SD, slave, time, and data-quality status | `true` |
| `smartnest/SmartNest_001/live/slaves` | Publish | Connection state, age, and link quality of the PZEM and Digital slave boards. | `true` |
| `smartnest/SmartNest_001/cmd/ack` | Publish | Feedback confirming the execution status of incoming command requests. | `false` |
| `smartnest/SmartNest_001/history/batch` | Publish | SD-backed offline history energy record batches. | `false` |
| `smartnest/SmartNest_001/cmd/request` | Subscribe | Command listener topic for backend requests. | `N/A` |
| `smartnest/SmartNest_001/history/ack` | Subscribe | Acknowledge the successful recording of a historical batch. | `N/A` |

---

## 4. Published Topic: `/live/sensors`

**Topic Name**: `smartnest/SmartNest_001/live/sensors`
- **Publish Trigger**: Every 30 seconds (heartbeat) or whenever raw sensor metrics fluctuate beyond the configured threshold limits.

### Example Payload
```json
{
  "voltage": 230.1,
  "energy_voltage": 230.1,
  "main_current": 1.25,
  "digital_current": 0.50,
  "ac_current": 0.350,
  "ac_power": 80.5,
  "ac_energy_kwh": 1.234,
  "pzem_cumulative_energy_kwh": 10.500,
  "ac_day_start_kwh": 9.200,
  "main_energy_kwh": 2.345,
  "digital_energy_kwh": 0.567,
  "temperature_c": 30.1,
  "humidity_pct": 55.0
}
```

### Field Definitions
- `voltage` *(number)*: PZEM voltage reading in Volts. Returns `0.0` if the PZEM slave board is offline.
- `energy_voltage` *(number)*: The voltage variable utilized for power calculations. Defaults to `220.0` if `voltage_estimated` is active.
- `main_current` *(number)*: Local Current Sensor (ACS712) reading for relays 1–6 in Amperes.
- `digital_current` *(number)*: ACS Current reading of relay 7 (Digital Board slave) in Amperes.
- `ac_current` *(number)*: Current reading from the PZEM sensor in Amperes.
- `ac_power` *(number)*: Active power calculated by the PZEM sensor in Watts.
- `ac_energy_kwh` *(number)*: Daily cumulative energy reading in kWh (raw PZEM energy minus the current day's start baseline).
- `pzem_cumulative_energy_kwh` *(number)*: Raw cumulative energy reading recorded on the PZEM sensor registers.
- `ac_day_start_kwh` *(number)*: Raw PZEM energy baseline captured for the current day.
- `main_energy_kwh` *(number)*: Cumulative calculated energy consumed by local relays 1–6 in kWh.
- `digital_energy_kwh` *(number)*: Cumulative calculated energy consumed by relay 7 (Digital Board) in kWh.
- `temperature_c` *(number)*: Temperature recorded by the local DHT11 sensor in Celsius.
- `humidity_pct` *(number)*: Relative humidity recorded by the local DHT11 sensor in %.

---

## 5. Published Topic: `/live/relays`

**Topic Name**: `smartnest/SmartNest_001/live/relays`
- **Publish Trigger**: Instantly when any relay toggles state, a lock is applied, or during the periodic 30-second heartbeat.

### Example Payload
```json
{
  "states": [true, false, false, false, false, false, true],
  "locks": [false, false, false, false, false, false, false],
  "master_lock": false,
  "digital_switch": true,
  "runtime_sec": [120, 0, 0, 0, 0, 0, 45]
}
```

### Field Definitions
- `states` *(boolean array)*: States of all relays. Indexes `0` to `5` map to local relays 1–6. Index `6` maps to remote relay 7 (Digital Board).
- `locks` *(boolean array)*: Lock status of all relays. Index `0` to `5` are local relay locks, index `6` represents remote relay 7 lock status.
- `master_lock` *(boolean)*: True if all relays (1-6) and relay 7 locks are set to active.
- `digital_switch` *(boolean)*: Physical toggle switch state reported by the Digital Board.
- `runtime_sec` *(number array)*: Cumulative runtimes in seconds for each of the 7 relays.

---

## 6. Published Topic: `/live/status`

**Topic Name**: `smartnest/SmartNest_001/live/status`
- **Publish Trigger**: Sent every 30 seconds as part of the system health heartbeat.

### Example Payload
```json
{
  "uptime": 123456,
  "ssid": "MyWiFi",
  "rssi": -55,
  "mqtt_status": 2,
  "sd_ok": true,
  "sd_total": 15931539456,
  "sd_used": 1024000,
  "dht_ok": true,
  "voltage_estimated": false,
  "time_source": "NTP",
  "reset_reason": "POWERON"
}
```

### Field Definitions
- `uptime` *(number)*: System uptime of the Internet ESP32 in milliseconds.
- `ssid` *(string)*: SSID of the Wi-Fi network the board is connected to.
- `rssi` *(number)*: Wi-Fi RSSI level (signal strength in dBm).
- `mqtt_status` *(number)*: Client state code: `0` = Disabled, `1` = Connecting, `2` = Connected, `3` = Disconnected/Error.
- `sd_ok` *(boolean)*: Status of the SD card reader on the Master ESP32.
- `sd_total` *(number)*: Total capacity of the SD card in bytes.
- `sd_used` *(number)*: Used space on the SD card in bytes.
- `dht_ok` *(boolean)*: DHT11 sensor health status (`true` if reading successfully, `false` if returning `NAN`).
- `voltage_estimated` *(boolean)*: Indicates if the PZEM sensor was offline or invalid, forcing the master to use a default estimation (220V) for energy calculations.
- `time_source` *(string)*: Source of the time configuration. Options: `NTP`, `RTC`, `SOFT`, `ESTIMATED`, `NONE`.
- `reset_reason` *(string)*: ESP32 hardware reset reason.

---

## 7. Published Topic: `/live/slaves`

**Topic Name**: `smartnest/SmartNest_001/live/slaves`
- **Publish Trigger**: Immediately on any slave connection status transition, and periodically every 60 seconds.

### Example Payload
```json
{
  "digital_board": {
    "online": true,
    "rssi": -55,
    "last_seen_sec_ago": 2
  },
  "pzem": {
    "online": false,
    "rssi": -82,
    "last_seen_sec_ago": 65
  }
}
```

### Field Definitions
- `online` *(boolean)*: Connection status of the slave board (calculated via ESP-NOW ping heartbeats).
- `rssi` *(number)*: ESP-NOW signal strength in dBm recorded during the last transmission.
- `last_seen_sec_ago` *(number)*: Number of seconds elapsed since the last packet was successfully received from the slave board.

---

## 8. Published Topic: `/cmd/ack`

**Topic Name**: `smartnest/SmartNest_001/cmd/ack`
- **Publish Trigger**: Dispatched immediately in response to any request received on `/cmd/request`.

### Payload Format
```json
{
  "cmd_id": "test-relay1-on",
  "command": "relay_set",
  "ok": true,
  "message": "relay 1 set to ON",
  "timestamp": 1780000000
}
```

### Field Definitions
- `cmd_id` *(string)*: Echoes the unique ID from the command request payload. If none was provided, a fallback `cmd-<millis>` is populated.
- `command` *(string)*: Echoes the requested command name.
- `ok` *(boolean)*: Operation result (`true` for success, `false` for failure).
- `message` *(string)*: Human-readable error/status details explaining the result.
- `timestamp` *(number)*: Unix epoch timestamp in seconds. Returns `0` if the device has not yet synchronized time.

---

### Command ACK Patterns

#### 1. Success ACK
```json
{
  "cmd_id": "test-relay1-on",
  "command": "relay_set",
  "ok": true,
  "message": "relay 1 set to ON",
  "timestamp": 1780000000
}
```

#### 2. Validation Failure ACK
```json
{
  "cmd_id": "bad-relay",
  "command": "relay_set",
  "ok": false,
  "message": "invalid relay number",
  "timestamp": 1780000000
}
```

#### 3. Locked Relay ACK
```json
{
  "cmd_id": "locked-r1",
  "command": "relay_set",
  "ok": false,
  "message": "relay 1 is locked",
  "timestamp": 1780000000
}
```



---

## 9. Subscribed Topic: `/cmd/request`

**Topic Name**: `smartnest/SmartNest_001/cmd/request`
- **Subscribed by**: SmartNest ESP32

To dispatch a command, publish a JSON object to this topic matching the generic schema below:
```json
{
  "cmd_id": "unique-command-id",
  "command": "command_name"
}
```

### Valid Command Whitelist
- `relay_set`: Turn a relay ON or OFF.
- `relay_lock`: Lock or unlock a relay state.
- `lights_set`: Set state for the lighting group (relays 1–5).
- `all_relays_off`: Switch off all relays (1–7).
- `unlock_all_relays`: Unlock lock constraints for all relays (1–7).
- `system_reboot`: Trigger an ESP32 hardware restart command sequence.
- `ac_set`: Control the air conditioner — power, exact temperature, temperature step, or fan speed.

---

## 10. MQTT Command Examples

### `relay_set`
Controls state of a specific relay (1–7).
- **Publish Topic**: `smartnest/SmartNest_001/cmd/request`

#### Example Payload (Relay 1 ON)
```json
{
  "cmd_id": "relay1-on-001",
  "command": "relay_set",
  "relay": 1,
  "state": true
}
```

#### Expected Success ACK
```json
{
  "cmd_id": "relay1-on-001",
  "command": "relay_set",
  "ok": true,
  "message": "relay 1 set to ON",
  "timestamp": 1780000000
}
```

#### Validation Rules
- `relay` must be an integer within range `1` to `7`.
- `state` must be boolean (`true` or `false`).
- If a relay is locked (e.g. `locks[0] == true`), turning it ON will fail with a locked relay error ACK.

---

### `relay_lock`
Sets state locks preventing unauthorized override commands.
- **Publish Topic**: `smartnest/SmartNest_001/cmd/request`

#### Example Payload (Lock Relay 1)
```json
{
  "cmd_id": "lock-r1-001",
  "command": "relay_lock",
  "relay": 1,
  "locked": true
}
```

#### Expected Success ACK
```json
{
  "cmd_id": "lock-r1-001",
  "command": "relay_lock",
  "ok": true,
  "message": "relay 1 locked",
  "timestamp": 1780000000
}
```

#### Validation Rules
- `relay` must be an integer within range `1` to `7`.
- `locked` must be boolean.

---

### `lights_set`
Controls the lights relay group (relays 1–5) together.
- **Publish Topic**: `smartnest/SmartNest_001/cmd/request`

#### Example Payload (Turn Lights OFF)
```json
{
  "cmd_id": "lights-off-001",
  "command": "lights_set",
  "state": false
}
```

#### Expected Success ACK
```json
{
  "cmd_id": "lights-off-001",
  "command": "lights_set",
  "ok": true,
  "message": "relay 1 to relay 5 set to OFF",
  "timestamp": 1780000000
}
```

---

### `all_relays_off`
Turns off all controllable relays (relays 1–7).
- **Publish Topic**: `smartnest/SmartNest_001/cmd/request`

#### Example Payload
```json
{
  "cmd_id": "all-off-001",
  "command": "all_relays_off"
}
```

#### Expected Success ACK
```json
{
  "cmd_id": "all-off-001",
  "command": "all_relays_off",
  "ok": true,
  "message": "all controllable relays turned OFF",
  "timestamp": 1780000000
}
```

---

### `unlock_all_relays`
Clears locks from all relays (relays 1–7).
- **Publish Topic**: `smartnest/SmartNest_001/cmd/request`

#### Example Payload
```json
{
  "cmd_id": "unlock-all-001",
  "command": "unlock_all_relays"
}
```

#### Expected Success ACK
```json
{
  "cmd_id": "unlock-all-001",
  "command": "unlock_all_relays",
  "ok": true,
  "message": "all relay locks cleared",
  "timestamp": 1780000000
}
```

---

### `system_reboot`
Dispatches a reboot command to restart Master, Digital Board, PZEM Board, and Internet ESP32.
- **Publish Topic**: `smartnest/SmartNest_001/cmd/request`

#### Example Payload
```json
{
  "cmd_id": "reboot-001",
  "command": "system_reboot"
}
```

#### Expected Success ACK
```json
{
  "cmd_id": "reboot-001",
  "command": "system_reboot",
  "ok": true,
  "message": "system reboot accepted; reboot sequence started",
  "timestamp": 1780000000
}
```

---

### `ac_set`
Controls the air conditioner via IR — power, exact temperature, temperature step, or fan speed. Exactly one control field must be present per request.
- **Publish Topic**: `smartnest/SmartNest_001/cmd/request`

#### Example Payload (Power ON)
```json
{
  "cmd_id": "ac-power-001",
  "command": "ac_set",
  "power": true
}
```

#### Example Payload (Set Exact Temperature)
```json
{
  "cmd_id": "ac-temp-001",
  "command": "ac_set",
  "temp": 22
}
```

#### Example Payload (Temperature Step)
```json
{
  "cmd_id": "ac-temp-step-001",
  "command": "ac_set",
  "temp_step": "up"
}
```

#### Example Payload (Fan Speed)
```json
{
  "cmd_id": "ac-fan-001",
  "command": "ac_set",
  "fan": "high"
}
```

#### Expected Success ACK
```json
{
  "cmd_id": "ac-temp-001",
  "command": "ac_set",
  "ok": true,
  "message": "ac temperature set to 22",
  "timestamp": 1780000000
}
```

#### Validation Rules
- Exactly one of `power`, `temp`, `temp_step`, `fan` must be present per request. A request with none or more than one of these fields fails validation.
- `power` must be boolean (`true` or `false`).
- `temp` must be an integer within range `16` to `30`.
- `temp_step` must be a string, either `"up"` or `"down"`.
- `fan` must be a string, one of `auto`, `min`, `low`, `med`, `high`, `max`.
- Operating mode is fixed internally and cannot be changed through this interface.

---

## 11. Invalid MQTT Command Examples

### Example 1: Missing `command` Field
If the `command` field is missing, validation fails immediately.

#### Request
```json
{
  "cmd_id": "missing-command-001"
}
```

#### Expected ACK Response
```json
{
  "cmd_id": "missing-command-001",
  "command": "unknown",
  "ok": false,
  "message": "missing command field",
  "timestamp": 1780000000
}
```

### Example 2: Unknown Command
Request sent with a non-existent command name.

#### Request
```json
{
  "cmd_id": "unknown-cmd-001",
  "command": "fan_speed"
}
```

#### Expected ACK Response
```json
{
  "cmd_id": "unknown-cmd-001",
  "command": "fan_speed",
  "ok": false,
  "message": "unknown MQTT command",
  "timestamp": 1780000000
}
```

### Example 3: Invalid JSON Formatting
If the published request payload is malformed or invalid JSON:

#### Expected ACK Response
```json
{
  "cmd_id": "unknown",
  "command": "unknown",
  "ok": false,
  "message": "invalid JSON payload",
  "timestamp": 0
}
```

### Example 4: AC Command With No Control Field
`ac_set` requires exactly one of `power`, `temp`, `temp_step`, or `fan`. Sending none (or more than one) fails validation.

#### Request
```json
{
  "cmd_id": "ac-empty-001",
  "command": "ac_set"
}
```

#### Expected ACK Response
```json
{
  "cmd_id": "ac-empty-001",
  "command": "ac_set",
  "ok": false,
  "message": "exactly one of power, temp, temp_step, fan must be provided",
  "timestamp": 1780000000
}
```

---

## 12. History Batch Upload

**Topic Name**: `smartnest/SmartNest_001/history/batch`
- **Purpose**: Uploads historical, SD-card-buffered log records when reconnecting or during offline catchups.

### Example Payload
```json
{
  "batch_id": "SmartNest_001-123-456789",
  "device": "SmartNest_001",
  "records": [
    {
      "id": 123,
      "epoch": 1780000000,
      "date": "2026-06-27 12:30:00",
      "voltage": 230.1,
      "main_current": 1.25,
      "main_power_w": 287.63,
      "main_energy_kwh": 2.345,
      "digital_current": 0.50,
      "digital_power_w": 115.00,
      "digital_energy_kwh": 0.567,
      "ac_current": 0.350,
      "ac_power_w": 80.50,
      "ac_energy_kwh": 1.234,
      "runtimes_sec": [120, 0, 0, 0, 0, 0, 45],
      "time_source": "NTP"
    }
  ]
}
```

### Field Definitions
- `batch_id` *(string)*: Built dynamically as `<clientId>-<lastRecordId>-<millis>`.
- `device` *(string)*: Echoes the Client ID (`SmartNest_001`).
- `records` *(array)*: Contains record objects containing logged metrics:
  - `id` *(number)*: The monotonic database record row ID.
  - `epoch` *(number)*: Local Unix timestamp in seconds.
  - `date` *(string)*: Local time string format (`YYYY-MM-DD HH:MM:SS`).
  - `voltage` *(number)*: Logged average voltage.
  - `main_current` *(number)*: Logged local current.
  - `main_power_w` *(number)*: Logged local calculated power.
  - `main_energy_kwh` *(number)*: Logged local energy reading.
  - `digital_current` *(number)*: Logged remote current.
  - `digital_power_w` *(number)*: Logged remote power.
  - `digital_energy_kwh` *(number)*: Logged remote energy reading.
  - `ac_current` *(number)*: Logged PZEM current.
  - `ac_power_w` *(number)*: Logged PZEM power.
  - `ac_energy_kwh` *(number)*: Logged daily PZEM energy calculations.
  - `runtimes_sec` *(number array)*: Relay runtimes index logged for this period.
  - `time_source` *(string)*: Timestamp status at the time of writing.

---

## 13. History ACK

**Topic Name**: `smartnest/SmartNest_001/history/ack`
- **Direction**: Subscribe (backend publishes, SmartNest ESP32 listens)

When a batch is successfully saved to the cloud, the backend must acknowledge receipt to let the Master ESP32 advance the log sync cursor.

### Payload Format
```json
{
  "batch_id": "SmartNest_001-123-456789",
  "ok": true,
  "last_id": 123
}
```

### Acknowledge Rules
1. **Validation**: The payload must have `ok == true` and `batch_id` matching the currently pending batch ID.
2. **Advancing Cursor**: `last_id` defines the highest successfully committed record row ID. (If `0` is passed, the device automatically commits up to the last ID of the current batch).
3. **Execution**: On a valid ACK, the device instructs the Master to record progress and purge the verified database entries, freeing up space. The device then immediately requests the next historical block.
4. **Reliability**: If no ACK arrives within the 15-second timeout window, the device drops the pending session status and retries uploading the block from the last verified index.


---

## 14. Backend Integration Notes

When designing a backend interface to process telemetry:
- **Separation of Telemetry**: Store real-time measurements (`/live/sensors`) in a volatile or short-retention cache (e.g. InfluxDB/Redis) and use historical batch records (`/history/batch`) for permanent database storage.
- **Deduplication**: Implement database unique constraints on `(device_id, record_id)` to handle retries idempotently.
- **Client IDs**: Ensure your backend service uses a unique client ID. Never reuse the device Client ID (`SmartNest_001`) as this will initiate broker connection drop loops.

---

## 15. Security Notes

- **Public Broker Danger**: The default broker (`broker.hivemq.com`) is public. Telemetry, status, and control packets are readable by any client on the internet.
- **Production Checklist**: Production deployments must configure a private broker using SSL/TLS, port `8883`, strong client credential authentication, and restricted access control lists (ACLs).
