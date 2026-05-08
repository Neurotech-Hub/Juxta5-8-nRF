# juxta5-8-prod

Production Hublink firmware for Juxta5-8 (nRF52840). Combines Hublink GATT BLE, append-only NOR CSV logging, and LIS2DH12 motion/temperature sensing. Settings persist in nRF52840 internal flash.

---

## Boot and State Machine

### Boot path

```
Power-on / Reset
      │
      ├─ RESETREAS == 0 (fresh power-on)
      │         └─→ Shelf mode (System OFF, MAG_INT wake). No LED.
      │
      ├─ RESETREAS[OFF] set (woke from System OFF via magnet)
      │         └─→ LED ON while magnet held → measure hold duration
      │                   ├─ < 7 s  → LED off → slow blink → Datetime sync phase
      │                   └─ ≥ 7 s  → 3× blink → fast blink → DFU mode (stub)
      │
      └─ Other (watchdog, pin reset, lockup)
                └─→ Skip shelf/hold, go straight to production init. No LED gate.
```

### Datetime sync phase (normal wake only)

```
Slow blink (50 ms ON / 450 ms OFF) — waiting for iOS connection
      │
      └─ iOS connects
               │  LED: solid ON
               ├─ timestamp received via Gateway
               │         └─ wait for disconnect
               │                   └─ 5× blink → LED off → Production init
               │
               └─ disconnect without timestamp
                         └─ slow blink → restart connectable advertising
                                         (loops indefinitely until timestamp received)
```

### Production state machine

```
IDLE
 ├─ scan interval elapsed  → SCANNING (3 s passive burst) → flush JXB rows → IDLE
 ├─ adv interval elapsed   → ADVERTISING (0.5 s non-conn) → IDLE
 ├─ JXGA_ beacon seen      → CONNECTABLE_ADV (30 s)       → IDLE
 └─ iOS connects           → CONNECTED (radio owned by BT stack)
                                     └─ disconnect → IDLE

Parallel vitals timer (every vitals_interval_s):
  LIS2DH12 temp + SAADC batt_mv + motion count → append JXV row
```

---

## State Machine Diagram

```mermaid
stateDiagram-v2
    [*] --> ShelfMode : Fresh power-on\n(RESETREAS == 0)

    ShelfMode --> MeasureHold : Magnet applied\nSystem OFF releases → cold boot

    MeasureHold --> WakeNormal : Released before 7 s\nLED off
    MeasureHold --> DFUMode : Held >= 7 s\n3x blink then fast blink

    DFUMode --> ShelfMode : Magnet swipe after 5 s debounce\nLED off
    DFUMode --> DFUMode : Fast blink waiting\n(MCUboot SMP BLE stub)

    WakeNormal --> ConnectableAdv : Slow blink begins\nStart connectable advertising

    ConnectableAdv --> ConnectableAdv : Disconnect without timestamp\nResume slow blink
    ConnectableAdv --> ConnectedSync : iOS connects\nLED solid ON

    ConnectedSync --> WaitDisconnect : timestamp received\nJXS time_set row appended
    WaitDisconnect --> ProductionInit : Disconnected\n5x blink then LED off

    ProductionInit --> IDLE : LIS2DH12 init\nhardware_ready = true

    IDLE --> SCANNING : scan interval elapsed
    IDLE --> ADVERTISING : adv interval elapsed
    IDLE --> CONNECTABLE_ADV : JXGA_ beacon seen

    SCANNING --> IDLE : 3 s burst done\nflush JXB rows
    ADVERTISING --> IDLE : 0.5 s burst done
    CONNECTABLE_ADV --> IDLE : 30 s timeout

    IDLE --> CONNECTED : iOS connects
    SCANNING --> CONNECTED : iOS connects
    CONNECTABLE_ADV --> CONNECTED : iOS connects

    CONNECTED --> IDLE : disconnected\nresume state machine

    ShelfMode --> ShelfMode : No magnet applied\nremains in System OFF
```

---

## Magnet Gesture Reference

| Gesture | LED feedback | Effect |
|---|---|---|
| Apply at any time (device in shelf mode) | LED ON immediately | Wakes device from System OFF → cold boot |
| Release < 7 s after cold boot | LED off → slow blink (50/450 ms) | Gateway advertising mode; waits for datetime sync |
| Hold ≥ 7 s after cold boot | 3× blink → fast blink (50/50 ms) | DFU mode; 5 s debounce then magnet swipe returns to shelf |
| Any connect event | Solid ON | Active BLE connection |
| Disconnect after valid timestamp | 5× blink → LED off | Production init begins |
| Disconnect without timestamp | Slow blink resumes | Restarts connectable advertising |
| Magnet swipe during DFU fast-blink | LED off | Returns device to shelf mode |
| Magnet held during production (not connected) | 5× blink → LED off | 5 s debounce then shelf mode |

