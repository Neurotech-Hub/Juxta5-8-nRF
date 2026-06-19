# Juxta5-4_nRF52840 Board Support

This document describes the Juxta5-4_nRF52840 board support for the nRF Connect SDK.

## Board Overview

The Juxta5-4_nRF52840 is a comprehensive sensor board based on the nRF52840 SoC designed for advanced low-power applications with multiple sensor interfaces and ample memory resources.

## Pin Assignments

| Pin   | Function | Direction | Notes                                |
| ----- | -------- | --------- | ------------------------------------ |
| P0.00 | XL1      | -         | Low frequency crystal                |
| P0.01 | XL2      | -         | Low frequency crystal                |
| P0.02 | AIN0     | Input     | ADC negative differential input     |
| P0.03 | AIN1     | Input     | ADC positive differential input     |
| P0.04 | GPIO     | Input     | Accelerometer interrupt              |
| P0.05 | LED      | Output    | Status LED (active high)             |
| P0.08 | MISO     | Input     | SPI Master In Slave Out              |
| P0.12 | SCK      | Output    | SPI Serial Clock                     |
| P0.15 | CS       | Output    | Accelerometer chip select            |
| P0.18 | RESET    | Input     | System reset                         |
| P1.09 | MOSI     | Output    | SPI Master Out Slave In              |
| P1.11 | GPIO     | Input     | Magnet sensor interrupt (active high) |
| P1.13 | CS       | Output    | FRAM chip select                     |
| G1    | SWDCLK   | -         | SWD debug clock                      |
| F1    | SWDIO    | -         | SWD debug data                       |

## Device Tree Configuration

### SPI Configuration
```dts
&spi0 {
	status = "okay";
	cs-gpios = <&gpio1 13 GPIO_ACTIVE_LOW>,  /* FRAM CS */
		   <&gpio0 5 GPIO_ACTIVE_LOW>;   /* Accelerometer CS */
	
	fram0: fram@0 {
		compatible = "jedec,spi-nor";
		reg = <0>;
		spi-max-frequency = <8000000>;
		size = <DT_SIZE_K(128)>;
		has-dpd;
		jedec-id = [04 7F 27];
	};

	accel0: accel@1 {
		compatible = "st,lis2dh12", "st,lis2dh";
		reg = <1>;
		spi-max-frequency = <8000000>;
		label = "LIS2DH12";
	};
};
```

### ADC Differential Configuration
```dts
&adc {
	status = "okay";
	
	channel@0 {
		reg = <0>;
		zephyr,gain = "ADC_GAIN_1_6";
		zephyr,reference = "ADC_REF_INTERNAL";
		zephyr,input-positive = <NRF_SAADC_AIN1>; /* P0.03 */
		zephyr,input-negative = <NRF_SAADC_AIN0>; /* P0.02 */
		zephyr,resolution = <12>;
		zephyr,oversampling = <8>;
	};
};
```

### GPIO Interrupt Configuration
```dts
gpio_keys {
	compatible = "gpio-keys";
	magnet_sensor: magnet_sensor {
		gpios = <&gpio1 11 GPIO_ACTIVE_LOW>;
		label = "Magnet sensor interrupt";
	};
	accel_int: accel_int {
		gpios = <&gpio0 4 GPIO_ACTIVE_LOW>;
		label = "Accelerometer interrupt";
	};
};
```

## Board Files

| File | Purpose |
|------|---------|
| `Juxta5-4_nRF52840.dts` | Main device tree with all peripherals and pin configurations |
| `Juxta5-4_nRF52840-pinctrl.dtsi` | Pin control configuration for SPI0 interface |
| `Juxta5-4_nRF52840_defconfig` | Hardware-specific Kconfig defaults (protection, peripherals, limitations) |
| `Kconfig.Juxta5-4_nRF52840` | Board selection for Kconfig system |
| `board.cmake` | Programming and debugging tool configurations |
| `board.yml` | Board metadata for Zephyr build system |
| `pre_dt_board.cmake` | Device tree compilation flags and settings |
