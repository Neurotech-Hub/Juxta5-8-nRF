# JUXTA-AXY Application

**Accelerometer Playground for Juxta5-1_AXY Boards**

This application demonstrates LIS2DH12 accelerometer communication, magnet sensor interrupt handling, and LED control in a low-power environment. It's designed as an accelerometer playground based on the `juxta-mvp` application.

## Features

- **LIS2DH12 Accelerometer Support**: WHO_AM_I register verification and communication testing
- **Magnet Sensor Interrupt**: Low-power sleep until magnet sensor triggers wake-up
- **LED Control**: Visual feedback for system activity
- **Low-Power Design**: Efficient sleep/wake cycles
- **STMicroelectronics Library Integration**: Uses official LIS2DH12 library with Zephyr wrapper

## Hardware Requirements

- **Board**: Juxta5-1_AXY (Accelerometer variant)
- **Accelerometer**: LIS2DH12TR
- **Magnet Sensor**: Hall effect sensor on P0.12
- **LED**: Status LED on P0.20

## Pin Configuration

| Pin | Function | Description |
|-----|----------|-------------|
| P0.04 | Accelerometer INT | Interrupt pin from LIS2DH12 |
| P0.05 | Accelerometer CS | SPI Chip Select for LIS2DH12 |
| P0.12 | Magnet Sensor | Hall effect sensor interrupt |
| P0.14 | SPI MISO | SPI data input |
| P0.16 | SPI SCK | SPI clock |
| P0.18 | SPI MOSI | SPI data output |
| P0.20 | LED | Status LED (no longer shared with FRAM) |

## Key Differences from juxta-mvp

- ✅ **Keeps**: Magnet sensor interrupt, LED control, low-power sleep
- ❌ **Removes**: FRAM functionality, ADC functionality  
- ➕ **Adds**: LIS2DH12 accelerometer support

## Application Flow

1. **Initialize**: Magnet sensor, LED, and LIS2DH12 accelerometer
2. **Test**: Verify LIS2DH12 WHO_AM_I communication
3. **Sleep**: Enter low-power mode until magnet sensor interrupt
4. **Wake**: On magnet sensor trigger:
   - Test accelerometer communication
   - Flash LED to indicate activity
   - Return to sleep

## Building and Flashing

### Quick Build
```bash
./build_axy.sh
```

### Manual Build
```bash
west build -b Juxta5-1_AXY applications/juxta-axy
west flash
```

### Monitor Output
```bash
west monitor
```

## Expected Output

```
[00:00:00.123] <inf> main: Starting JUXTA-AXY Application v1.0
[00:00:00.124] <inf> main: Accelerometer playground with LIS2DH12 support
[00:00:00.125] <inf> juxta_axy_example: Starting JUXTA-AXY Low-Power Accelerometer Example
[00:00:00.126] <inf> juxta_axy_example: Board: Juxta5-1_AXY (Accelerometer variant)
[00:00:00.200] <inf> juxta_axy_example: Magnet sensor initialized on pin 12
[00:00:00.201] <inf> juxta_axy_example: LED initialized on pin 20
[00:00:00.250] <inf> lis2dh12_zephyr: LIS2DH12 initialized: freq=8000000 Hz, CS=P0.05, INT=P0.04
[00:00:00.300] <inf> lis2dh12_zephyr: ✅ LIS2DH12 WHO_AM_I verified: 0x33
[00:00:00.301] <inf> juxta_axy_example: LIS2DH12 accelerometer initialized successfully
[00:00:00.400] <inf> juxta_axy_example: ✅ LIS2DH12 communication verified - Device ID: 0x33
[00:00:00.500] <inf> juxta_axy_example: 🔋 Entering low-power mode
[00:00:00.501] <inf> juxta_axy_example: 🧲 Trigger the magnet sensor (P0.12) to wake the device
[00:00:00.502] <inf> juxta_axy_example: 🚀 Each wake-up will test LIS2DH12 WHO_AM_I communication
```

## Future Development

### Accelerometer Playground Extensions
- [ ] Configure LIS2DH12 for continuous data acquisition
- [ ] Implement interrupt-based acceleration event detection
- [ ] Add accelerometer data logging capabilities
- [ ] Test different power modes and sampling rates
- [ ] Implement motion detection algorithms

### FRAM Integration
This application is designed to be easily extended with FRAM functionality:

```c
// Future integration example
#ifdef CONFIG_JUXTA_FRAM
    #include <juxta_fram/fram.h>
    
    // In handle_magnet_event():
    fram_store_event(magnet_event_count, accel_data);
#endif
```

## Troubleshooting

### Common Issues

1. **Build Errors**
   - Ensure you're building with `Juxta5-1_AXY` board
   - Check SPI driver is enabled in `prj.conf`

2. **Accelerometer Not Responding**
   - Verify SPI wiring (P0.14, P0.16, P0.18, P0.05)
   - Check power supply to LIS2DH12
   - Ensure correct SPI mode configuration

3. **WHO_AM_I Verification Fails**
   - Check LIS2DH12 chip select (P0.05)
   - Verify SPI communication settings
   - Ensure device is properly powered

### Debug Configuration

Enable debug logging in `prj.conf`:
```
CONFIG_LOG_DEFAULT_LEVEL=4
CONFIG_SPI_LOG_LEVEL_DBG=y
CONFIG_GPIO_LOG_LEVEL_DBG=y
```

## Library Architecture

```
applications/juxta-axy/
├── src/
│   ├── main.c                 # Application entry point
│   ├── juxta_axy_example.c    # Main application logic
│   ├── lis2dh12_zephyr.c      # LIS2DH12 Zephyr integration
│   └── lis2dh12_zephyr.h      # Integration header
├── CMakeLists.txt             # Build configuration
├── prj.conf                   # Project configuration
└── build_axy.sh               # Build script
```

## License

Apache-2.0 - Copyright (c) 2024 NeurotechHub 