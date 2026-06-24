# SmartNest MQTT Guide

This document is the main MQTT reference for SmartNest cloud integration.

Default base topic: `smartnest`

The base topic is configurable from the local dashboard or Serial Monitor. In this document, `<base>` means the configured MQTT base topic.

## MQTT Sections

SmartNest MQTT is divided into three sections.

| Section | Topic prefix | Purpose | ACK required |
|---|---|---|---|
| Live data | `<base>/live/...` | Current readings and current state | No |
| Commands | `<base>/cmd/...` | Cloud-to-device commands and command result ACKs | Yes |
| Historic data | `<base>/history/...` | SD-backed records for cloud database storage | Yes, batch ACK |

Old topics such as `<base>/sensor/...`, `<base>/relay/...`, `<base>/slave/...`, `<base>/status`, and `<base>/cmd/slave/...` are removed.

## Connection Defaults

| Setting | Default |
|---|---|
| Broker | `broker.hivemq.com` |
| Port | `1883` |
| Client ID | `SmartNest_001` |
| Username | empty |
| Password | empty |
| Keepalive | `60` seconds |
| Base topic | `smartnest` |

## Live Data Topics

Live topics publish current data only. Cloud should not send ACKs for these messages.

| Topic | Payload | Retained | Meaning |
|---|---|---:|---|
| `<base>/live/status` | JSON | no | Device, WiFi, MQTT, SD, slave, and sensor health |
| `<base>/live/sensors` | JSON | no | Voltage, currents, power, energy, temperature, humidity |
| `<base>/live/relays` | JSON | no | Relay states, lock states, master lock, digital switch, runtimes |

### `<base>/live/status`

Example:

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
  "reset_reason": "POWERON"
}
```

### `<base>/live/sensors`

Example:

```json
{
  "voltage": 230.1,
  "energy_voltage": 230.1,
  "voltage_estimated": false,
  "main_current": 1.25,
  "digital_current": 0.52,
  "ac_current": 1.234,
  "ac_power": 287.5,
  "ac_energy_kwh": 18.905,
  "main_energy_kwh": 0.120,
  "digital_energy_kwh": 0.040,
  "temperature_c": 28.4,
  "humidity_pct": 61.0
}
```

### `<base>/live/relays`

Relay array indexes are zero-based inside arrays:

| Array index | UI relay |
|---:|---|
| `0` to `5` | SmartNest local relay 1-6 |
| `6` | Digital Board relay 7 |

Example:

```json
{
  "states": [false, false, true, false, false, false, true],
  "locks": [false, false, false, false, false, false, false],
  "master_lock": false,
  "digital_switch": true,
  "runtime_sec": [12, 0, 44, 0, 0, 0, 18]
}
```

## Command Topics

Cloud sends all new commands to:

```text
<base>/cmd/request
```

SmartNest replies on:

```text
<base>/cmd/ack
```

Every command should include a unique `cmd_id`. Cloud should use the ACK with the same `cmd_id` to update UI state.

### Command Request Format

```json
{
  "cmd_id": "abc123",
  "type": "relay_set",
  "relay": 1,
  "state": true
}
```

### Command ACK Format

```json
{
  "cmd_id": "abc123",
  "type": "relay_set",
  "ok": true,
  "reason": "done",
  "relay": 1,
  "state": true,
  "locked": false
}
```

### Supported Commands

| Type | Required fields | Meaning |
|---|---|---|
| `relay_set` | `cmd_id`, `relay`, `state` | Set relay 1-7 ON/OFF |
| `relay_toggle` | `cmd_id`, `relay` | Toggle relay 1-7 |
| `relay_lock` | `cmd_id`, `relay`, `locked` | Lock/unlock relay 1-7 |
| `master_lock` | `cmd_id`, `state` | Set master lock ON/OFF |
| `slave_reboot` | `cmd_id`, `target` | Reboot `digital`, `d1`, or `pzem` |
| `pzem_energy_reset` | `cmd_id` | Reset PZEM energy register |

### ACK Reasons

| Reason | Meaning |
|---|---|
| `done` | Command completed |
| `sent` | Command was sent to another board; no final board-level ACK is available |
| `invalid_payload` | JSON could not be parsed |
| `invalid_relay` | Relay number is outside 1-7 |
| `invalid_target` | Slave target is invalid |
| `locked` | Relay lock blocked ON command |
| `master_locked` | Master lock blocked ON command |
| `overcurrent_locked` | Digital Board overcurrent lock blocked command |
| `busy` | Another relay 7 command is waiting for Digital Board ACK |
| `timeout` | Digital Board ACK did not arrive in time |
| `rejected` | Command was not accepted |
| `unsupported` | Command type is not supported |

## Historic Data Topics

Historic records are intended for cloud database storage.

SmartNest publishes:

```text
<base>/history/batch
```

Cloud acknowledges:

```text
<base>/history/ack
```

### History Batch

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

### History ACK

```json
{
  "batch_id": "SmartNest_001-51-123456",
  "ok": true,
  "last_id": 51
}
```

SmartNest forwards the ACK to Master as `hist_ack`. Master stores the latest acknowledged record id in SD sync metadata.

## Historic SD Source

Master owns energy calculation and SD logs. New log rows use:

```csv
record_id,epoch,date,voltage,main_current,main_power_w,main_energy_kwh,digital_current,digital_power_w,digital_energy_kwh,ac_current,ac_power_w,ac_energy_kwh,r1_on_s,r2_on_s,r3_on_s,r4_on_s,r5_on_s,r6_on_s,r7_on_s
```

Important rules:

- Master writes history to SD first.
- SmartNest requests unsent records from Master over UART.
- SmartNest publishes those records to MQTT.
- Cloud must ACK the batch.
- Master advances sync state only after ACK.
- If MQTT/cloud is offline, SD keeps records for later upload.

## Removed Compatibility Topics

Old `<base>/sensor/...`, `<base>/relay/...`, `<base>/slave/...`, `<base>/switch/...`, `<base>/status`, and `<base>/cmd/slave/...` topics are not published or subscribed. Use only live JSON topics, `<base>/cmd/request`, `<base>/cmd/ack`, and history batch/ACK topics.

## Cloud Implementation Notes

- Use JSON command payloads for all new commands.
- Generate unique `cmd_id` per command.
- Update command UI from `<base>/cmd/ack`, not from live data alone.
- Store history records by `id`; use idempotent upsert to avoid duplicates.
- Send `<base>/history/ack` only after records are safely stored in the cloud database.
- Treat live data as volatile; do not use it as permanent history.
