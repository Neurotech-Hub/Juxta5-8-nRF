# Juxta5-8 nRF52840 Firmware

This repository is the active Juxta5-8 firmware workspace. The `Reference/` directory is retained as historical source material and is not part of active implementation.

## Hardware Pin Map

| Signal      | nRF Pin        | Notes                                                                             |
| ----------- | -------------- | --------------------------------------------------------------------------------- |
| XL1         | `P0.00`        | External oscillator                                                               |
| XL2         | `P0.01`        | External oscillator                                                               |
| SPI MISO    | `P0.08`        | Shared SPI bus                                                                    |
| LED0        | `P0.09`        | Status LED                                                                        |
| SPI SCK     | `P0.12`        | Shared SPI bus                                                                    |
| ~RESET      | `P0.18`        | Reset pin                                                                         |
| MAG_INT     | `P0.25`        | **Electrical LOW when magnet present** (active-low; devicetree `GPIO_ACTIVE_LOW`) |
| ~AXY_INT2   | `P0.28` / AIN4 | Accelerometer interrupt 2 (input)                                                 |
| FUEL / AIN6 | `P0.30`        | Battery divider output                                                            |
| ~AXY_CS     | `P1.06`        | LIS2DH12 chip select                                                              |
| SPI MOSI    | `P1.09`        | Shared SPI bus                                                                    |
| ~AXY_INT1   | `P1.11`        | Accelerometer interrupt 1                                                         |
| ~CS_MEM     | `P1.13`        | NOR flash chip select                                                             |
| ANT         | Fixed RF       | BLE antenna (not assignable)                                                      |

## Hardware Notes

- **MCU**: Nordic nRF52840
- **Magnet input `MAG_INT` (`P0.25`)**: DTS uses **`GPIO_ACTIVE_LOW`** — a present magnet asserts **electrical LOW** on the pad. Zephyr **`gpio_pin_get_dt(magnet_sensor)`** is **non-zero** when the magnet is present.
- **Accelerometer**: LIS2DH12TR over SPI (reuse existing LIS2DH12 approach from reference applications)
- **External memory**: 32 Mbit SPI NOR flash (`C2939873.pdf` in `Reference/`)
- **Fuel divider** (P0.30 / AIN6):
  - `Rtop = 1.5 MOhm` to `VBATT`
  - `Rbottom = 220 kOhm` to `GND`
  - Divider ratio: `0.12790698`
  - Nominal conversion: `VBATT = VFUEL * 7.82`; calibrated empirically to `7.96` (board under-reads ~2%)
  - Expected max VFUEL at 4.2V LiPo: about `0.537V`
- **P0.28 / AIN4**: `~AXY_INT2` (accelerometer interrupt 2, input only — not currently wired in firmware)

## Software Constraints

- **FUEL ADC only** on production and bring-up apps (P0.30 / AIN6); no electric mode.
- **External NOR** logging uses append-only CSV pseudo-files (not legacy FRAM behavior). See [`docs/JUXTA_NOR_Flash_Logging_Spec_v3.md`](docs/JUXTA_NOR_Flash_Logging_Spec_v3.md) and production schema **`jxta-nor-csv-v5`** in [`applications/juxta5-8-prod`](applications/juxta5-8-prod).

## Planned Application Structure

Bring-up and feature work are intentionally split into separate apps so each subsystem can be validated in isolation.

### `applications/juxta5-8-blink` (Phase 2)
- **Purpose**: Hardware sanity check for LED, magnet path, and FUEL ADC (battery divider on **`P0.30` / `AIN6`**, same as production).
- **Behavior**: **Magnet absent** ⇒ pad **high**, GPIO **inactive** ⇒ LED blinks (~200 ms); **magnet present** ⇒ pad driven **LOW**, GPIO **active** ⇒ LED forced on. RTT magnet lines mirror **inactive (“LOW blink mode”) vs active (“forced ON”)** rather than spelling out raw voltage — see Hardware Notes for polarity. Every ~2 s, RTT logs FUEL (`vfuel` / `vbatt` / rough `soc`, same divider as below).
- **Dependencies**: GPIO, SAADC (`zephyr,user` / `io-channels` → `AIN4`), RTT logging.
- **Reference links**:
  - [`Reference/nRF/applications/p0_05_test`](Reference/nRF/applications/p0_05_test)
  - [`Reference/nRF/applications/juxta-axy`](Reference/nRF/applications/juxta-axy)

