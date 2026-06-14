# RTC Setup for Juxta5-1_ADC

This guide describes how to set up the Real-Time Clock (RTC) functionality for the Juxta5-1_ADC board using the nRF52805's RTC0 peripheral.

## Device Tree Configuration

### Board-Level Configuration (Juxta5-1_ADC.dts)
```dts
/ {
    aliases {
        rtc = &rtc0;  /* Required for RTC driver to find the device */
    };
};

&rtc0 {
    status = "okay";  /* Enable RTC0 peripheral */
};
```

### Board Defconfig (Juxta5-1_ADC_defconfig)
```
# Enable RTC
CONFIG_RTC=y
CONFIG_COUNTER=y  /* Required by RTC driver */
```

## Project Configuration (prj.conf)
```
# Enable RTC for timestamp management (after clock initialization)
CONFIG_RTC=y
CONFIG_RTC_INIT_PRIORITY=55  /* Set priority after clock initialization */

# Enable POSIX API (replaces deprecated POSIX_CLOCK)
CONFIG_POSIX_API=y
```

## Usage Example

```c
#include <zephyr/drivers/rtc.h>

/* Get RTC device from device tree */
const struct device *rtc = DEVICE_DT_GET(DT_ALIAS(rtc));

/* Set initial time (e.g., 2024-01-20 12:00:00) */
struct rtc_time time = {
    .tm_year = 2024 - 1900,  /* Years since 1900 */
    .tm_mon = 1 - 1,         /* Month (0-11) */
    .tm_mday = 20,           /* Day of month (1-31) */
    .tm_hour = 12,           /* Hours (0-23) */
    .tm_min = 0,             /* Minutes (0-59) */
    .tm_sec = 0             /* Seconds (0-59) */
};

/* Check if RTC is ready */
if (!device_is_ready(rtc)) {
    LOG_ERR("RTC device not ready");
    return;
}

/* Set RTC time */
int ret = rtc_set_time(rtc, &time);
if (ret < 0) {
    LOG_ERR("Could not set RTC time: %d", ret);
    return;
}

/* Read RTC time */
struct rtc_time current_time;
ret = rtc_get_time(rtc, &current_time);
if (ret < 0) {
    LOG_ERR("Could not get RTC time: %d", ret);
    return;
}

LOG_INF("Current time: %04d-%02d-%02d %02d:%02d:%02d",
        current_time.tm_year + 1900,
        current_time.tm_mon + 1,
        current_time.tm_mday,
        current_time.tm_hour,
        current_time.tm_min,
        current_time.tm_sec);
```

## Important Notes

1. **Time Format**
   - `tm_year`: Years since 1900 (e.g., 2024 would be 124)
   - `tm_mon`: Months since January (0-11)
   - `tm_mday`: Day of month (1-31)
   - `tm_hour`: Hours since midnight (0-23)
   - `tm_min`: Minutes (0-59)
   - `tm_sec`: Seconds (0-59)

2. **Hardware Dependencies**
   - The nRF52805 uses RTC0 for timekeeping
   - Requires 32.768 kHz low-frequency crystal for accurate timing
   - Crystal is already configured in board device tree

3. **Power Considerations**
   - RTC continues running in low-power modes
   - Uses low-frequency clock domain
   - Minimal power impact when properly configured

4. **Limitations**
   - No automatic daylight savings adjustment
   - No timezone support (uses UTC)
   - No battery backup (time resets on power loss)

## Troubleshooting

1. **RTC Not Incrementing**
   - Check if 32.768 kHz crystal is properly configured
   - Verify RTC0 is enabled in device tree
   - Ensure CONFIG_RTC is enabled

2. **Incorrect Time**
   - Remember months are 0-based (January = 0)
   - Years should be offset from 1900
   - Check all time components are within valid ranges

3. **Build Errors**
   - Verify all required configs are enabled
   - Check device tree syntax
   - Ensure RTC driver is included in build 