LED0 (P0.09, `CONFIG_NFCT_PINS_AS_GPIOS=y`) is used for all sequences.

---

## Hublink Gateway Data Exchange

All BLE communication uses the **Hublink service** (`57617368-5501-0001-8000-00805f9b34fb`).

### Characteristics

| Characteristic | UUID suffix | Permissions | Description |
|---|---|---|---|
| Node | `...5505` | READ | JSON device status; read once after connect |
| Gateway | `...5504` | WRITE / WRITE NO RSP | JSON command from iOS to device |
| Filename | `...5502` | READ / WRITE / INDICATE | File listing and file selection |
| File Transfer | `...5503` | READ / INDICATE / CCC | Chunked file payload |

---

### Node characteristic (READ → iOS)

Single JSON object. iOS reads this once on connect.

```json
{
  "upload_path": "/TEST",
  "firmware_version": "5.8.0",
  "battery_level": 87,
  "device_id": "JX_9B10A1",
  "operating_mode": 0,
  "alert": "",
  "product": "Juxta5-8",
  "log_schema": "jxta-nor-csv-v3",
  "logging_version": 3,
  "experiment": "social_v1"
}
```

| Field | Type | Description |
|---|---|---|
| `upload_path` | string | iOS upload destination path |
| `firmware_version` | string | Semantic version; `5.8.x` signals NOR CSV decoder |
| `battery_level` | integer 0–100 | Percent, derived from SAADC voltage (3.0 V = 0 %, 4.2 V = 100 %) |
| `device_id` | string | Stable hardware ID `JX_XXXXXX` (last 3 MAC bytes); never changes |
| `operating_mode` | integer | `0` = normal; reserved, no ADC mode on this hardware |
| `alert` | string | Reserved for future device alerts; currently always `""` |
| `product` | string | `"Juxta5-8"` — tells iOS to use the NOR CSV decoder |
| `log_schema` | string | `"jxta-nor-csv-v3"` — identifies the log format version |
| `logging_version` | integer | `3` — integer decoder selector |
| `experiment` | string | Current experiment string from NVS settings |

---

### Gateway characteristic (WRITE → device)

JSON object; any subset of keys may be sent. Unrecognized keys are silently ignored.

```json
{
  "timestamp": 1746000000,
  "sendFilenames": true,
  "clearMemory": true,
  "operatingMode": 0,
  "scanInterval": 30,
  "vitalsInterval": 60,
  "subjectId": "mouse_07",
  "uploadPath": "/social_v1",
  "experiment": "social_v1"
}
```

| Field | Type | Effect |
|---|---|---|
| `timestamp` | uint32 Unix seconds | Sets device clock; appends `time_set` row to JXS |
| `sendFilenames` | bool | Triggers file listing indication on Filename characteristic |
| `clearMemory` | bool | Erases all NOR CSV regions; creates fresh daily files; appends `memory_cleared` to JXS; **does not clear NVS settings** |
| `reset` | bool | Immediately enters shelf mode (System OFF in production; soft reboot in debug) |
| `operatingMode` | integer | Stored in NVS `mode`; no behavioral effect on this hardware (ADC mode removed) |
| `scanInterval` | integer (seconds) | BLE scan interval; stored in NVS; appends `settings_changed` to JXS |
| `vitalsInterval` | integer (seconds) | Vitals logging interval; stored in NVS; appends `settings_changed` to JXS |
| `subjectId` | string | Subject assignment; stored in NVS; appends `settings_changed` to JXS |
| `uploadPath` | string | iOS upload path prefix; stored in NVS |
| `experiment` | string | Experiment grouping string; stored in NVS; appends `settings_changed` to JXS |

---

### File listing (Filename characteristic)

Triggered by `"sendFilenames": true` in a Gateway write, or by writing `"LIST"` to the Filename characteristic directly. The device sends an **indication** with a semicolon-separated list:

```
JXS20260507.csv|2048;JXV20260507.csv|14592;JXB20260507.csv|30720
```

Each entry is `filename|size_in_bytes`. The size for active (open) files includes the synthetic `#EOF` marker that will be appended during transfer.

---

### File transfer protocol (File Transfer characteristic)

1. iOS writes the desired filename to the **Filename** characteristic (e.g. `JXV20260507.csv`).
2. The device streams the file as a sequence of **indications** on the **File Transfer** characteristic.
   - Each indication payload is `min(MTU − 3, 512)` bytes.
   - The device waits for each indication confirmation before sending the next chunk.
3. The final indication contains the literal string `#EOF` (5 bytes), signalling end of file.
4. iOS should request MTU exchange to 247 bytes for optimal throughput (~244 bytes/indication).

---

## NOR Flash Layout

