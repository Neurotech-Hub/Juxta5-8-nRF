# JUXTA NOR Flash Logging Specification v3

## Overview

The device stores experimental data in append-only pseudo-files on onboard NOR flash.

Goals:

- Human-readable CSV exports
- Simple firmware implementation
- Append-only writes
- No in-place editing
- Survive firmware/schema changes
- Support subject/device remapping
- Support offline operation
- Enable straightforward downstream analysis
- Make raw flash dumps human-readable and recoverable

Storage medium:

- 32 Mb NOR flash
- 4 MB usable storage

---

# Flash Layout

## Partition Scheme

| Region | Purpose | Size |
|---|---|---|
| 0x000000 - 0x00FFFF | Metadata / config | 64 KB |
| 0x010000 - 0x01FFFF | JXS settings/events | 64 KB |
| 0x020000 - 0x11FFFF | JXV vitals log | 1024 KB |
| 0x120000 - 0x3FFFFF | JXB BLE observations | 3008 KB |

### Rationale

- Vitals are predictable and low bandwidth
- BLE observations are variable and dominate storage in dense environments
- Settings/events are tiny but critical for experiment provenance
- Current operating settings are cached in config memory for fast access

---

# File Types

The system exposes pseudo-files during transfer/export.

Each file is append-only.

Each file physically stores:

1. Filename
2. CSV header
3. Data rows
4. EOF marker

This fully embraces a CSV-in-flash architecture.

Advantages:

- Self-describing logs
- No schema reconstruction
- No schema registry logic
- Easier debugging
- Easier firmware migration
- Easier raw flash recovery

---

# File Structure

Example physical flash contents:

```text
JXV20260507.csv
unix,motion,batt_v,temp_c
1715200000,12,3.81,24
1715200060,14,3.80,24
#EOF
```

---

# 1. Vitals Log (`JXV`)

## Filename

```text
JXVYYYYMMDD.csv
```

Example:

```text
JXV20260507.csv
```

## Schema

```csv
unix,motion,batt_v,temp_c
```

## Example Rows

```csv
1715200000,12,3.81,24
1715200060,14,3.80,24
1715200120,13,3.80,24
```

## Field Definitions

| Field | Description |
|---|---|
| unix | Unix timestamp in seconds |
| motion | Motion count (0–255) |
| batt_v | Battery voltage as decimal volts |
| temp_c | Integer temperature in Celsius |

## Temperature Source

The LIS2DH12TR temperature sensor is 8-bit resolution.

Temperature is stored as integer Celsius.

Example:

```text
24 = 24°C
```

## Logging Rate

```text
1 row per minute
```

---

# 2. BLE Observation Log (`JXB`)

## Filename

```text
JXBYYYYMMDD.csv
```

Example:

```text
JXB20260507.csv
```

## Schema

```csv
unix,observer_id,peer_id,rssi
```

## Example Rows

```csv
1715200000,JX_9B10A1,JX_3FA2B7,-68
1715200030,JX_9B10A1,JX_2C77D4,-74
```

## Field Definitions

| Field | Description |
|---|---|
| unix | Unix timestamp in seconds |
| observer_id | Device performing scan |
| peer_id | Observed BLE device ID |
| rssi | RSSI in dBm |

## Logging Behavior

A row is created for every observed BLE advertisement match.

If multiple peers are observed during a scan window, multiple rows are written.

## BLE Identity Rules

BLE advertising names represent stable hardware identities.

Required format:

```text
JX_000000
```

Where:

```text
000000 = last 6 hex characters of BLE MAC address
```

Example:

```text
JX_3FA2B7
```

Advantages:

- Deterministic
- Stable
- Human-readable
- Derived directly from hardware identity
- No stored lookup table required

## Important Design Rule

`subject_id` is NEVER used as BLE advertising name.

Reason:

- Devices may be swapped between subjects during experiments
- Hardware identity must remain stable across subject reassignment
- Prevents BLE name caching ambiguity
- Preserves historical continuity

## Scan Rate Assumption

```text
1 scan every 30 seconds
average 1 peer observed per scan
```

---

# 3. Settings / Events Log (`JXS`)

## Filename

```text
JXSYYYYMMDD.csv
```

Example:

```text
JXS20260507.csv
```