### `applications/juxta5-8-axy` (Implemented)
- **Purpose**: Validate LIS2DH12 SPI transactions and interrupt behavior on Juxta5-8 pins.
- **Behavior**:
  - Uses SPI Mode 0 transport for LIS2DH12.
  - Emits periodic RTT telemetry (`x_mg`, `y_mg`, `z_mg`, `temp_c`, magnet state, motion count, `INT1_SRC`).
  - Logs motion events from INT1 with event counters and latest accelerometer snapshot.
- **Dependencies**: SPI, GPIO interrupt handling, LIS2DH12 register layer.
- **Reference links**:
  - [`Reference/nRF/applications/juxta-axy`](Reference/nRF/applications/juxta-axy)
  - [`Reference/nRF/applications/juxta-ble/src/lis2dh12.c`](Reference/nRF/applications/juxta-ble/src/lis2dh12.c)

### `applications/juxta5-8-mem` (Implemented)
- **Purpose**: Exercise the external 32 Mbit SPI NOR via Zephyr’s flash API (`DT_ALIAS(spi_mem)` → board `mem0`).
- **Warning**: The firmware **erases the first physical erase block at offset 0**, then writes and verifies a short incremental pattern. **Anything that must persist in the bottom of external flash will be destroyed.** Only run when that region is unused (or you accept a full reflash of metadata stored there).
- **Behavior**:
  - At boot, logs `[PROBE]` parameters (write block size, erase value, optional total size) and the first page layout entry.
  - `[ERASE]` at offset 0 for the full first erase page, then `[WRITE]` / read-back and `[VERIFY]` for up to 256 bytes (length aligned to `write_block_size` and capped by that page).
  - Log lines are tagged `[PROBE]`, `[ERASE]`, `[WRITE]`, `[VERIFY]` with `PASS`/`FAIL` for each step.
- **Devicetree note**: `prj.conf` enables `CONFIG_SPI_NOR_SFDP_RUNTIME` so the part can describe itself without a static `jedec-id` on `mem0`. If initialization still fails on hardware, add `jedec-id` from the part datasheet ([`Reference/C2939873.pdf`](Reference/C2939873.pdf)) to the `mem0` node in the board DTS.
- **Example RTT lines** (success):
  - `[PROBE] ok write_block_size=… erase_value=0xff …`
  - `[ERASE] offs=0 size=…` → `[ERASE] PASS`
  - `[WRITE] …` → `[WRITE] PASS`
  - `[VERIFY] PASS len=…`
- **Dependencies**: `CONFIG_FLASH`, SPI NOR, RTT console (same pattern as other bring-up apps).
- **Reference links**:
  - [`Reference/C2939873.pdf`](Reference/C2939873.pdf)
  - [`Reference/nRF/applications/juxta-file-system`](Reference/nRF/applications/juxta-file-system)

### `applications/juxta5-8-ble-test` (Implemented)
- **Purpose**: BLE bring-up plus power-characterization helpers: LED control, one-shot battery read, and magnet-wake deep sleep emulation.
- **Behavior**:
  - Advertises as **`Juxta5-8-BLE`** (connectable, fast interval). **TX power** is **+8 dBm** at the radio via **`CONFIG_BT_CTLR_TX_PWR_ANTENNA`** in the board defconfig [`boards/NeurotechHub/Juxta5-8_nRF52840/Juxta5-8_nRF52840_defconfig`](boards/NeurotechHub/Juxta5-8_nRF52840/Juxta5-8_nRF52840_defconfig) (SoftDevice Controller; nearest legal level if HW differs). Net power at the **antenna** depends on matching and layout—this is not a regulatory certification setting.
  - **Re-advertises** after disconnect unless a **deep sleep** sequence has started (advertising restart is queued from the system workqueue when appropriate).
  - Single custom **128-bit service** `ad2a98a4-2148-4b58-9e14-7e2cbb6c7a01`:
    - **LED** `…7a02`: read + write (+ write-no-rsp); **1 byte** — `0` = LED off, non-zero = on (`DT_ALIAS(led0)`).
    - **Battery** `…7a03`: **read-only** — **little-endian `uint16` pack voltage (mV)** from the FUEL divider (`VBATT ≈ vfuel × 7.82`, same as blink). Each host read performs a single SAADC acquisition.
    - **Deep sleep** `…7a04`: **write only** (+ write-no-rsp). **`0x01`** begins the sleep sequence (**magnet away** ⇒ pad **high**/GPIO **inactive**, same **`gpio_pin_get_dt(magnet_sensor) == 0`** as blink inactive — magnet present ⇒ **abort**). Runs on a work queue: **`bt_le_adv_stop()`**, **`bt_conn_disconnect`** (reason **Remote Power Off**), brief delay, **`bt_disable()`**, `MAG_INT` **`GPIO_INPUT` + `GPIO_INT_LEVEL_LOW`** (wake when the pad goes **electrically LOW**, i.e. **magnet present**), **`sys_poweroff()`**. **`0x00` or other values** write successfully but **do nothing**. Wake causes a **cold boot** — pair/scan BLE again afterward.
  - **SAADC analog front-end**: `CONFIG_PM_DEVICE_RUNTIME` + Nordic SAADC driver turn the converter **on only around each `adc_read`** issued from the battery characteristic (minimal steady-state analog draw between reads—oversampling bursts still consume energy briefly per read).
  - RTT logs Bluetooth lifecycle, LED writes, battery sample failures, and deep-sleep sequencing.
