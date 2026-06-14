# JUXTA FRAM Library

A lightweight, reusable library for communicating with MB85RS1MT FRAM memory devices over SPI in nRF Connect SDK applications.

## Overview

This library provides a clean, easy-to-use API for FRAM operations, extracted from the original implementation in `juxta5_example.c`. It's designed to be shared across multiple applications and boards within the JUXTA project.

**🔗 Special Feature**: Built-in support for shared CS/LED pin functionality, allowing safe switching between FRAM operations and LED control on the same pin.

## Features

- **Device Tree Integration**: Automatic configuration from device tree
- **Error Handling**: Comprehensive error codes and logging  
- **Device Verification**: Automatic ID verification against expected values
- **Memory Safety**: Address bounds checking
- **Clean API**: Simple, intuitive function interface
- **Built-in Testing**: Test functions for verification
- **🆕 Shared Pin Management**: Safe LED control on the CS pin when not using FRAM
- **✅ Verified Performance**: 200-250 KB/s write, 250-300 KB/s read speeds

## Supported Hardware

- **FRAM Chip**: MB85RS1MT (1Mbit/128KB)
- **Interface**: SPI (up to 8MHz)
- **nRF Chips**: nRF52805, nRF52832, nRF52840 (any with SPI support)
- **Special**: Shared CS/LED pin functionality (JUXTA boards use P0.20)

## Quick Start

### 1. Enable the Library

Add to your application's `prj.conf`:
```ini
# Enable JUXTA FRAM Library
CONFIG_JUXTA_FRAM=y
CONFIG_JUXTA_FRAM_LOG_LEVEL_DBG=y

# Required memory settings
CONFIG_MAIN_STACK_SIZE=4096
CONFIG_HEAP_MEM_POOL_SIZE=4096
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=2048

# For proper logging with float support
CONFIG_LOG=y
CONFIG_LOG_DEFAULT_LEVEL=3
CONFIG_CBPRINTF_FP_SUPPORT=y
CONFIG_LOG_BACKEND_FORMAT_TIMESTAMP=y
```

### 2. Direct Initialization (Recommended)

Since device tree configuration requires specific board setup, use direct initialization:

```c
#include <juxta_fram/fram.h>
#include <zephyr/drivers/spi.h>

/* GPIO specifications */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static struct juxta_fram_device fram_dev;

int main(void)
{
    int ret;
    
    /* Get SPI device by label */
    const struct device *spi_dev = device_get_binding("SPI_0");
    if (!spi_dev) {
        printk("Failed to get SPI device\n");
        return -1;
    }
    
    /* Initialize FRAM with direct parameters */
    ret = juxta_fram_init(&fram_dev, spi_dev, 1000000, &led); /* 1MHz SPI */
    if (ret < 0) {
        printk("FRAM init failed: %d\n", ret);
        return ret;
    }
    
    /* Verify device ID */
    struct juxta_fram_id id;
    ret = juxta_fram_read_id(&fram_dev, &id);
    if (ret < 0) {
        printk("Failed to verify FRAM ID: %d\n", ret);
        return ret;
    }
    
    /* Write data */
    uint8_t data[] = {0x12, 0x34, 0x56, 0x78};
    ret = juxta_fram_write(&fram_dev, 0x1000, data, sizeof(data));
    if (ret < 0) {
        printk("Write failed: %d\n", ret);
        return ret;
    }
    
    /* Read data back */
    uint8_t read_data[4];
    ret = juxta_fram_read(&fram_dev, 0x1000, read_data, sizeof(read_data));
    if (ret < 0) {
        printk("Read failed: %d\n", ret);
        return ret;
    }
    
    /* Use LED functionality */
    ret = juxta_fram_led_mode_enable(&fram_dev);
    ret = juxta_fram_led_on(&fram_dev);
    k_sleep(K_MSEC(500));
    ret = juxta_fram_led_off(&fram_dev);
    
    return 0;
}
```

### 3. Device Tree Configuration (Advanced)

For advanced users with custom device tree setup:

```dts
&spi0 {
    fram0: fram@0 {
        compatible = "jedec,spi-nor";
        reg = <0>;
        spi-max-frequency = <8000000>;
        size = <DT_SIZE_K(128)>;
        has-dpd;
        jedec-id = [04 7F 27];
    };
};

/ {
    aliases {
        spi-fram = &fram0;
    };
};
```

**Note**: Device tree initialization (`juxta_fram_init_dt`) is not available in the current implementation. Use direct initialization instead.

## API Reference

### Data Structures

