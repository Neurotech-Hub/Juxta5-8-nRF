# JUXTA FRAM File System Library

A lightweight, append-only file system for FRAM storage with automatic daily file management and consolidated data logging.

## Purpose

This library provides persistent storage for time-series data with automatic daily file creation, MAC address indexing, and consolidated record formats that include device scan data, motion events, battery levels, and temperature readings in a single efficient structure.

## Board Overview

The library is designed for nRF52-based devices with FRAM storage, providing efficient data logging for IoT applications that need to track device interactions, environmental conditions, and system vitals over extended periods.

## Main Program Flow

The library operates on a minute-by-minute cycle:

1. **Time Management**: Uses RTC timestamp to determine current minute of day (0-1439)
2. **File Management**: Automatically creates new daily files (YYMMDD format) when date changes
3. **Data Consolidation**: Every minute, collects all sensor data into a single record:
   - Device scan results (MAC addresses, RSSI values)
   - Motion event count
   - Battery level reading
   - Temperature reading
4. **Storage**: Appends consolidated record to current daily file
5. **MAC Indexing**: Maintains global MAC address table to minimize storage overhead

## Pin Assignments

| Pin | Function | Direction | Notes |
|-----|----------|-----------|-------|
| P0.20 | FRAM CS | Output | SPI0 CS0 |
| P0.21 | LIS2DH CS | Output | SPI0 CS1 |
| P0.22 | LIS2DH INT | Input | Motion detection interrupt |
| P0.24 | SPI0 SCK | Output | SPI clock |
| P0.25 | SPI0 MOSI | Output | SPI data out |
| P0.26 | SPI0 MISO | Input | SPI data in |

## Essential Usage Examples

### Basic Initialization
```c
struct juxta_framfs_context fs_ctx;
struct juxta_framfs_ctx ctx;

juxta_framfs_init(&fs_ctx, &fram_dev);
juxta_framfs_init_with_time(&ctx, &fs_ctx, get_rtc_date, true);
```

### Minute-by-Minute Data Logging
```c
/* Every minute, log consolidated data */
uint16_t minute = juxta_vitals_get_minute_of_day(&vitals_ctx);
uint8_t battery_level = juxta_vitals_get_battery_percent(&vitals_ctx);
int8_t temperature = juxta_vitals_get_temperature(&vitals_ctx);

if (device_count > 0) {
    /* Log device scan with sensor data */
    juxta_framfs_append_device_scan_data(&ctx, minute, motion_count,
                                         battery_level, temperature,
                                         mac_ids, rssi_values, device_count);
} else {
    /* Log no activity with sensor data */
    juxta_framfs_append_device_scan_data(&ctx, minute, motion_count,
                                         battery_level, temperature,
                                         NULL, NULL, 0);
}
```

### System Event Logging
```c
/* Log system events */
juxta_framfs_append_simple_record_data(&ctx, minute, JUXTA_FRAMFS_RECORD_TYPE_BOOT);
juxta_framfs_append_simple_record_data(&ctx, minute, JUXTA_FRAMFS_RECORD_TYPE_CONNECTED);
```

### User Settings Management
```c
/* Get/set advertising interval */
uint8_t adv_interval;
juxta_framfs_get_adv_interval(&ctx, &adv_interval);
juxta_framfs_set_adv_interval(&ctx, 5);

/* Get/set subject ID */
char subject_id[16];
juxta_framfs_get_subject_id(&ctx, subject_id);
juxta_framfs_set_subject_id(&ctx, "vole001");
```

## Record Structure

The consolidated record format includes all sensor data:

```c
struct juxta_framfs_device_record {
    uint16_t minute;          /* 0-1439 for full day */
    uint8_t type;             /* Number of devices (0-128) */
    uint8_t motion_count;     /* Motion events this minute */
    uint8_t battery_level;    /* Battery level (0-100) */
    int8_t temperature;       /* Temperature in degrees Celsius */
    uint8_t mac_indices[128]; /* MAC address indices (0-127) */
    int8_t rssi_values[128];  /* RSSI values for each device */
} __packed;
```

## Memory Layout

```
0x0000: FileSystemHeader (13 bytes)
0x000D: FileEntry[0-63] (1,280 bytes)
0x050D: Global MAC Index (772 bytes)
0x07C9: User Settings (36 bytes)
0x07ED: File data starts here
``` 