- **Dependencies**: BLE peripheral, GPIO, SAADC/`zephyr,user` fuel channel (see DTS), **`sys_poweroff`** (`CONFIG_POWEROFF=y`), RTT (`CONFIG_PM_DEVICE_RUNTIME` enabled for SAADC idle). Assumes **SoftDevice Controller**; **TX power** (+8 dBm) is set in the **Juxta5-8_nRF52840** board defconfig when `CONFIG_BT` is enabled.
- **Reference links**:
  - [`Reference/nRF/applications/juxta-ble`](Reference/nRF/applications/juxta-ble)
  - [`Reference/nRF/samples/ble/peripheral`](Reference/nRF/samples/ble/peripheral)

### `applications/juxta5-8-ble-range` (Implemented)
- **Purpose**: Minimal two-device **BLE range** check. **Advertiser**: **non-connectable** legacy advertising only, local name **`JX_XXXXXX`** (same rule as production: last three bytes of the public identity as `%02X%02X%02X` after `JX_`). **Scanner**: passive scan bursts on a **~1 s** cadence; **LED on** if a matching name was seen in a **500 ms** window, **LED off** otherwise. **No** GATT, battery/SAADC, magnet, or deep sleep — flash and test.
- **Role**: Build-time under **“Juxta5-8 BLE range test”** (`CONFIG_JUXTA_BLE_RANGE_ROLE_ADVERTISER` vs `CONFIG_JUXTA_BLE_RANGE_ROLE_SCANNER`). Scanner optional **`CONFIG_JUXTA_RANGE_PEER_NAME`**: exact peer name; if empty, any **`JX_` + six hex digits** except the scanner’s own identity name. The app’s [`Kconfig`](applications/juxta5-8-ble-range/Kconfig) **must** `source` Zephyr’s `Kconfig.zephyr` first: if an application has a root `Kconfig` file, Zephyr sets `KCONFIG_ROOT` to it instead of `${ZEPHYR_BASE}/Kconfig`, so omitting that `source` breaks configuration (undefined `GPIO`, `BT`, etc.).
- **Build**:  
  `west build -b Juxta5-8_nRF52840 applications/juxta5-8-ble-range -d applications/juxta5-8-ble-range/build/juxta5-8-ble-range`  
  **Advertiser**: default `prj.conf` only. **Scanner**: add Kconfig fragment [`applications/juxta5-8-ble-range/scanner.conf`](applications/juxta5-8-ble-range/scanner.conf) in the nRF Connect build configuration, or from the repo root:  
  `west build ... -- -DEXTRA_CONF_FILE=applications/juxta5-8-ble-range/scanner.conf`  
  Uncomment / set `CONFIG_JUXTA_RANGE_PEER_NAME` in that file if you want an exact `JX_…` match.
- **Bring-up with host tools**: Use [`applications/juxta5-8-ble-test`](applications/juxta5-8-ble-test) if you need connectable GATT, FUEL read, or magnet-wake sleep.

### `applications/juxta5-8-prod` (Implemented)

Full design, GATT tables, state machine, and validation detail: **[`applications/juxta5-8-prod/README.md`](applications/juxta5-8-prod/README.md)**.