```c
struct juxta_fram_device {
    const struct device *spi_dev;
    struct spi_config spi_cfg;
    struct gpio_dt_spec cs_gpio;  /* For LED control */
    bool initialized;
    bool led_mode;  /* Current pin mode */
};

struct juxta_fram_id {
    uint8_t manufacturer_id;    /* 0x04 (Fujitsu) */
    uint8_t continuation_code;  /* 0x7F */
    uint8_t product_id_1;       /* 0x27 (1Mbit) */
    uint8_t product_id_2;       /* 0x03 */
};
```

### Core Functions

#### Initialization
```c
int juxta_fram_init_dt(struct juxta_fram_device *fram_dev, 
                       const struct device *fram_node,
                       const struct gpio_dt_spec *cs_spec);
```
Initialize FRAM from device tree configuration.

#### Device Verification
```c
int juxta_fram_read_id(struct juxta_fram_device *fram_dev, 
                       struct juxta_fram_id *id);
```
Read and verify device ID matches expected MB85RS1MT values.

#### Data Operations
```c
int juxta_fram_write(struct juxta_fram_device *fram_dev,
                     uint32_t address, const uint8_t *data, size_t length);

int juxta_fram_read(struct juxta_fram_device *fram_dev,
                    uint32_t address, uint8_t *data, size_t length);
```
Write/read data to/from FRAM at specified address.

#### Convenience Functions
```c
int juxta_fram_write_byte(struct juxta_fram_device *fram_dev,
                          uint32_t address, uint8_t data);

int juxta_fram_read_byte(struct juxta_fram_device *fram_dev,
                         uint32_t address, uint8_t *data);
```
Single byte operations.

#### Testing
```c
int juxta_fram_test(struct juxta_fram_device *fram_dev, uint32_t test_address);
```
Comprehensive test including ID verification and read/write validation.

### 🆕 LED Helper Functions (Shared Pin)

#### Mode Management
```c
int juxta_fram_led_mode_enable(struct juxta_fram_device *fram_dev);
int juxta_fram_led_mode_disable(struct juxta_fram_device *fram_dev);
bool juxta_fram_is_led_mode(struct juxta_fram_device *fram_dev);
```
Switch between SPI CS mode and GPIO LED mode.

#### LED Control
```c
int juxta_fram_led_on(struct juxta_fram_device *fram_dev);
int juxta_fram_led_off(struct juxta_fram_device *fram_dev);
int juxta_fram_led_toggle(struct juxta_fram_device *fram_dev);
int juxta_fram_led_set(struct juxta_fram_device *fram_dev, bool state);
```
Control LED state. Pin must be in LED mode first.

### Error Codes

| Code                    | Value | Description             |
| ----------------------- | ----- | ----------------------- |
| `JUXTA_FRAM_OK`         | 0     | Success                 |
| `JUXTA_FRAM_ERROR`      | -1    | General error           |
| `JUXTA_FRAM_ERROR_INIT` | -2    | Initialization error    |
| `JUXTA_FRAM_ERROR_ID`   | -3    | Device ID mismatch      |
| `JUXTA_FRAM_ERROR_ADDR` | -4    | Invalid address         |
| `JUXTA_FRAM_ERROR_SPI`  | -5    | SPI communication error |
| `JUXTA_FRAM_ERROR_MODE` | -6    | Wrong pin mode error    |

## 🔗 Shared CS/LED Pin Usage

### How It Works

The JUXTA boards share pin P0.20 between FRAM CS and LED functionality. The library manages this safely:

1. **SPI Mode** (default): Pin controlled by SPI driver for FRAM operations
2. **LED Mode**: Pin controlled as GPIO output for LED operations
3. **Automatic Switching**: FRAM operations auto-switch from LED to SPI mode
4. **Safe Transitions**: Mode switching is handled internally

### Usage Patterns

#### Pattern 1: Explicit Mode Control
```c
/* FRAM operations (SPI mode) */
juxta_fram_write(&fram_dev, addr, data, len);
juxta_fram_read(&fram_dev, addr, data, len);

/* Switch to LED mode */
juxta_fram_led_mode_enable(&fram_dev);
juxta_fram_led_on(&fram_dev);
k_sleep(K_MSEC(500));
juxta_fram_led_off(&fram_dev);

/* Switch back to SPI mode */
juxta_fram_led_mode_disable(&fram_dev);
juxta_fram_write(&fram_dev, addr2, data2, len2);
```

#### Pattern 2: Automatic Mode Switching
```c
/* Enable LED mode */
juxta_fram_led_mode_enable(&fram_dev);
juxta_fram_led_on(&fram_dev);

/* FRAM operation automatically switches to SPI mode */
juxta_fram_write(&fram_dev, addr, data, len);  /* Auto-switches mode */

/* Can continue with LED operations by re-enabling LED mode */
juxta_fram_led_mode_enable(&fram_dev);
juxta_fram_led_off(&fram_dev);
```