| Region | Address | Size | Content |
|---|---|---|---|
| JXS | `0x000000 – 0x00FFFF` | 64 KB | Settings / events log |
| JXV | `0x010000 – 0x10FFFF` | 1 MB | Vitals log |
| JXB | `0x110000 – 0x3FFFFF` | 3 MB | BLE observations log |

All regions are append-only CSV. Each pseudo-file is physically stored as:

```
JXVYYYYMMDD.csv
unix,motion,batt_v,temp_c
1715200000,12,3.81,24
#EOF
```

Settings and the log-state cache live in nRF52840 internal flash (`storage_partition` at `0xF0000`, 64 KB) via Zephyr NVS.

---

## File Structure

```
applications/juxta5-8-prod/
├── CMakeLists.txt
├── prj.conf
├── sample.yaml
├── README.md            ← this file
└── src/
    ├── main.c           ← boot sequence, magnet hold, shelf mode, state machine
    ├── ble_service.c/h  ← Hublink GATT (Node/Gateway/Filename/FileTransfer)
    ├── juxta_log.c/h    ← NOR CSV append, list, read, recover
    ├── juxta_settings.c/h ← NVS-backed current settings + log-state cache
    ├── juxta_time.c/h   ← Unix timestamp set/get, date string formatter
    └── juxta_prod.h     ← shared constants (version, sizes, defaults)
```

---

## Implementation Status and Remaining Work

### Implemented

| Feature | Notes |
|---|---|
| Shelf mode (System OFF) | Fresh boot → `sys_poweroff()`, MAG_INT `GPIO_INT_LEVEL_LOW` wake source |
| Magnet hold measurement | < 7 s → normal wake, ≥ 7 s → DFU path |
| LED mode state machine | `OFF`, `ON` (connected), `SLOW_BLINK` (50/450 ms, gateway adv), `FAST_BLINK` (50/50 ms, DFU) driven by kernel timer + work item |
| LED blink sequences | Magnet hold: solid ON; DFU entry: 3× 200 ms blink; production init: 5× 50 ms blink |
| Datetime sync gate | Connectable adv after normal wake; loops indefinitely until timestamp received; must have timestamp before production init |
| Watchdog | 30 s window, fed every 10 s, pauses on debug halt |
| NVS settings | `subject_id`, `experiment`, `mode`, `scan_interval_s`, `vitals_interval_s`, `upload_path` |
| NOR CSV logging | JXS events, JXV vitals, JXB BLE observations; append-only, `#EOF` on close |
| Log-state cache | MCU NVS caches file offsets; scans NOR on cache miss |
| BLE state machine | Non-conn advertising bursts, passive scan bursts, JXGA_ gateway detection |
| Battery level in Node JSON | SAADC mV sampled on connect and each vitals tick; linear 3.0–4.2 V → 0–100 % |
| Hublink GATT service | Node, Gateway, Filename, File Transfer characteristics; UUIDs match iOS companion |
| Node JSON | Legacy keys + `product`, `log_schema`, `logging_version`, `experiment` (5.8 fields) |
| Gateway commands | `timestamp`, `sendFilenames`, `clearMemory`, `operatingMode`, `scanInterval`, `subjectId`, `uploadPath`, `experiment` |
| Vitals logging | LIS2DH12 temperature, SAADC battery voltage, motion count → JXV |
| BLE observation logging | `JX_XXXXXX` peer detection → JXB rows with observer/peer/rssi |
| JXS provenance rows | `boot`, `time_set`, `settings_changed`, `user_connected`, `user_disconnected`, `memory_cleared` |

### Pending / Not Yet Implemented

| Feature | Notes |
|---|---|
| **DFU mode** | `enter_dfu_mode()` is a halting stub. Requires MCUboot SMP BLE stack (`CONFIG_MCUMGR`, `CONFIG_IMG_MANAGER`, SMP Bluetooth transport). DFU advertising name and behavior to be defined. |
| **Re-enter shelf mode** | No mechanism to return to System OFF after production mode. Needs a gateway command (e.g., `goToSleep`) or idle timeout. |
| **Low-battery gate** | No write suppression at low battery. Needs a threshold check before NOR writes (pattern from legacy `should_allow_fram_write()`). |
| **Advertising interval setting** | `advInterval` gateway key updates `scan_interval_s` as a placeholder. These should be separate fields. |
| **Daily file rotation** | New date-named files are created when `juxta_time_now()` crosses midnight. Requires the clock to be valid; files accumulate until `clearMemory`. |
| **iOS companion decoder** | iOS app needs a branch path for `log_schema: jxta-nor-csv-v3` to parse JXS/JXV/JXB CSV files instead of legacy FRAMFS binary blobs. |
| **Hardware validation** | Full boot/magnet/datetime/scan/vitals/transfer sequence not yet tested on hardware. |
