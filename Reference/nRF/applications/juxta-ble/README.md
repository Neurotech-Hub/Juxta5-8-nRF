# JUXTA BLE Application

A power-efficient BLE application for the JUXTA device with consolidated data logging, motion detection, and pulsed advertising/scanning for reliable device discovery.

## Purpose

This application provides a complete IoT data logging solution with BLE connectivity, automatic daily file management, motion-based power optimization, and consolidated sensor data recording for long-term environmental monitoring.

## Board Overview

The application is designed for the Juxta5-1_AXY board with nRF52805, featuring FRAM storage, LIS2DH motion detection, and power-efficient BLE operation for battery-powered IoT deployments.

## Main Program Flow

The application operates on a state machine with multiple timing loops:

1. **Main State Machine**: Alternates between advertising and scanning bursts
   - Advertising burst: 100ms every 5 seconds (configurable)
   - Scanning burst: 500ms every 20 seconds (configurable)
   - Gateway advertising: 30 seconds when gateway detected

2. **Minute Timer**: Every minute, logs consolidated data to FRAM
   - Device scan results (MAC addresses, RSSI values)
   - Motion event count
   - Battery level reading
   - Temperature reading

3. **Motion Detection**: LIS2DH interrupt-driven motion detection
   - Resets power optimization when motion detected
   - Extends intervals when no motion for 1+ minutes

4. **BLE Connection Handling**: Pauses data logging during connections
   - Stops advertising/scanning when connected
   - Resumes normal operation when disconnected

## Pin Assignments

| Pin | Function | Direction | Notes |
|-----|----------|-----------|-------|
| P0.20 | FRAM CS | Output | SPI0 CS0, shared with LED |
| P0.21 | LIS2DH CS | Output | SPI0 CS1 |
| P0.22 | LIS2DH INT | Input | Motion detection interrupt |
| P0.23 | Magnet Sensor | Input | Optional wake-up source |
| P0.24 | SPI0 SCK | Output | SPI clock |
| P0.25 | SPI0 MOSI | Output | SPI data out |
| P0.26 | SPI0 MISO | Input | SPI data in |

## Data Logging and File System Integration

The application works in concert with the `juxta_framfs` library to create automatic daily files with consolidated sensor data:

### Daily File Creation
- **File Naming**: Automatic YYMMDD format (e.g., `240120` for January 20, 2024)
- **File Switching**: New file created automatically at midnight
- **Data Consolidation**: Every minute writes a single record containing:
  - Device scan results (0-128 devices)
  - Motion event count
  - Battery level (0-100%)
  - Temperature reading (°C)

### Record Structure
Each minute record uses the `juxta_framfs_device_record` structure:
```c
struct juxta_framfs_device_record {
    uint16_t minute;          /* 0-1439 for full day */
    uint8_t type;             /* Number of devices (0-128) or type cde */
    uint8_t motion_count;     /* Motion events this minute */
    uint8_t battery_level;    /* Battery level (0-100) */
    int8_t temperature;       /* Temperature in degrees Celsius */
    uint8_t mac_indices[128]; /* MAC address indices (0-127) */
    int8_t rssi_values[128];  /* RSSI values for each device */
} __packed;
```

### FRAM Memory Usage Analysis

The 1 Mbit FRAM (128 KB) provides the following storage capacity based on daily device discovery rates:

| Daily Device Count | Avg Devices/Min | Record Size | Daily File Size | Days Until Full | Usage Scenario |
|-------------------|-----------------|-------------|-----------------|-----------------|----------------|
| **10 devices/day** | 0.007/min | 6.1 bytes | 8.8 KB | **14.7 days** | Very low activity |
| **100 devices/day** | 0.069/min | 6.8 bytes | 9.8 KB | **13.2 days** | Low activity |
| **1,000 devices/day** | 0.694/min | 13.4 bytes | 19.3 KB | **6.7 days** | Moderate activity |
| **No devices** | 0/min | 6.0 bytes | 8.6 KB | **15.0 days** | Battery/temp only |

**Memory Breakdown:**
- **Total FRAM**: 131,072 bytes (1 Mbit)
- **File System Overhead**: ~2,000 bytes (headers, MAC table, settings)
- **Available for Data**: ~129,072 bytes
- **Daily Records**: 1,440 minutes per day

**Storage Calculation:**
- **Base record size**: 6 bytes (time, motion, battery, temperature)
- **Per device**: 2 bytes (MAC index + RSSI)
- **Daily file size**: (avg record size × 1,440 minutes) + system events
- **System events**: ~10 bytes/day (BOOT, CONNECTED, ERROR records)

**Real-World Usage Patterns:**
- **10 devices/day**: Typical for isolated deployments
- **100 devices/day**: Common in moderate traffic environments  
- **1,000 devices/day**: High-traffic areas or dense deployments

### System Events
Additional 3-byte records are logged for system events:
- **BOOT**: Device startup
- **CONNECTED**: BLE connection established
- **ERROR**: System errors

## Essential Usage Examples

### Basic Initialization Pattern
```
1. Initialize vitals library with battery monitoring
2. Initialize FRAM file system with device
3. Initialize time-aware file system with RTC function
4. Set up automatic daily file management
```

### Minute-by-Minute Data Logging Pattern
```
Every minute:
1. Get current minute of day from RTC
2. Read battery level and temperature sensors
3. If devices were scanned:
   - Convert scan results to packed format
   - Log consolidated record with device data
4. If no devices found:
   - Log consolidated record with battery/temperature only
5. Update last logged minute timestamp
```

### Motion-Based Power Optimization Pattern
```
Motion detection:
1. Increment motion counter on interrupt
2. Reset power optimization flag
3. Start motion processing timer

Minute timer check:
1. If motion detected this minute:
   - Reset motion counter
   - Use default power intervals
2. If no motion detected:
   - Switch to extended power intervals (2x)
```

### BLE State Machine Pattern
```
Main state machine loop:
1. Check if time for advertising burst
2. Check if time for scanning burst
3. Handle gateway advertising priority
4. Start appropriate BLE operation
5. Set timer for burst duration
6. Calculate next action timing with randomization
```

### Device Scanning and Discovery Pattern
```
BLE scan callback:
1. Parse device name from advertisement
2. If JUXTA peripheral device (JX_XXXXXX):
   - Extract MAC ID from name
   - Add to scan table with RSSI
3. If JUXTA gateway device (JXGA_XXXX):
   - Set gateway advertising flag
   - Trigger connectable advertising mode
```

## BLE Service

### Service UUID: 0x1234
- **LED Control**: 0x1235 (Read/Write)
- **Device Info**: 0x1236 (Read)
- **Settings**: 0x1237 (Read/Write)

### LED Control Values
- `0x00`: LED OFF
- `0x01`: LED ON 