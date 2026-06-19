# Testing Guide for Juxta5-1_ADC Board

This guide explains how to build and test the `juxta-mvp` application with the `Juxta5-1_ADC` board.

## Prerequisites

1. **nRF Connect SDK environment** properly set up
2. **CMake** installed and available in PATH
3. **J-Link** or other SWD programmer connected to your board
4. **Juxta5-1_ADC hardware** with proper connections

## Board Hardware Requirements

Ensure your Juxta5-1_ADC board has the following connections:

| Pin         | Function      | Connection                      |
| ----------- | ------------- | ------------------------------- |
| P0.00/P0.01 | 32kHz Crystal | Low frequency crystal           |
| P0.04       | AIN4          | ADC negative input              |
| P0.05       | AIN5          | ADC positive input              |
| P0.12       | GPIO          | Magnet sensor interrupt         |
| P0.14       | MISO          | SPI Master In Slave Out         |
| P0.16       | SCK           | SPI Serial Clock                |
| P0.18       | MOSI          | SPI Master Out Slave In         |
| P0.20       | CS/LED        | FRAM Chip Select & LED (shared) |
| P0.21       | RESET         | System reset                    |
| G1          | SWDCLK        | SWD debug clock                 |
| F1          | SWDIO         | SWD debug data                  |

## Building the Application

### Option 1: Using the Build Script
```bash
# From the nRF root directory
./applications/juxta-mvp/build_adc.sh
```

### Option 2: Manual Build
```bash
# From the nRF root directory
west build -b Juxta5-1_ADC applications/juxta-mvp
```

### Option 3: Clean Build
```bash
# From the nRF root directory
rm -rf build
west build -b Juxta5-1_ADC applications/juxta-mvp
```

## Testing Options

The application supports two different testing modes:

### 1. Original Example Application (Default)
**What it tests:**
- Custom blink driver
- Example sensor driver
- Basic LED blinking functionality

**How to use:**
- Build normally (default configuration)
- Uses custom drivers from the `common/` directory
- Tests the general nRF Connect SDK framework

### 2. Board-Specific Example (Juxta5-1_ADC Testing)
**What it tests:**
- ADC differential measurement (P0.04/P0.05)
- SPI FRAM communication
- GPIO interrupt handling (magnet sensor)
- Shared LED/CS pin management

**How to enable:**
1. Edit `applications/juxta-mvp/src/main.c`
2. Uncomment the line: `#define USE_BOARD_SPECIFIC_EXAMPLE`
3. Rebuild the application

## Flashing and Running

### Flash to Hardware
```bash
west flash
```

### Debug Mode
```bash
west debug
```

### Monitor Output
Since the board has no UART, use one of these methods:
- **SWD debugging** with GDB
- **J-Link RTT** for real-time logging
- **J-Link RTT Viewer** for live log output

## Expected Behavior

### Original Example (Default)
- LED blinks at varying rates
- Sensor triggers change LED blink speed
- Console output shows sensor values

### Board-Specific Example
- **ADC readings** every second (differential voltage on P0.04/P0.05)
- **LED blinks** on P0.20 (when not using FRAM)
- **Magnet sensor** interrupt triggers on P0.12
- **FRAM test** can be enabled by commenting out LED usage

## Testing Each Peripheral

### 1. ADC Testing
- **Apply voltage** between P0.04 and P0.05
- **Check log output** for ADC readings in mV
- **Expected range**: ~3.6V full scale with 1/6 gain

### 2. FRAM Testing
- **Enable FRAM test** in code (comment out LED usage)
- **Check log output** for "FRAM test passed" message
- **SPI signals** should be visible on oscilloscope

### 3. GPIO Interrupt Testing
- **Toggle P0.12** to ground (active LOW)
- **Check log output** for "Magnet sensor interrupt triggered!"
- **No external pull-up needed** (driven line)

### 4. LED Testing
- **LED should blink** on P0.20
- **Note**: Cannot use LED and FRAM simultaneously

## Troubleshooting

### Build Issues
```bash
# Check west configuration
west config --list

# Update west
west update

# Clean build
rm -rf build
```

### Flash Issues
```bash
# Check programmer connection
nrfjprog --ids

# Recover board
nrfjprog --recover
```

### Debug Issues
```bash
# Check debug configuration
west debug --cmake-only

# Use J-Link RTT
JLinkRTTClient
```

### Hardware Issues
1. **No ADC readings**: Check voltage connections to P0.04/P0.05
2. **No SPI activity**: Check SPI wiring (MOSI, MISO, SCK, CS)
3. **No interrupts**: Check P0.12 connection and signal levels
4. **No LED**: Check P0.20 connection and CS conflict

## Performance Expectations

### Resource Usage
- **Flash**: ~150KB (plenty of room in 192KB)
- **RAM**: ~20KB (within 24KB limit)
- **Build time**: ~30 seconds

### Power Consumption
- **Active**: ~5-10mA (depending on peripherals)
- **Sleep**: <1mA (if power management enabled)

## Next Steps

1. **Test basic functionality** with original example
2. **Enable board-specific testing** to verify all peripherals
3. **Develop your application** using the working peripheral configurations
4. **Add power management** for low-power operation
5. **Implement your sensor algorithms** using the ADC/FRAM infrastructure

## Support

For issues specific to:
- **Board definition**: Check `boards/NeurotechHub/Juxta5-1_ADC/README.md`
- **Application code**: Check source files in `applications/juxta-mvp/src/`
- **Build system**: Check `applications/juxta-mvp/CMakeLists.txt` and `prj.conf` 