# Juxta5-8 nRF52840 Firmware

This repository is the active Juxta5-8 firmware workspace. The `Reference/` directory is retained as historical source material and is not part of active implementation.

## Hardware Pin Map

| Signal | nRF Pin | Notes |
| --- | --- | --- |
| XL1 | `P0.00` | External oscillator |
| XL2 | `P0.01` | External oscillator |
| SPI MISO | `P0.08` | Shared SPI bus |
| LED0 | `P0.09` | Status LED |
| SPI SCK | `P0.12` | Shared SPI bus |
| ~RESET | `P0.18` | Reset pin |
| MAG_INT | `P0.25` | High when magnet is present |
| FUEL / AIN4 | `P0.28` | Battery divider output |
| ~AXY_CS | `P1.06` | LIS2DH12 chip select |
| SPI MOSI | `P1.09` | Shared SPI bus |
| ~AXY_INT1 | `P1.11` | Accelerometer interrupt 1 |
| ~CS_MEM | `P1.13` | NOR flash chip select |
| ANT | Fixed RF | BLE antenna (not assignable) |

## Hardware Notes

- **MCU**: Nordic nRF52840
- **Accelerometer**: LIS2DH12TR over SPI (reuse existing LIS2DH12 approach from reference applications)
- **External memory**: 32 Mbit SPI NOR flash (`C2939873.pdf` in `Reference/`)
- **Fuel divider**:
  - `Rtop = 1.5 MOhm` to `VBATT`
  - `Rbottom = 220 kOhm` to `GND`
  - Divider ratio: `0.12790698`
  - Conversion: `VBATT = VFUEL * 7.82`
  - Expected max VFUEL at 4.2V LiPo: about `0.537V`

## Software Constraints

- Only FUEL ADC sampling is planned; no electric mode.
- Memory code will target NOR flash behavior (not legacy FRAM behavior).

## Planned Application Structure

Bring-up and feature work are intentionally split into separate apps so each subsystem can be validated in isolation.

### `applications/juxta5-8-blink` (Phase 2)
- **Purpose**: Hardware sanity check for LED and magnet path.
- **Behavior**: LED0 blinks every 200 ms when `MAG_INT` is low; LED0 is forced on when `MAG_INT` is high.
- **Dependencies**: GPIO + logging only.
- **Reference links**:
  - [`Reference/nRF/applications/p0_05_test`](Reference/nRF/applications/p0_05_test)
  - [`Reference/nRF/applications/juxta-axy`](Reference/nRF/applications/juxta-axy)

### `applications/juxta5-8-axy` (Planned)
- **Purpose**: Validate LIS2DH12 SPI transactions and interrupt behavior on Juxta5-8 pins.
- **Dependencies**: SPI, GPIO interrupt handling, LIS2DH12 register layer.
- **Reference links**:
  - [`Reference/nRF/applications/juxta-axy`](Reference/nRF/applications/juxta-axy)
  - [`Reference/nRF/applications/juxta-ble/src/lis2dh12.c`](Reference/nRF/applications/juxta-ble/src/lis2dh12.c)

### `applications/juxta5-8-mem` (Planned)
- **Purpose**: Verify raw read/write/erase behavior against the 32 Mbit NOR flash.
- **Dependencies**: SPI NOR driver model and a simple test harness.
- **Reference links**:
  - [`Reference/C2939873.pdf`](Reference/C2939873.pdf)
  - [`Reference/nRF/applications/juxta-file-system`](Reference/nRF/applications/juxta-file-system)

### `applications/juxta5-8-ble-test` (Planned)
- **Purpose**: Basic BLE advertise/connect test with a characteristic to control LED0.
- **Dependencies**: BLE stack, GPIO integration, simple GATT service.
- **Reference links**:
  - [`Reference/nRF/applications/juxta-ble`](Reference/nRF/applications/juxta-ble)
  - [`Reference/nRF/samples/ble/peripheral`](Reference/nRF/samples/ble/peripheral)

### `applications/juxta5-8-juxta-ble` (Planned integration)
- **Purpose**: Start Juxta BLE port after all hardware bring-up apps are validated.
- **Dependencies**: BLE + accelerometer + memory + fuel path integration.
- **Reference links**:
  - [`Reference/nRF/applications/juxta-ble`](Reference/nRF/applications/juxta-ble)

## Validation Plan

### `juxta5-8-blink` (defined now)

Manual checklist:
1. Program `applications/juxta5-8-blink` for board `Juxta5-8_nRF52840`.
2. Boot device with no magnet present:
   - Expected: `LED0` toggles every 200 ms.
   - Expected RTT log: `MAG_INT is LOW (blink mode)`.
3. Present magnet so `MAG_INT` goes high:
   - Expected: `LED0` becomes steady ON within one loop cycle.
   - Expected RTT log: `MAG_INT is HIGH (LED forced ON)`.
4. Remove magnet again:
   - Expected: LED resumes 200 ms blinking.
5. Repeat magnet toggle at least 10 times:
   - Expected: deterministic transitions, no lockup, no GPIO errors in RTT.

Execution status:
- Checklist defined: yes
- Checklist executed on hardware: pending (requires board session)

### Later app validation placeholders

- `juxta5-8-axy`: pending details
- `juxta5-8-mem`: pending details
- `juxta5-8-ble-test`: pending details
- `juxta5-8-juxta-ble`: pending details
