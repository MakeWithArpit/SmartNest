# SmartNest Verification Checklist

Use this checklist after firmware changes, flashing, wiring changes, MQTT broker changes, or cloud dashboard updates.

## 1. Build Verification

Run these from the repository root.

```text
arduino-cli compile --fqbn esp32:esp32:esp32 SmartNest
arduino-cli compile --fqbn esp32:esp32:esp32 master
arduino-cli compile --fqbn esp32:esp32:esp32 slave_digital_board
arduino-cli compile --fqbn esp32:esp32:esp32 slave_pzem
```

Pass criteria:

- All four sketches compile.
- No missing library error.
- Program storage is below ESP32 limit.
- Dynamic memory is below ESP32 limit.

## 2. Flashing Verification

Flash the correct firmware to each ESP32.

| Board | Sketch |
|---|---|
| SmartNest WiFi/MQTT ESP | `SmartNest` |
| Master ESP | `master` |
| Digital Board ESP | `slave_digital_board` |
| PZEM ESP | `slave_pzem` |

Pass criteria:

- Every board boots without reboot loop.
- Serial Monitor opens at `115200`.
- Each board prints startup logs.

## 3. Wiring Verification

Check before powering high-voltage loads.

### SmartNest ESP

- Relay pins: GPIO `2`, `4`, `5`, `19`, `18`, `15`.
- ACS712 input: GPIO `34`.
- DHT11 data: GPIO `23` / D23.
- UART2 to Master: RX `16`, TX `17`.
- Reset button: GPIO `0`.

### Master ESP

- UART2 to SmartNest: RX `16`, TX `17`.
- SD card SPI: CS `5`, MOSI `23`, MISO `19`, SCK `18`.
- Manual switches: GPIO `33`, `32`, `26`, `27`, `14`, `13`.

### Digital Board ESP

- Relay hardware switches ON/OFF correctly.
- ACS712 is connected and calibrated.
- Manual switch input works.

### PZEM ESP

- PZEM Serial2 RX/TX wiring is correct.
- PZEM has common ground with ESP power side.
- AC measurement wiring is safe and insulated.

Pass criteria:

- No board gets hot.
- No relay chatters on boot.
- DHT11 readings become valid after a few seconds.
- SD card is detected by Master.

## 4. WiFi Provisioning Verification

Test from a clean WiFi state when needed.

Pass criteria:

- SmartNest starts provisioning AP if no WiFi credentials exist.
- WiFi setup page scans networks.
- Correct SSID/password connects.
- SmartNest prints IP address.
- `http://smart-nest.local/` opens when mDNS works.
- Direct IP opens dashboard if mDNS does not work.

## 5. Dashboard Verification

Pass criteria:

- Login works.
- Visible Logout button is not shown.
- Refresh button works.
- Closing the browser tab logs out automatically.
- Reopening the dashboard requires login again after tab/session close.
- Dashboard shows:
  - Temperature
  - Humidity
  - Main board current and energy
  - Digital board current and energy
  - AC current, power, and energy
  - Voltage
  - SD status/free space
  - Digital/PZEM online status
  - Master lock state
  - Relay states, locks, and runtimes

## 6. Local Relay Verification

Test relays 1-6 from dashboard and Serial Monitor.

Commands:

```text
RELAY 1 ON
RELAY 1 OFF
RELAY 1 TOGGLE
LOCK 1 ON
LOCK 1 OFF
```

Pass criteria:

- Relay turns ON/OFF.
- Dashboard state updates.
- Locked relay does not turn ON.
- Unlocking allows relay ON again.
- Relay state persists according to current firmware behavior after restart.

## 7. Relay 7 Digital Board Verification

Commands:

```text
RELAY 7 ON
RELAY 7 OFF
LOCK 7 ON
LOCK 7 OFF
SLAVE D1 reboot
```

Pass criteria:

- Relay 7 responds through Master and ESP-NOW.
- Digital Board ACK reaches SmartNest.
- MQTT command ACK for relay 7 is delayed until Digital Board ACK or timeout.
- Relay lock blocks ON command.
- Overcurrent lock blocks ON command and reports failure reason.

## 8. Master Lock Verification

Commands:

```text
MASTERLOCK ON
MASTERLOCK OFF
```

Pass criteria:

- Master lock turns local relays OFF.
- Relay 7 receives master lock through Master/Digital Board.
- ON commands are blocked while master lock is active.
- Unlocking allows relay control again.
- Dashboard updates master lock status.

## 9. Sensor Verification

### DHT11

Pass criteria:

- Dashboard shows temperature and humidity.
- `/api/status` includes `temp_c`, `humidity`, and `dht_ok`.
- MQTT live sensors include `temperature_c`, `humidity_pct`, and `dht_ok`.

### SmartNest ACS712

Pass criteria:

- Current reads `0.00 A` when all local relays are OFF.
- Current increases when relay 1-6 load is ON.
- Master receives local current over UART as `acs`.

### Digital Board ACS712

Pass criteria:

- Digital current reads `0.00 A` when relay 7 is OFF.
- Current increases when relay 7 load is ON.
- Overcurrent protection trips at configured threshold.

