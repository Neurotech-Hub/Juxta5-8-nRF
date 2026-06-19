# Juxta5 nRF Project

## AI Directives (see .cursor/rules for current version)
1. Never try to build my applications: I exclusively build using VS Code's nRF extension.
2. If you do not know how to do something, stop and request assistance with an example.
3. Do not make any git actions on my behalf; you may use git status and git diff anytime to see what has changed (most commits represent a working version so any changes likely depart from working).
4. Err on the side of strategizing first for complex problems that need my input. Present strategies in a concise and technical form. Always ask me questions (if unclear) before coding.
6. Do not alter readme files until I have told you a build is tested and complete.
7. Make minimal changes so we can quickly iterate on errors and avoid continuing in the wrong direction. Let's make small changes and test often.

### Project Scope & Organization
1. Our sample applications are in ./applications. These will be explicitly referred to when working.
2. Our customs boards are in ./boards.
3. Our custom libraries are in ./lib.
4. Samples I've collected from the Zephyr project are in ./samples. You must refer to these first before coding.

#### BLE
BLE should observe other devices and obtain their MAC and RSSI. These will have a filter applied so we only focus on devices with a specific advertisement name (eg, "JUXTA_XXXX" where XXXX will be the last four characters of that devices MAC address). These devices will also need to connect to a gateway to retrieve settings and perform transfer data. Advertising will occur at a fixed rate whereas observing will need to be duty cycles (eg, once every 30s with a scan duration of 5s).

#### Memory
We will be writing to a MB85RS1M FRAM memory chip. We have developed a hardware layer in ./lib/juxta_fram. We will utilize a file system layer in ./lib/juxta_framfs.

#### Vitals & Other Utilities
We are making ./lib/juxta_vitals_nrf52 to support other critical functions:
1. We will need to read the device voltage using the internal ADC to measure VDD.
2. We will need the ability to write to, and read the RTC. We will get the current time over BLE, but this library should only work on the RTC level and accept a unix timestamp to set the RTC (it shall not interact directly with BLE).

## Setup Instructions

1. Install the nRF Connect SDK and set up your development environment following the [official guide](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/getting_started.html).

2. Clone this repository:
```bash
git clone <your-repo-url>
cd Juxta5
```

3. Open the project in VS Code:
   - Open VS Code
   - File -> Open Workspace from File...
   - Select `Juxta5.code-workspace`
   - Install recommended extensions when prompted

4. Install required tools:
   ```bash
   # Install nrfutil (required for debugging)
   python3 -m pip install nrfutil
   
   # Verify installation
   nrfutil version
   ```
   Note: If you get a "command not found" error after installation, you may need to add Python's bin directory to your PATH.

5. The nRF Connect extension should automatically detect:
   - Your applications in `/applications`
   - Custom board definitions in `/boards`
   - Build configurations from `prj.conf` and other `.conf` files

6. Build an application:
   - Use the nRF Connect extension in VS Code
   - Or use the provided build scripts:
     ```bash
     # For juxta-mvp (original working application)
     ./applications/juxta-mvp/build_adc.sh
     
     # For juxta-file-system (FRAM library testing)
     ./applications/juxta-file-system/build.sh
     ```

7. Connect to RTT Server (Terminal):
   ```bash
   # Install JLinkRTTClient if not already installed
   brew install segger-jlink   # macOS
   
   # Connect to RTT Server
   JLinkRTTClient
   ```
   Note: Ensure your device is flashed and JLink is connected before running RTT Client.

## Project Structure

- `/applications` - Application code
  - `juxta-mvp` - **Original working application** (direct SPI, stable baseline)
  - `juxta-file-system` - **FRAM library testing application** (validates libraries)
  - `juxta-ble` - **Bluetooth application** (device discovery and gateway connectivity)
  - `juxta-axy` - Accelerometer application
- `/boards` - Custom board definitions
- `/lib` - Shared libraries
  - `juxta_fram` - FRAM driver library
  - `juxta_framfs` - FRAM file system library
  - `juxta_vitals_nrf52` - Vitals monitoring library (ADC, RTC)

## Applications Overview

### 🎯 `juxta-mvp` (Stable Baseline)
- **Purpose**: Original working application with direct SPI implementation
- **Status**: ✅ **Stable and tested**
- **Use case**: Baseline for comparison, known working state
- **FRAM**: Uses direct SPI commands, bypasses library

### 🧪 `juxta-file-system` (Library Testing)
- **Purpose**: Comprehensive testing of FRAM libraries
- **Status**: ✅ **Ready for testing**
- **Use case**: Validate `juxta_fram` and `juxta_framfs` libraries
- **Features**: Full test suite, performance metrics, file system validation

### 📱 `juxta-ble` (Bluetooth Device Discovery & Gateway Connectivity)
- **Purpose**: BLE device discovery, scanning, and gateway connectivity
- **Status**: ✅ **Core functionality implemented and tested**
- **Features**:
  - **Device Discovery**: Scans for other JUXTA devices with MAC-based filtering
  - **Dynamic Naming**: Device names generated as "JX_XXXXXX" based on MAC address
  - **Gateway Detection**: Recognizes gateway devices advertising as "JXGA_XXXX"
  - **Smart Advertising**: 
    - Normal bursts: Non-connectable for energy efficiency
    - Gateway-triggered: 30-second connectable windows for file transfer
  - **State Management**: Pauses during connections, resumes after disconnection
  - **10-minute Cooldown**: Limits gateway advertising to once per 10 minutes
  - **MTU Optimization**: Supports 247-byte MTU for efficient data transfer
- **Advertising Strategy**:
  - **Normal Operation**: 5-second non-connectable advertising bursts
  - **Gateway Response**: 30-second connectable advertising when gateway detected
  - **Connection Persistence**: Maintains connections without state machine interference

### 📊 Other Applications
- `juxta-axy` - Accelerometer sensor integration

## Build Configurations

Multiple build configurations are available:
- `prj.conf` - Default configuration
- `debug.conf` - Debug configuration with additional logging
- `test_build.conf` - Minimal test configuration
- `prj_with_fram_lib.conf` - Configuration with FRAM library support

## Development Workflow

### For New Development:
1. **Start with `juxta-mvp`** - Verify hardware works
2. **Test with `juxta-file-system`** - Validate libraries work
3. **Build your application** - Use validated libraries

### For Library Testing:
1. **Build `juxta-file-system`** - Comprehensive library validation
2. **Monitor RTT output** - Detailed test results
3. **Verify all tests pass** - Libraries are ready for use

### For BLE Development:
1. **Build `juxta-ble`** - Core BLE functionality
2. **Test device discovery** - Verify scanning and filtering work
3. **Test gateway connectivity** - Verify connectable advertising and file transfer

## Development Notes

- The project uses the nRF52840 SoC
- Custom board definitions are provided for Juxta5-1_ADC and Juxta5-1_AXY variants
- Build artifacts are stored in the `build` directory (ignored by git)
- VS Code workspace settings ensure consistent development environment
- **FRAM Libraries**: `juxta_fram` and `juxta_framfs` are available for applications
- **BLE Features**: Device discovery, gateway connectivity, and file transfer capabilities implemented

## Coding Agents

Utilize Zephyr examples here: /opt/nordic/ncs/v3.0.2/zephyr/samples/bluetooth/