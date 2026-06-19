# JUXTA File System Test Application

A comprehensive test suite for the JUXTA FRAM file system, designed to validate the `juxta_fram` and `juxta_framfs` libraries.

## Overview

This application provides a complete testing framework for:
- **FRAM Library** (`juxta_fram`) - Low-level FRAM device operations
- **File System** (`juxta_framfs`) - High-level file system with MAC indexing and data encoding

## Quick Start

### Prerequisites
- Nordic nRF Connect SDK v3.0.2+
- Juxta5-4_nRF52840 board with MB85RS1MT FRAM
- Zephyr development environment

## Practical Usage Examples

### Basic File Operations
```c
#include <juxta_framfs/framfs.h>

// Initialize file system
struct juxta_framfs_context fs_ctx;
juxta_framfs_init(&fs_ctx, &fram_dev);

// Create a daily log file
juxta_framfs_create_active(&fs_ctx, "240120", JUXTA_FRAMFS_TYPE_SENSOR_LOG);

// Append sensor data
uint8_t sensor_data[] = {0x12, 0x34, 0x56, 0x78};
juxta_framfs_append(&fs_ctx, sensor_data, sizeof(sensor_data));

// Seal file when done
juxta_framfs_seal_active(&fs_ctx);
```

### High-level Data Logging
```c
// Log device scan with MAC indexing (3-byte MAC IDs)
uint8_t mac_ids[][3] = {{0x55, 0x66, 0x77}}; // Last 3 bytes of MAC
int8_t rssi[] = {-45};
juxta_framfs_append_device_scan(&fs_ctx, 1234, 5, mac_ids, rssi, 1);

// Files are automatically named YYMMDD (e.g., "240120" for 2024-01-20)
```

### File System Statistics
```c
struct juxta_framfs_header stats;
juxta_framfs_get_stats(&fs_ctx, &stats);

printf("Files: %d/%d, Data: %d bytes, Next: 0x%06X\n",
       stats.file_count, JUXTA_FRAMFS_MAX_FILES,
       stats.total_data_size, stats.next_data_addr);
```

### Time-Aware API (Primary)
```c
/* Initialize with automatic time management */
struct juxta_framfs_context fs_ctx;
struct juxta_framfs_ctx ctx;

juxta_framfs_init(&fs_ctx, &fram_dev);
juxta_framfs_init_with_time(&ctx, &fs_ctx, get_rtc_date, true);

/* Automatic file management - data goes to correct daily file */
juxta_framfs_append_data(&ctx, sensor_data, sizeof(sensor_data));
juxta_framfs_append_device_scan_data(&ctx, minute, motion, mac_ids, rssi, count);
juxta_framfs_append_simple_record_data(&ctx, minute, JUXTA_FRAMFS_RECORD_TYPE_BOOT);
juxta_framfs_append_battery_record_data(&ctx, minute, 87);
```

## Configuration

### Board Configuration
The application uses the Juxta5-4_nRF52840 board configuration:
- **SPI Frequency**: 8MHz
- **CS Pin**: P1.13 (dedicated to FRAM)
- **FRAM Size**: 1Mbit (131,072 bytes)

### Test Modes
Modify `CURRENT_TEST_MODE` in `main.c`:
- `TEST_MODE_FRAM_ONLY` - Test FRAM library only
- `TEST_MODE_FRAMFS_ONLY` - Test file system only
- `TEST_MODE_FULL` - Complete test suite (default)
- `TEST_MODE_TIME_API` - Test new time-aware API
- `TEST_MODE_INTERACTIVE` - Interactive menu

## Performance Characteristics

- **Write Speed**: 200-250 KB/s
- **Read Speed**: 250-300 KB/s
- **Memory Overhead**: 1.57% of FRAM (1,293 bytes)
- **File Limit**: 64 files maximum
- **MAC ID Index**: 128 unique devices (3-byte format)
- **Daily Logging**: ~14.9 days of minute-based data
- **Filename Format**: YYMMDD (6 characters, e.g., "240120")

## Troubleshooting

### Common Issues

**FRAM Not Detected**
```
<err> fram_test: Failed to initialize FRAM: -1
```
- Check SPI connections and CS pin configuration
- Verify FRAM device ID in device tree

**File System Errors**
```
<err> framfs_test: Failed to create active file: -6
```
- File system may need formatting
- Check available space and file limits

**MAC Table Full**
```
<err> juxta_framfs: MAC table full
```
- Clear MAC table or reduce device count
- Maximum 128 unique MAC addresses

### Debug Output
Enable debug logging in `prj.conf`:
```
CONFIG_LOG_DEFAULT_LEVEL=4
CONFIG_LOG_LEVEL_DEBUG=y
```

## Development

### Adding New Tests
1. Create test function in `fram_test.c` or `framfs_test.c`
2. Add to test sequence in main function
3. Update test results display

### Customizing Test Data
Modify test data structures and parameters in test functions to match your application requirements.

## License

Apache-2.0 License - see LICENSE file for details.

## Support

For issues and questions:
- Check the library documentation in `lib/juxta_framfs/README.md`
- Review test output for specific error codes
- Verify hardware connections and device tree configuration 