- **Purpose**: Production Hublink firmware. Combines BLE peripheral advertising/scanning, LIS2DH12 motion/temperature, battery monitoring, and append-only NOR CSV logging (JXS settings/events, JXV vitals, JXB BLE observations). Settings persist in nRF52840 internal flash (NVS). External NOR holds self-describing CSV pseudo-files recoverable by flash scan.
- **BLE**: Hublink service UUIDs preserved for iOS compatibility. Node and Gateway JSON use **camelCase only** (`firmwareVersion` must start with **`5.8`**, `batteryLevel`, `memoryLevel`, `deviceId`, `subjectId`, `experiment`, `advInterval`, `scanInterval`, `inactivityMultiplier`; gateway adds `timestamp`, `sendFilenames`, `clearMemory`, `reset`, `vitalsInterval`). Filename listing uses `name|size;…` wire format; file transfer concludes with a standalone **`EOF`** (3-byte) indication after CSV data chunks. MTU exchange and 4 s supervision timeout requested on connect.
- **Boot / shelf**: Fresh power-on → System OFF. Magnet wake → measure hold duration. **Sub-3 s holds are rejected** as false positives at every magnet detection site (`MAGNET_DEBOUNCE_MS`). **3 s ≤ t < 10 s** → normal wake (connectable adv, datetime sync required). **≥ 10 s** (`DFU_HOLD_THRESHOLD_MS`, raised from 7 s) → DFU (MCUboot SMP BLE when configured). Production magnet hold (not connected) ≥ 3 s → 5× blink → 5 s debounce → shelf; JXS gets a `shelf_entry` row before power-off. Shelf entry stops timers, disconnects BLE, powers down LIS2DH12, then `sys_poweroff()`. **NOR or LIS2DH12 init failure** → indefinite **long blink** (1 s on / 1 s off); production does not start. Debugger detected via `CoreDebug->DHCSR` and shelf/wake/DFU simulated in-band.
- **Production recovery boot**: After a silent reset (watchdog `DOG`, CPU `LOCKUP`, soft `SREQ` reboot) with NVS `op_mode == PROD`, the firmware restores the RTC from a retained-RAM (`.noinit`) snapshot (magic + version + CRC32) and re-enters production without a magnet swipe or gateway re-sync. The 1 s `k_timer` checkpoint keeps the snapshot accurate to ≤1 s. If the snapshot is invalid (cold boot with retained-RAM loss, partial write), the device falls through to datetime sync. Cold boot (`RESETREAS == 0`) still routes to shelf. Each recovery emits a `wdt_recovery_{dog,sreq,lockup,no_rtc}` row to JXS for audit at the next offload.
- **JXS lifecycle vocabulary**: every shelf-wake cycle records `shelf_exit` (once per boot, written as soon as the gateway supplies a valid clock), then every real BLE link-layer connection writes `user_connected` / `user_disconnected` pairs (the sync-gate `user_connected` is deferred until the clock is set so it doesn't get dropped). `time_set` follows each gateway timestamp write, `boot` marks production init complete, and `shelf_entry` marks an operator-initiated magnet-shelf return. Together those events read as a continuous device lifecycle in JXS.
- **Radio cadence**: Non-connectable adv and passive scan bursts **1 s** each; intervals **0–120 s** (0 = off) from NVS / Gateway.
- **Battery**: FUEL on P0.30/AIN6. Calibrated factor **7.96×** (was 7.82). Brownout at **2.75 V** → shelf + JXS `low_battery`; DFU access gated at **3.2 V**.
- **NOR layout**: `0x000000–0x00FFFF` JXS (64 KB), `0x010000–0x10FFFF` JXV (1 MB), `0x110000–0x3FFFFF` JXB (3 MB). Schema **`jxta-nor-csv-v5`**: JXS rows include `adv_interval_s`; each new **JXS** file auto-emits a `day_start` row (NVS snapshot via the standard JXS columns) so each day is interpretable from JXS alone. JXV/JXB are strict CSV (single header, data rows only). Calendar rollover creates JXS/JXV/JXB dated files for the same day.
- **Build**: `west build -b Juxta5-8_nRF52840 applications/juxta5-8-prod`
- **Deprecated**: ADC burst / electric mode removed entirely.
- **Dependencies**: BLE peripheral + observer + GATT client (MTU exchange), SPI NOR, NVS, LIS2DH12 SPI, SAADC P0.30/AIN6, watchdog, RTT.
- **Reference links**:
  - [`Reference/nRF/applications/juxta-ble`](Reference/nRF/applications/juxta-ble)
  - [`docs/JUXTA_NOR_Flash_Logging_Spec_v3.md`](docs/JUXTA_NOR_Flash_Logging_Spec_v3.md)

## Validation Plan

### `juxta5-8-blink` (defined now)

Manual checklist:
1. Program `applications/juxta5-8-blink` for board `Juxta5-8_nRF52840`.
2. Boot device with **no magnet** (`MAG_INT` **high**/inactive):
   - Expected: `LED0` toggles every 200 ms.
   - Expected RTT log: `MAG_INT is LOW (blink mode)` (means **inactive** / no magnet).

3. Bring **magnet** so `MAG_INT` is pulled **LOW** (active-low **present**):
   - Expected: `LED0` becomes steady ON within one loop cycle.
   - Expected RTT log: `MAG_INT is HIGH (LED forced ON)` (means **active** / magnet present).
4. Remove magnet again:
   - Expected: LED resumes 200 ms blinking.
5. Repeat magnet toggle at least 10 times:
   - Expected: deterministic transitions, no lockup, no GPIO errors in RTT.
6. With a battery connected:
   - Expected RTT every ~2 s: `FUEL vfuel=… mV vbatt=… mV soc~=…%` (values track supply; `soc` is a simple voltage ladder, not a fuel gauge).

Execution status:
- Checklist defined: yes
- Checklist executed on hardware: pending (requires board session)

### `juxta5-8-ble-test`

Manual checklist:
1. Build and flash `applications/juxta5-8-ble-test` for `Juxta5-8_nRF52840`.
2. With a phone or **nRF Connect**, scan: device **`Juxta5-8-BLE`** appears.
3. Connect; RTT shows `Connected` with a BLE address.
4. Open the custom service UUID `ad2a98a4-...-7a01`; write **`0x00`** / **`0x01`** to characteristic `...-7a02`; LED0 follows (off/on).
5. Read characteristic `…-7a03`; expect **two bytes LE** ≈ LiPo voltage in millivolts (same divider model as blink).
6. Disconnect; RTT shows `Disconnected` with a reason code.
7. Scan again: **`Juxta5-8-BLE`** should reappear (connectable advertising restarted).
8. Deep sleep (**use with care** — link drops and device **resets after wake**): ensure **`gpio_pin_get_dt(magnet)==0`** (magnet absent, pad idle **high**; otherwise firmware aborts with a WARN). Write **`0x01`** to characteristic `…-7a04`. Expect RTT `"Entering System OFF"`; BLE disappears. **Wake** when **`MAG_INT` falls LOW** (magnet present); cold boot then **`Juxta5-8-BLE`** advertising resumes.

Execution status:
- Checklist defined: yes
- Checklist executed on hardware: **done** (LED, battery read, advertise/reconnect, deep-sleep / magnet-wake validated)

### `juxta5-8-prod` validation checklist

See also the expanded checklist in [`applications/juxta5-8-prod/README.md`](applications/juxta5-8-prod/README.md#validation-checklist).

1. Build and flash `applications/juxta5-8-prod` for `Juxta5-8_nRF52840`.
2. RTT should log device ID (`JX_XXXXXX`), NVS settings load, and NOR log init. On the first sync of each shelf-wake boot, JXS records `shelf_exit`, then `user_connected`, then `time_set`; the post-disconnect production init then appends `boot`.
3. Connect with nRF Connect or the iOS companion app. Node characteristic should return JSON with `"firmwareVersion":"5.8.1"`, `"deviceId":"JX_…"`, and the settings keys above (`advInterval`, `scanInterval`, etc.).
4. Write gateway JSON `{"timestamp":1746000000}`. RTT logs timestamp accepted; `JXS` gets a `time_set` row (with `shelf_exit` and the deferred `user_connected` immediately preceding it on the first sync after boot).
5. Write gateway JSON `{"sendFilenames":true}`. Filename characteristic indication lists `JXS*.csv;JXV*.csv;JXB*.csv` with sizes.
6. Write a listed filename to the filename characteristic. File-transfer stream: CSV-only data indications (payload byte count equals listing **size**), then one final **`EOF`** (3-byte) indication — not `#EOF`/NOR terminator in the CSV payload.
7. Allow device to run one vitals period (default **60 s**, NVS `vitals_interval_s` / Gateway `vitalsInterval`). RTT logs vitals; `JXV` byte count grows. JXV/JXB carry only their column header and data rows; the NVS snapshot lives in the JXS `day_start` row at the top of each day's JXS file.
8. With another `JX_XXXXXX` device nearby, let a scan cycle complete. RTT logs peers; `JXB` rows are appended.
9. Write `{"clearMemory":true}`. NOR regions erase and fresh CSV files are created with a `memory_cleared` event. Internal settings survive (reconnect, read Node; `subjectId` and `experiment` unchanged).
10. Power cycle; RTT shows NVS settings reloaded and NOR log cache recovered without re-scanning flash.

### Later app validation placeholders

- `juxta5-8-axy`: implemented, runtime checklist pending hardware session
- `juxta5-8-mem`: implemented; destructive-from-offset-0 checklist pending hardware session
- `juxta5-8-ble-test`: implemented; BLE bring-up checklist executed on hardware
- `juxta5-8-ble-range`: implemented; advertiser/scanner + LED range check pending hardware session
- `juxta5-8-prod`: implemented; hardware + iOS validation pending board session
