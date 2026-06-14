# JUXTA Vitals Library for nRF52

A comprehensive vitals monitoring library for nRF52 devices, providing battery voltage monitoring, temperature sensing, and RTC time management.

## Features

- **Battery Monitoring**: Real-time battery voltage measurement and percentage calculation
- **Temperature Sensing**: Internal temperature monitoring
- **RTC Integration**: Unix timestamp management with date/time conversion
- **File System Integration**: Functions designed for seamless integration with file systems
- **Error Handling**: Comprehensive error codes and validation

## Quick Start

```c
#include "juxta_vitals_nrf52/vitals.h"

/* Initialize vitals monitoring */
struct juxta_vitals_ctx vitals;
juxta_vitals_init(&vitals, true);  // Enable battery monitoring

/* Set current time */
juxta_vitals_set_timestamp(&vitals, 1705752000);  // 2024-01-20 12:00:00

/* Get vitals data */
uint8_t battery = juxta_vitals_get_battery_percent(&vitals);
int8_t temp = juxta_vitals_get_temperature(&vitals);
uint32_t date = juxta_vitals_get_date_yyyymmdd(&vitals);
```

## File System Integration

The library provides specialized functions for easy integration with file systems:

### Date and Time Functions

```c
/* Get date for file naming (YYMMDD format) */
uint32_t file_date = juxta_vitals_get_file_date(&vitals);

/* Get date in legacy YYYYMMDD format if needed */
uint32_t full_date = juxta_vitals_get_date_yyyymmdd(&vitals);

/* Get minute of day for time-series records (0-1439) */
uint16_t minute = juxta_vitals_get_minute_of_day(&vitals);
```

### Battery Functions with Validation

```c
/* Validate battery level for storage */
bool is_valid = juxta_vitals_validate_battery_level(85);  // true
bool is_invalid = juxta_vitals_validate_battery_level(150); // false

/* Get validated battery level for file system storage */
uint8_t battery_level;
int ret = juxta_vitals_get_validated_battery_level(&vitals, &battery_level);
if (ret == 0) {
    // battery_level is valid (0-100) and ready for storage
}
```

### Integration Example

```c
/* Initialize both libraries */
struct juxta_vitals_ctx vitals;
struct juxta_framfs_context fs_ctx;
struct juxta_framfs_ctx time_ctx;

juxta_vitals_init(&vitals, true);
juxta_framfs_init(&fs_ctx, &fram_dev);

/* Initialize time-aware file system with vitals RTC function */
juxta_framfs_init_with_time(&time_ctx, &fs_ctx, 
                           juxta_vitals_get_file_date, true);

/* Write battery data with automatic validation */
uint8_t battery_level;
if (juxta_vitals_get_validated_battery_level(&vitals, &battery_level) == 0) {
    juxta_framfs_append_battery_record_data(&time_ctx, 
                                           juxta_vitals_get_minute_of_day(&vitals),
                                           battery_level);
}
```

## API Reference

### Core Functions

- `juxta_vitals_init()` - Initialize vitals monitoring
- `juxta_vitals_update()` - Update all vitals readings
- `juxta_vitals_get_summary()` - Get text summary of all vitals

### Battery Functions

- `juxta_vitals_get_battery_mv()` - Get battery voltage in millivolts
- `juxta_vitals_get_battery_percent()` - Get battery percentage (0-100)
- `juxta_vitals_is_low_battery()` - Check if battery is low
- `juxta_vitals_validate_battery_level()` - Validate battery level for storage
- `juxta_vitals_get_validated_battery_level()` - Get validated battery level

### RTC Functions

- `juxta_vitals_set_timestamp()` - Set current Unix timestamp
- `juxta_vitals_get_timestamp()` - Get current Unix timestamp
- `juxta_vitals_get_date_yyyymmdd()` - Get date in YYYYMMDD format
- `juxta_vitals_get_time_hhmmss()` - Get time in HHMMSS format
- `juxta_vitals_get_file_date()` - Get date in YYMMDD format for file system operations
- `juxta_vitals_get_file_date_yymmdd()` - Get date in YYMMDD format (explicit)
- `juxta_vitals_get_minute_of_day()` - Get minute of day (0-1439)

### System Functions

- `juxta_vitals_get_uptime()` - Get system uptime in seconds
- `juxta_vitals_get_temperature()` - Get internal temperature

## Configuration

The library can be configured via Kconfig:

```kconfig
CONFIG_JUXTA_VITALS_NRF52=y
CONFIG_JUXTA_VITALS_NRF52_LOG_LEVEL=3
```

## Error Codes

```c
#define JUXTA_VITALS_OK 0
#define JUXTA_VITALS_ERROR_INIT -1
#define JUXTA_VITALS_ERROR_NOT_READY -2
#define JUXTA_VITALS_ERROR_INVALID_PARAM -3
#define JUXTA_VITALS_ERROR_HARDWARE -4
```

## License

Apache-2.0 License - see LICENSE file for details. 