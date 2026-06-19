# Juxta5-1_AXY Board Support

This document describes the Juxta5-1_AXY board support for the nRF Connect SDK.

## Board Overview

The Juxta5-1_AXY is a sensor-focused board based on the nRF52805 SoC designed for low-power applications with magnetic field sensing and accelerometer measurements.

### Key Features
- **SoC**: nRF52805-CAAA-R (24KB RAM, 192KB Flash)
- **Crystal**: 32.768 kHz low-frequency crystal
- **Storage**: SPI FRAM memory
- **Sensors**: Magnet sensor with interrupt capability
- **Accelerometer**: SPI-based motion sensor with interrupt
- **Debugging**: SWD interface (no UART)

## Pin Assignments

| Pin   | Function | Direction | Notes                                |
| ----- | -------- | --------- | ------------------------------------ |
| P0.00 | LFXTAL1  | -         | Low frequency crystal                |
| P0.01 | LFXTAL2  | -         | Low frequency crystal                |
| P0.04 | GPIO     | Input     | Accelerometer interrupt              |
| P0.05 | CS       | Output    | Accelerometer chip select            |
| P0.12 | GPIO     | Input     | Magnet sensor interrupt (active LOW) |
| P0.14 | MISO     | Input     | SPI Master In Slave Out              |
| P0.16 | SCK      | Output    | SPI Serial Clock                     |
| P0.18 | MOSI     | Output    | SPI Master Out Slave In              |
| P0.20 | CS/LED   | Output    | **Shared**: FRAM Chip Select and LED |
| P0.21 | RESET    | Input     | System reset                         |
| G1    | SWDCLK   | -         | SWD debug clock                      |
| F1    | SWDIO    | -         | SWD debug data                       |

## Device Tree Configuration

### SPI FRAM Configuration
```dts
&spi0 {
	status = "okay";
	cs-gpios = <&gpio0 20 GPIO_ACTIVE_LOW>;
	
	fram0: fram@0 {
		compatible = "atmel,at25";
		reg = <0>;
		spi-max-frequency = <8000000>;
		size = <DT_SIZE_K(32)>;
		pagesize = <64>;
		address-width = <16>;
		timeout = <5>;
		label = "FRAM0";
	};
};
```

### Accelerometer Configuration
```dts
&spi0 {
	accel0: accel@1 {
		compatible = "adi,adxl362";
		reg = <1>;
		spi-max-frequency = <8000000>;
		int1-gpios = <&gpio0 4 GPIO_ACTIVE_LOW>;
		label = "ACCEL0";
	};
};
```

### GPIO Interrupt Configuration
```dts
gpio_keys {
	compatible = "gpio-keys";
	magnet_sensor: magnet_sensor {
		gpios = <&gpio0 12 GPIO_ACTIVE_LOW>;
		label = "Magnet sensor interrupt";
	};
	accel_int: accel_int {
		gpios = <&gpio0 4 GPIO_ACTIVE_LOW>;
		label = "Accelerometer interrupt";
	};
};
```

## Building Applications

### Basic Build
```bash
west build -b Juxta5-1_AXY applications/my-app
```

### With Board Overlay
```bash
west build -b Juxta5-1_AXY applications/my-app -- -DDTC_OVERLAY_FILE=boards/Juxta5-1_AXY.overlay
```

## Programming and Debugging

### Programming
```bash
west flash
```

### Debugging
Since there's no UART, use SWD debugging:
```bash
west debug
```

### Debug Tools Supported
- **J-Link**: Primary debugging interface
- **PyOCD**: Alternative debugging interface
- **OpenOCD**: Alternative debugging interface

## Application Example

See `applications/juxta-mvp/src/juxta5_example.c` for a complete example demonstrating:
- SPI FRAM read/write operations
- ADC differential measurements
- GPIO interrupt handling
- Shared LED/CS pin management

## Usage Notes

### Shared LED/CS Pin (P0.20)
**Important**: Pin P0.20 is shared between the LED and FRAM chip select. You must manage this carefully in your application:

- **When using FRAM**: The SPI driver automatically controls the CS signal
- **When using LED**: Manually control the GPIO pin
- **Cannot use both simultaneously**: Choose one function at a time

### Example Usage Pattern
```c
/* Option 1: Use as LED */
gpio_pin_set_dt(&led, 1);  // Turn on LED
gpio_pin_set_dt(&led, 0);  // Turn off LED

/* Option 2: Use as FRAM CS (handled by SPI driver) */
eeprom_write(fram_dev, addr, data, len);
eeprom_read(fram_dev, addr, data, len);
```

### Power Considerations
- The nRF52805 has limited flash (192KB) and RAM (24KB)
- Optimize your application for low power consumption
- Use the 32kHz crystal for accurate timing

### ADC Voltage Range
- **Input range**: Depends on gain setting (currently 1/6 gain)
- **Reference**: Internal 0.6V reference
- **Effective range**: ~3.6V full scale with 1/6 gain
- **Resolution**: 12-bit with 8x oversampling

## Troubleshooting

### Common Issues

1. **SPI Communication Fails**
   - Check that P0.20 is not being used as LED simultaneously
   - Verify SPI wiring (MOSI, MISO, SCK, CS)
   - Check SPI frequency (max 8MHz for FRAM)

2. **ADC Readings Invalid**
   - Verify differential input connections (P0.04, P0.05)
   - Check voltage levels are within range
   - Ensure proper grounding

3. **Magnet Sensor Interrupt Not Working**
   - Verify P0.12 is connected properly
   - Check that interrupt is configured as active LOW
   - Ensure magnet sensor is driving the line

4. **Build Errors**
   - Check that all required drivers are enabled in `prj.conf`
   - Verify device tree syntax
   - Ensure board files are in correct location

### Debug Output
Since there's no UART, use:
- SWD debugging with GDB
- RTT (Real-Time Transfer) for log output
- J-Link RTT Viewer for real-time logging

## Hardware Verification

### Pin Connectivity Test
1. **SPI**: Use oscilloscope to verify SCK, MOSI, MISO signals
2. **ADC**: Apply known voltages to P0.04/P0.05 and verify readings
3. **Interrupt**: Toggle P0.12 and verify interrupt callback
4. **LED**: Toggle P0.20 and verify LED operation

### Expected Behavior
- FRAM should respond to SPI commands
- ADC should provide differential measurements
- Magnet sensor should trigger interrupts
- LED should toggle on P0.20

## Support
For issues specific to this board definition, check:
1. Device tree configuration
2. Pin assignments match your hardware
3. Driver configurations in `_defconfig`
4. Application overlay files 