## Purpose

Tracks:

- Subject assignment
- Experiment assignment
- Firmware version
- Configuration changes
- Mode changes
- System events
- Time changes
- Experiment provenance

This file is critical for downstream analysis.

## Schema

```csv
unix,event,device_id,subject_id,experiment,fw_version,mode,scan_interval_s,vitals_interval_s,ble_name
```

## Example Rows

```csv
1715200000,boot,JX_9B10A1,mouse_07,social_v1,1.2.3,normal,30,60,JX_9B10A1
1715220000,settings_changed,JX_9B10A1,mouse_07,social_v1,1.2.3,dense_scan,10,60,JX_9B10A1
1715260000,subject_changed,JX_9B10A1,mouse_08,social_v1,1.2.3,normal,30,60,JX_9B10A1
```

## Logged Events

Recommended events:

```text
boot
reset
settings_changed
mode_changed
subject_changed
firmware_updated
user_connected
user_disconnected
time_set
low_battery
```

## Subject ID Rules

`subject_id` must never be empty.

Default behavior:

```text
subject_id = device_id
```

Example default assignment:

```text
device_id  = JX_3FA2B7
subject_id = JX_3FA2B7
```

This guarantees:

- Every row always maps to a subject
- No null subject identifiers
- Unassigned devices still produce valid datasets

## Current Settings Cache

Current operating settings are additionally cached in the config region for fast access.

The JXS log serves as the historical append-only provenance record.

---

# Metadata / Directory Index

A small append-safe metadata region tracks pseudo-files.

## Directory Entry Fields

```text
filename
log_type
start_addr
length_bytes
start_unix
end_unix
flags
```

## Example

```text
JXV20260507.csv,V,0x020000,45210,1715126400,1715212799,closed
JXB20260507.csv,B,0x120000,66320,1715126400,1715212799,closed
```

---

# File Creation Rules

New pseudo-files are created when:

```text
date changes
schema changes
mode requires different columns
```

Examples:

```text
JXV20260507.csv
JXV20260508.csv
JXB20260507.csv
JXS20260507.csv
```

## Schema Change Behavior

If schema changes occur and today's file already exists:

- The existing file for today is erased
- Writing restarts at the beginning of today's file region
- Previous-day files remain untouched

This avoids complex schema migration logic.

---

# Write Behavior

## Rules

```text
append-only
no in-place edits
no row deletion
no row mutation
```

## EOF Marker

Closed files terminate with:

```text
#EOF
```

Advantages:

- Easier recovery
- Easier debugging
- Prevents stale flash ambiguity
- Simplifies export termination

## Advantages

```text
simple firmware
power-loss resilience
minimal flash wear
easy recovery
schema flexibility
```

---

# Export Behavior

During transfer:

```text
pseudo-files are reconstructed directly from flash regions
```

Because headers are physically stored in flash, exports require minimal transformation logic.

## Export Path Structure

During transfer/export, reconstructed files should be organized as:

```text
experiment/subject_id/filename
```

Example:

```text
social_v1/mouse_07/JXV20260507.csv
```

This structure is intended to simplify downstream analysis pipelines.

---

# Timekeeping Rules

## Primary Format

```text
Unix time (seconds)
```

## Time Source Events

Clock updates are logged using:

```text
time_set
```

Example:

```csv
1715200000,time_set,JX_9B10A1,mouse_07,social_v1,1.2.3,normal,30,60,JX_9B10A1
```

---

# Identity Model

## Device Identity

Stable hardware identifier.

Required format:

```text
JX_000000
```

Example:

```text
JX_3FA2B7
```

## Subject Identity

Experimental assignment.

Example:

```text
mouse_07
```

## Experiment Identity

Experiment grouping string.

Example:

```text
social_v1
```

## Identity Architecture

The BLE layer is device-centric.

The analysis layer is subject-centric.

BLE observations always store hardware device identities.

Subject mappings are reconstructed offline using timestamped JXS records.

## Analysis Join Strategy

Analysis software joins:

```text
JXB observer_id/peer_id
→ JXS device_id
→ subject_id
using timestamp windows
```

This allows:

- Device swapping without data corruption
- Stable BLE identities
- Reliable historical reconstruction
- Clean downstream analysis workflows