#### Pattern 3: Mixed Operations
```c
for (int i = 0; i < 10; i++) {
    /* Store data in FRAM */
    juxta_fram_write_byte(&fram_dev, 0x1000 + i, i);
    
    /* Flash LED to indicate progress */
    juxta_fram_led_mode_enable(&fram_dev);
    juxta_fram_led_toggle(&fram_dev);
    k_sleep(K_MSEC(100));
}
```

### Error Handling

```c
/* This will fail if not in LED mode */
int ret = juxta_fram_led_on(&fram_dev);
if (ret == JUXTA_FRAM_ERROR_MODE) {
    printk("Need to enable LED mode first\n");
    juxta_fram_led_mode_enable(&fram_dev);
    juxta_fram_led_on(&fram_dev);  /* Now works */
}
```

## Memory Layout

- **Total Size**: 128KB (131,072 bytes)
- **Address Range**: 0x000000 to 0x01FFFF
- **Page Size**: No page restrictions (byte-addressable)
- **Write Endurance**: 10^13 cycles

## Integration Guide

### CMakeLists.txt
Update your application's `CMakeLists.txt`:
```cmake
target_sources(app PRIVATE 
    src/main.c
    src/your_app.c
)

# Library will be automatically linked when CONFIG_JUXTA_FRAM=y
```

### Project Structure
```
your_project/
├── lib/
│   └── juxta_fram/          # This library
├── applications/
│   └── your_app/
│       ├── prj.conf         # Enable CONFIG_JUXTA_FRAM=y
│       └── src/
│           └── main.c       # Use juxta_fram API
└── boards/
    └── your_board/
        └── board.dts        # Configure FRAM device
```

## Examples

### Basic FRAM Operations
See `applications/juxta-mvp/src/fram_library_example.c` for comprehensive usage examples including:
- String storage and retrieval
- Binary data operations
- Periodic counter storage
- Error handling patterns

### Shared Pin Operations
See `applications/juxta-mvp/src/fram_led_example.c` for LED/FRAM sharing examples including:
- Mode switching demonstrations
- Mixed operation patterns
- Error handling
- Continuous operation examples

## Performance Notes

- **Write Speed**: 200-250 KB/s (verified through testing)
- **Read Speed**: 250-300 KB/s (verified through testing)
- **SPI Frequency**: Up to 8MHz (library enforces this limit)
- **CS Timing**: 30µs delay between transactions (configurable)
- **Memory Requirements**:
  - Stack: 4KB minimum
  - Heap: 4KB recommended
  - System workqueue: 2KB
- **Mode Switching**: ~10µs overhead for pin reconfiguration

## Troubleshooting

### Common Issues

1. **Stack Overflow**: Ensure `CONFIG_MAIN_STACK_SIZE=4096`
2. **SPI Not Ready**: Ensure SPI driver is enabled in `prj.conf`
3. **CS Pin Conflict**: Remember P0.20 is shared with LED on JUXTA boards
4. **Device ID Mismatch**: Check SPI wiring and power supply
5. **Address Out of Range**: Library checks bounds automatically
6. **LED Mode Error**: Must enable LED mode before LED operations
7. **Float Formatting**: Enable `CONFIG_CBPRINTF_FP_SUPPORT=y` for proper logging

### Debug Configuration
```
CONFIG_JUXTA_FRAM_LOG_LEVEL_DBG=y
CONFIG_SPI_LOG_LEVEL_DBG=y
CONFIG_GPIO_LOG_LEVEL_DBG=y
```

### Pin Sharing Debug
```c
/* Check current mode */
if (juxta_fram_is_led_mode(&fram_dev)) {
    printk("Currently in LED mode\n");
} else {
    printk("Currently in SPI mode\n");
}
```

## Migration from Direct SPI Code

If you have existing direct SPI FRAM code:

1. Replace SPI setup with `juxta_fram_init_dt()`
2. Replace manual command sequences with library functions
3. Update error handling to use library error codes
4. Remove device ID verification code (library handles this)
5. **NEW**: Use LED helper functions instead of manual GPIO control

### Before (Direct SPI + Manual GPIO)
```c
// Manual SPI setup, WREN commands, error-prone
uint8_t tx_write[] = {0x02, 0x00, 0x10, 0x00, 0xAA};
spi_write(spi_dev, &spi_cfg, &tx_write_set);

// Manual LED control with conflicts
gpio_pin_configure_dt(&led_gpio, GPIO_OUTPUT);
gpio_pin_set_dt(&led_gpio, 1);  // May conflict with SPI
```

### After (Library)
```c
// Clean, safe API
juxta_fram_write_byte(&fram_dev, 0x1000, 0xAA);

// Safe LED control
juxta_fram_led_mode_enable(&fram_dev);
juxta_fram_led_on(&fram_dev);
```

## License

Apache-2.0 - Copyright (c) 2024 NeurotechHub 