### PZEM

Pass criteria:

- Voltage is realistic.
- AC current/power change with AC load.
- Energy value increases over time.
- `pzem_health` is true when sensor is valid.
- Offline/unhealthy PZEM does not crash the system.

## 10. SD Logging Verification

Pass criteria:

- Master creates `/SmartNestLogs`.
- Master creates `energy_log.csv`.
- Header includes `record_id`.
- New rows include:
  - main energy
  - digital energy
  - AC/PZEM energy
  - relay runtimes
- Dashboard `SD Info` works.
- Dashboard `List` returns `energy_log.csv`.
- Dashboard `Download CSV` downloads the log.
- `RESET LOGS` clears and recreates CSV with header.

## 11. MQTT Connection Verification

Pass criteria:

- Dashboard MQTT settings load.
- Save MQTT reconnects client.
- SmartNest publishes cloud online status to Master.
- `mqtt_status` becomes connected.
- Broker receives data under configured `<base>` topic.

## 12. MQTT Live Data Verification

Subscribe to:

```text
<base>/live/status
<base>/live/sensors
<base>/live/relays
```

Pass criteria:

- Messages publish after MQTT connection.
- Messages update every heartbeat.
- Messages update after telemetry changes.
- Live topics are not used for command ACKs.
- Live topics are not required to be retained.

## 13. MQTT Command Verification

Subscribe to:

```text
<base>/cmd/ack
```

Publish relay command:

```json
{"cmd_id":"test-relay-1","type":"relay_set","relay":1,"state":true}
```

To:

```text
<base>/cmd/request
```

Pass criteria:

- Relay turns ON.
- ACK arrives on `<base>/cmd/ack`.
- ACK has same `cmd_id`.
- ACK has `ok:true`.
- Live relay state updates.

Test failure:

```json
{"cmd_id":"bad-relay","type":"relay_set","relay":99,"state":true}
```

Pass criteria:

- ACK has `ok:false`.
- Reason is `invalid_relay`.

## 14. MQTT Relay 7 ACK Verification

Publish:

```json
{"cmd_id":"relay7-on","type":"relay_set","relay":7,"state":true}
```

Pass criteria:

- SmartNest forwards command to Master.
- Master forwards command to Digital Board.
- Digital Board sends `cmd_ack`.
- SmartNest publishes `<base>/cmd/ack`.
- If Digital Board is unavailable, ACK eventually returns `timeout`.

## 15. MQTT Historic Sync Verification

Subscribe to:

```text
<base>/history/batch
```

Cloud must publish ACK to:

```text
<base>/history/ack
```

Example ACK:

```json
{"batch_id":"BATCH_ID_FROM_MESSAGE","ok":true,"last_id":51}
```

Pass criteria:

- Master writes record to SD first.
- SmartNest requests unsent records with `hist_req`.
- SmartNest publishes history batch.
- Cloud stores records.
- Cloud sends history ACK.
- SmartNest forwards `hist_ack` to Master.
- Master advances `sync_state.txt`.
- Same record is not resent after ACK.

## 16. Offline Recovery Verification

Test by disconnecting WiFi or MQTT broker.

Pass criteria:

- Local relays still work.
- Master continues SD logging.
- MQTT reconnects when network returns.
- Unsent history records upload after reconnect.
- No duplicate database rows if cloud stores by `id`.

## 17. Compatibility Topic Verification

Old topics should still work during migration.

Subscribe:

```text
<base>/sensor/voltage
<base>/relay/0/state
<base>/relay/6/state
```

Publish:

```text
Topic: <base>/relay/0/set
Payload: true
```

Pass criteria:

- Old sensor topics still publish.
- Old relay state topics still publish retained state.
- Old relay command topics still control relays.
- New `<base>/cmd/ack` may emit legacy command ACK with generated command id.

## 18. Reset Verification

Commands:

```text
RESET MQTT
RESET ENERGY
RESET LOGS
RESET FULL
```

Pass criteria:

- `RESET MQTT` restores default MQTT settings.
- `RESET ENERGY` clears energy counters.
- `RESET LOGS` clears SD CSV and sync state.
- `RESET FULL` resets intended state and restarts provisioning path.

## 19. Failure Cases To Verify

- Wrong MQTT broker host.
- MQTT disabled.
- MQTT broker disconnect during history upload.
- Cloud does not send history ACK.
- Invalid command JSON.
- Relay ON while relay lock is active.
- Relay ON while master lock is active.
- Relay 7 command while Digital Board is offline.
- SD card missing.
- SD card full or write failure.
- PZEM offline/unhealthy.
- DHT11 disconnected.
- WiFi reconnect after router restart.

## 20. Final Acceptance Criteria

The system is ready when:

- All four sketches compile.
- Dashboard works and no visible Logout button is present.
- Live MQTT topics publish correct data.
- Commands use JSON request/ACK flow.
- Relay 7 ACK/timeout behavior works.
- Historic records are SD-first and cloud-ACKed.
- Offline MQTT does not lose history data.
- Old MQTT topics still work during migration.
- Documentation matches actual topics and payloads.
