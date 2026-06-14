# FRAM Library Migration Guide

This guide shows how to integrate the JUXTA FRAM Library into your existing nRF Connect SDK project.

## Step 1: Project Structure Setup

Add the library files to your project:

```
nRF/
├── lib/                          # ← CREATE THIS
│   ├── CMakeLists.txt           # ← ADD THIS
│   ├── Kconfig                  # ← ADD THIS  
│   └── juxta_fram/              # ← ADD THIS DIRECTORY
│       ├── CMakeLists.txt
│       ├── Kconfig
│       ├── README.md
│       ├── include/
│       │   └── juxta_fram/
│       │       └── fram.h
│       └── src/
│           └── fram.c
├── CMakeLists.txt               # ← UPDATE THIS
├── Kconfig                      # ← UPDATE THIS
└── applications/
    └── your-app/
        ├── prj.conf             # ← UPDATE THIS
        └── src/
            └── main.c           # ← UPDATE THIS
```

## Step 2: Root Configuration Files

### CMakeLists.txt (root level)
```cmake
# JUXTA nRF Project
cmake_minimum_required(VERSION 3.13.1)

# Include the library directory
add_subdirectory(lib)
```

### Kconfig (root level)
```
# JUXTA nRF Project Configuration

# Source the libraries configuration
source "lib/Kconfig"

# Standard Zephyr configuration
menu "Zephyr"
source "Kconfig.zephyr"
endmenu
```

## Step 3: Application Configuration

### Update prj.conf
Add these lines to your application's `prj.conf`:
```
# Enable JUXTA FRAM Library
CONFIG_JUXTA_FRAM=y
CONFIG_JUXTA_FRAM_LOG_LEVEL_DBG=y
```

### Update CMakeLists.txt (Application Level)
Your application `CMakeLists.txt` remains mostly the same:
```cmake
cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(app LANGUAGES C)

target_sources(app PRIVATE 
    src/main.c
    src/your_source.c
)

# Library will be automatically linked when CONFIG_JUXTA_FRAM=y
```

## Step 4: Code Migration

### Before (Direct SPI Code)
```c
// Old direct SPI implementation
#include <zephyr/drivers/spi.h>

static int test_fram(void)
{
    const struct device *spi_dev = DEVICE_DT_GET(DT_BUS(FRAM_NODE));
    
    struct spi_config spi_cfg = {
        .frequency = 8000000,
        .operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
        .slave = DT_REG_ADDR(FRAM_NODE),
        .cs = {
            .gpio = {
                .port = led.port,
                .pin = led.pin,
                .dt_flags = led.dt_flags
            },
            .delay = 0
        }
    };

    // Manual WREN command
    uint8_t tx_wren = 0x06;
    const struct spi_buf tx_buf_wren = {.buf = &tx_wren, .len = 1};
    const struct spi_buf_set tx_wren_set = {.buffers = &tx_buf_wren, .count = 1};
    
    int ret = spi_write(spi_dev, &spi_cfg, &tx_wren_set);
    if (ret < 0) return ret;
    
    k_usleep(30);

    // Manual write command
    uint8_t tx_write[] = {0x02, 0x00, 0x00, 0x00, 0xAA};
    const struct spi_buf tx_buf_write = {.buf = tx_write, .len = sizeof(tx_write)};
    const struct spi_buf_set tx_write_set = {.buffers = &tx_buf_write, .count = 1};
    
    ret = spi_write(spi_dev, &spi_cfg, &tx_write_set);
    // ... more manual SPI code
}
```

### After (FRAM Library)
```c
// New library-based implementation
#include <juxta_fram/fram.h>

static struct juxta_fram_device fram_dev;

static int test_fram(void)
{
    int ret;
    
    // Simple initialization
    ret = juxta_fram_init_dt(&fram_dev, DEVICE_DT_GET(FRAM_NODE), &led);
    if (ret < 0) return ret;
    
    // Simple write operation
    ret = juxta_fram_write_byte(&fram_dev, 0x000000, 0xAA);
    if (ret < 0) return ret;
    
    // Simple read operation  
    uint8_t read_data;
    ret = juxta_fram_read_byte(&fram_dev, 0x000000, &read_data);
    if (ret < 0) return ret;
    
    // Built-in verification
    return juxta_fram_test(&fram_dev, 0x1000);
}
```

## Step 5: Device Tree Compatibility

The library works with your existing device tree configuration. Ensure you have:

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
        led0 = &led_0;
    };
};
```

## Step 6: Build and Test

1. **Clean build**:
   ```bash
   rm -rf build
   west build -b Juxta5-1_ADC applications/your-app
   ```

2. **Flash and test**:
   ```bash
   west flash
   ```

3. **Check logs** for FRAM library messages:
   ```
   [00:00:00.123,456] <inf> juxta_fram: FRAM initialized: freq=8000000 Hz, CS=P0.20
   [00:00:00.234,567] <inf> juxta_fram: FRAM ID verified successfully
   [00:00:00.345,678] <inf> juxta_fram: FRAM test passed: wrote 0xAA, read 0xAA
   ```

## Step 7: Benefits After Migration

### Code Reduction
- **Before**: ~200 lines of SPI setup and command handling
- **After**: ~20 lines using library functions

### Error Handling
- **Before**: Manual error checking for each SPI operation
- **After**: Comprehensive error codes with descriptive logging

### Maintainability
- **Before**: SPI details scattered throughout application
- **After**: Clean API, library handles all SPI complexity

### Reusability
- **Before**: Copy/paste SPI code between applications
- **After**: Same library works across all applications

## Common Migration Issues

### 1. Missing Configuration
**Error**: `undefined reference to juxta_fram_init`
**Solution**: Add `CONFIG_JUXTA_FRAM=y` to `prj.conf`

### 2. Device Tree Mismatch
**Error**: `FRAM ID verification failed`
**Solution**: Check device tree alias `spi-fram` points to correct node

### 3. CS Pin Conflict
**Error**: FRAM operations fail when LED is used
**Solution**: This is expected - P0.20 is shared. Use one at a time.

### 4. Build System Issues
**Error**: `No rule to make target 'lib/CMakeLists.txt'`
**Solution**: Ensure root `CMakeLists.txt` includes `add_subdirectory(lib)`

## Testing the Migration

Create a simple test to verify everything works:

```c
#include <juxta_fram/fram.h>

static struct juxta_fram_device fram_dev;
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

int test_migration(void)
{
    int ret;
    
    // Test initialization
    ret = juxta_fram_init_dt(&fram_dev, DEVICE_DT_GET(DT_ALIAS(spi_fram)), &led);
    if (ret != JUXTA_FRAM_OK) {
        printk("❌ FRAM init failed: %d\n", ret);
        return ret;
    }
    printk("✅ FRAM initialized\n");
    
    // Test device ID
    ret = juxta_fram_read_id(&fram_dev, NULL);
    if (ret != JUXTA_FRAM_OK) {
        printk("❌ FRAM ID check failed: %d\n", ret);
        return ret;
    }
    printk("✅ FRAM ID verified\n");
    
    // Test read/write
    ret = juxta_fram_test(&fram_dev, 0x1000);
    if (ret != JUXTA_FRAM_OK) {
        printk("❌ FRAM test failed: %d\n", ret);
        return ret;
    }
    printk("✅ FRAM test passed\n");
    
    printk("🎉 Migration successful!\n");
    return 0;
}
```

## Next Steps

1. **Remove old SPI code** once library is working
2. **Update documentation** to reference library usage
3. **Share library** across other applications in your project
4. **Consider extending library** for application-specific needs 