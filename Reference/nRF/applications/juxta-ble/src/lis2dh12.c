/*
 * Custom LIS2DH12 Zephyr Wrapper Implementation
 *
 * Copyright (c) 2024 NeurotechHub
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lis2dh12.h"
#include <zephyr/sys/util.h>
#include <string.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(lis2dh12, LOG_LEVEL_INF);

/* Global context for platform functions */
static struct lis2dh12_dev *g_lis2dh12_dev = NULL;

/**
 * @brief Platform-specific SPI read function for STMicroelectronics library
 *
 * This function implements the required interface for the library's function pointers.
 */
int32_t lis2dh12_platform_read(void *handle, uint8_t reg, uint8_t *data, uint16_t len)
{
    /* For SPI, set read bit (bit 7) and increment bit (bit 6) for multi-byte reads */
    uint8_t tx_reg = reg | 0x80; /* Set read bit */
    if (len > 1)
    {
        tx_reg |= 0x40; /* Set increment bit for multi-byte reads */
    }

    LOG_DBG("LIS2DH READ: reg=0x%02X, tx_reg=0x%02X, len=%d", reg, tx_reg, len);

    uint8_t tx_buf[1 + len]; /* Register + dummy bytes */
    uint8_t rx_buf[1 + len]; /* Response */

    tx_buf[0] = tx_reg;
    memset(&tx_buf[1], 0x00, len); /* Dummy bytes */

    LOG_DBG("LIS2DH READ: tx_buf[0]=0x%02X, tx_buf[1]=0x%02X", tx_buf[0], tx_buf[1]);

    const struct spi_buf tx_bufs = {
        .buf = tx_buf,
        .len = sizeof(tx_buf)};
    const struct spi_buf rx_bufs = {
        .buf = rx_buf,
        .len = sizeof(rx_buf)};
    const struct spi_buf_set tx = {
        .buffers = &tx_bufs,
        .count = 1};
    const struct spi_buf_set rx = {
        .buffers = &rx_bufs,
        .count = 1};

    /* Set CS low (select device - CS is active low) */
    gpio_pin_set(g_lis2dh12_dev->cs_gpio.port, g_lis2dh12_dev->cs_gpio.pin, 0);
    k_sleep(K_USEC(10)); // Small delay for CS setup
    LOG_DBG("LIS2DH READ: CS set low (selected), starting SPI transaction");

    /* Perform SPI transaction */
    int ret = spi_transceive(g_lis2dh12_dev->spi_dev, &g_lis2dh12_dev->spi_cfg, &tx, &rx);

    /* Set CS high (deselect device - CS is active low) */
    gpio_pin_set(g_lis2dh12_dev->cs_gpio.port, g_lis2dh12_dev->cs_gpio.pin, 1);
    LOG_DBG("LIS2DH READ: CS set high (deselected), SPI transaction complete");

    if (ret < 0)
    {
        LOG_ERR("SPI read failed: %d", ret);
        return ret;
    }

    LOG_DBG("LIS2DH READ: raw rx_buf[0]=0x%02X, rx_buf[1]=0x%02X", rx_buf[0], rx_buf[1]);

    /* Copy received data (skip first byte which is the register echo) */
    memcpy(data, &rx_buf[1], len);

    LOG_DBG("LIS2DH READ: received data[0]=0x%02X", data[0]);
    return 0;
}

/**
 * @brief Platform-specific SPI write function for STMicroelectronics library
 *
 * This function implements the required interface for the library's function pointers.
 */
int32_t lis2dh12_platform_write(void *handle, uint8_t reg, const uint8_t *data, uint16_t len)
{
    if (!g_lis2dh12_dev || !g_lis2dh12_dev->initialized)
    {
        LOG_ERR("LIS2DH12 device not initialized");
        return -ENODEV;
    }

    /* For SPI, clear read bit (bit 7) and set increment bit (bit 6) for multi-byte writes */
    uint8_t tx_reg = reg & 0x7F; /* Clear read bit */
    if (len > 1)
    {
        tx_reg |= 0x40; /* Set increment bit for multi-byte writes */
    }

    uint8_t tx_buf[1 + len]; /* Register + data */
    tx_buf[0] = tx_reg;
    memcpy(&tx_buf[1], data, len);

    const struct spi_buf tx_bufs = {
        .buf = tx_buf,
        .len = sizeof(tx_buf)};
    const struct spi_buf_set tx = {
        .buffers = &tx_bufs,
        .count = 1};

    /* Set CS low (select device - CS is active low) */
    gpio_pin_set(g_lis2dh12_dev->cs_gpio.port, g_lis2dh12_dev->cs_gpio.pin, 0);
    k_sleep(K_USEC(10)); // Small delay for CS setup

    /* Perform SPI transaction */
    int ret = spi_write(g_lis2dh12_dev->spi_dev, &g_lis2dh12_dev->spi_cfg, &tx);

    /* Set CS high (deselect device - CS is active low) */
    gpio_pin_set(g_lis2dh12_dev->cs_gpio.port, g_lis2dh12_dev->cs_gpio.pin, 1);

    if (ret < 0)
    {
        LOG_ERR("SPI write failed: %d", ret);
        return ret;
    }

    return 0;
}

int lis2dh12_init(struct lis2dh12_dev *dev)
{
    if (!dev)
    {
        return -EINVAL;
    }

    /* Initialize SPI device */
    if (!device_is_ready(dev->spi_dev))
    {
        LOG_ERR("SPI device not ready");
        return -ENODEV;
    }

    /* Initialize CS GPIO */
    if (!device_is_ready(dev->cs_gpio.port))
    {
        LOG_ERR("CS GPIO not ready");
        return -ENODEV;
    }

    /* Configure CS as output - since device tree shows GPIO_ACTIVE_LOW, we need to set it HIGH to deselect */
    int ret = gpio_pin_configure(dev->cs_gpio.port, dev->cs_gpio.pin, GPIO_OUTPUT);
    if (ret < 0)
    {
        LOG_ERR("Failed to configure CS GPIO: %d", ret);
        return ret;
    }

    /* Set CS high initially (deselect device since CS is active low) */
    gpio_pin_set(dev->cs_gpio.port, dev->cs_gpio.pin, 1);

    /* Configure SPI for LIS2DH12 */
    dev->spi_cfg.frequency = 8000000;                            /* 8MHz max for LIS2DH12 */
    dev->spi_cfg.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB; /* Mode 0 (CPOL=0, CPHA=0) */
    dev->spi_cfg.slave = 1;                                      /* Use slave 1 (accel@1 in device tree) */
    dev->spi_cfg.cs.delay = 0;

    /* Set global context for platform functions */
    g_lis2dh12_dev = dev;
    dev->initialized = true;

    /* Read device ID to verify communication */
    uint8_t device_id;
    ret = lis2dh12_platform_read(NULL, 0x0F, &device_id, 1); // WHO_AM_I register
    if (ret < 0)
    {
        LOG_ERR("Failed to read device ID: %d", ret);
        return ret;
    }

    if (device_id != 0x33)
    { // LIS2DH12 device ID
        LOG_ERR("Invalid device ID: 0x%02X (expected 0x33)", device_id);
        return -ENODEV;
    }

    LOG_INF("LIS2DH12 initialized (ID: 0x%02X)", device_id);

    /* Configure basic settings using direct register writes */

    /* Set data rate to 10Hz and enable XYZ axes in low-power mode (CTRL_REG1 = 0x20) */
    /* ODR = 10Hz (0x2), XYZ enabled (0x07), LPen = 1 (0x08) for low-power mode */
    uint8_t ctrl_reg1 = 0x2F;                                 // 0b00101111: ODR=10Hz, XYZ enabled, LPen=1 (low-power)
    ret = lis2dh12_platform_write(NULL, 0x20, &ctrl_reg1, 1); // CTRL_REG1
    if (ret < 0)
    {
        LOG_ERR("Failed to set CTRL_REG1: %d", ret);
        return ret;
    }

    /* Set scale to 2g and low-power mode (CTRL_REG4 = 0x23) */
    /* 2g scale (0x00), low-power mode (HR=0), BDU enabled (0x80) */
    uint8_t ctrl_reg4 = 0x80;                                 // 0b10000000: BDU=1, HR=0, FS=00 (2g, low-power)
    ret = lis2dh12_platform_write(NULL, 0x23, &ctrl_reg4, 1); // CTRL_REG4
    if (ret < 0)
    {
        LOG_ERR("Failed to set CTRL_REG4: %d", ret);
        return ret;
    }

    /* Enable temperature sensor */
    /* TEMP_CFG_REG (0x1F): Set TEMP_EN[1:0] bits to enable temperature sensor */
    uint8_t temp_cfg = 0xC0;                                 // 0b11000000: TEMP_EN[1:0] = 11 (enable temp sensor)
    ret = lis2dh12_platform_write(NULL, 0x1F, &temp_cfg, 1); // TEMP_CFG_REG
    if (ret < 0)
    {
        LOG_ERR("Failed to enable temperature sensor: %d", ret);
        return ret;
    }

    /* Small delay to allow accelerometer to start producing data */
    k_sleep(K_MSEC(50));

    return 0;
}

int lis2dh12_read_accel(struct lis2dh12_dev *dev, float *x, float *y, float *z)
{
    if (!dev || !dev->initialized || !x || !y || !z)
    {
        LOG_ERR("LIS2DH read_accel: invalid parameters");
        return -EINVAL;
    }

    /* Read raw acceleration data directly using our SPI functions */
    uint8_t data[6];                                       // 3 axes * 2 bytes each
    int ret = lis2dh12_platform_read(NULL, 0x28, data, 6); // OUT_X_L register
    if (ret < 0)
    {
        LOG_ERR("Failed to read acceleration data: %d", ret);
        return ret;
    }

    /* Convert to 16-bit values (little endian) */
    int16_t raw_x = (int16_t)(data[1] << 8 | data[0]); // OUT_X_H << 8 | OUT_X_L
    int16_t raw_y = (int16_t)(data[3] << 8 | data[2]); // OUT_Y_H << 8 | OUT_Y_L
    int16_t raw_z = (int16_t)(data[5] << 8 | data[4]); // OUT_Z_H << 8 | OUT_Z_L

    /* Convert to mg (2g scale, high resolution mode) */
    /* 2g scale in HR mode: 1 LSB = 1mg */
    *x = (float)raw_x;
    *y = (float)raw_y;
    *z = (float)raw_z;

    return 0;
}

/**
 * @brief Read temperature from LIS2DH12 sensor (low power mode, 8-bit resolution)
 *
 * Temperature sensor characteristics:
 * - Low power mode (LPen=1): 8-bit resolution, 1 LSB ≈ 1.0°C, ±1.0°C precision
 * - Data format: 8-bit two's complement
 * - Operating range: -40°C to +85°C (covers full range with 8-bit: -128°C to +127°C)
 * - Relative sensor: Only delta-T matters, not absolute temperature
 * - Efficiency: Reads only 1 byte for minimal SPI transaction
 *
 * Requirements:
 * - Device must be in low power mode (LPen=1 in CTRL_REG1)
 * - Temperature sensor must be enabled (TEMP_EN[1:0] in TEMP_CFG_REG)
 *
 * For high resolution (10-bit) temperature reading, use lis2dh12_read_temperature_highres()
 * which requires normal mode (LPen=0) and reads 2 bytes for ±0.25°C precision.
 */

int lis2dh12_read_temperature_lowres(struct lis2dh12_dev *dev, int8_t *temperature)
{
    if (!dev || !dev->initialized || !temperature)
    {
        LOG_ERR("LIS2DH read_temperature_lowres: invalid parameters");
        return -EINVAL;
    }

    /* Read both temperature registers */
    uint8_t temp_l, temp_h;
    int ret = lis2dh12_platform_read(NULL, 0x0C, &temp_l, 1); // OUT_TEMP_L
    if (ret < 0)
    {
        LOG_ERR("Failed to read OUT_TEMP_L: %d", ret);
        return ret;
    }
    ret = lis2dh12_platform_read(NULL, 0x0D, &temp_h, 1); // OUT_TEMP_H
    if (ret < 0)
    {
        LOG_ERR("Failed to read OUT_TEMP_H: %d", ret);
        return ret;
    }

    /* Combine into 16-bit LSB value */
    int16_t lsb = ((int16_t)temp_h << 8) | temp_l;

    /* Convert using low-power mode formula */
    float_t temp_celsius = lis2dh12_from_lsb_lp_to_celsius(lsb);

    /* Convert to 8-bit signed for compatibility */
    *temperature = (int8_t)temp_celsius;

    return 0;
}

int lis2dh12_configure_motion_detection(struct lis2dh12_dev *dev,
                                        uint8_t threshold, uint8_t duration)
{
    if (!dev || !dev->initialized)
    {
        return -EINVAL;
    }

    int ret;

    /* Configure motion detection using high-pass filter - following ST AN5005 section 6.3.3 */

    /* Step 1: Write 57h into CTRL_REG1 - Turn on sensor, enable X, Y, Z, ODR = 100 Hz */
    uint8_t ctrl_reg1 = 0x57; // 0b01010111: ODR=100Hz, XYZ enabled
    ret = lis2dh12_platform_write(NULL, 0x20, &ctrl_reg1, 1);
    if (ret < 0)
    {
        LOG_ERR("Failed to configure CTRL_REG1: %d", ret);
        return ret;
    }

    /* Step 2: Write 09h into CTRL_REG2 - High-pass filter enabled on interrupt activity 1 */
    uint8_t ctrl_reg2 = 0x09; // 0b00001001: HP filter enabled on INT1
    ret = lis2dh12_platform_write(NULL, 0x21, &ctrl_reg2, 1);
    if (ret < 0)
    {
        LOG_ERR("Failed to configure CTRL_REG2: %d", ret);
        return ret;
    }

    /* Step 3: Write 40h into CTRL_REG3 - Interrupt activity 1 driven to INT1 pin */
    uint8_t ctrl_reg3 = 0x40; // 0b01000000: INT1 activity routed to INT1 pin
    ret = lis2dh12_platform_write(NULL, 0x22, &ctrl_reg3, 1);
    if (ret < 0)
    {
        LOG_ERR("Failed to configure CTRL_REG3: %d", ret);
        return ret;
    }

    /* Step 4: Write 00h into CTRL_REG4 - FS = ±2 g */
    uint8_t ctrl_reg4 = 0x00; // 0b00000000: ±2g scale
    ret = lis2dh12_platform_write(NULL, 0x23, &ctrl_reg4, 1);
    if (ret < 0)
    {
        LOG_ERR("Failed to configure CTRL_REG4: %d", ret);
        return ret;
    }

    /* Step 5: Write 08h into CTRL_REG5 - Interrupt 1 pin latched */
    uint8_t ctrl_reg5 = 0x08; // 0b00001000: INT1 latched
    ret = lis2dh12_platform_write(NULL, 0x24, &ctrl_reg5, 1);
    if (ret < 0)
    {
        LOG_ERR("Failed to configure CTRL_REG5: %d", ret);
        return ret;
    }

    /* Step 6: Write threshold into INT1_THS - Threshold in mg */
    ret = lis2dh12_platform_write(NULL, 0x32, &threshold, 1);
    if (ret < 0)
    {
        LOG_ERR("Failed to set INT1 threshold: %d", ret);
        return ret;
    }

    /* Step 7: Write duration into INT1_DURATION - Duration in samples */
    ret = lis2dh12_platform_write(NULL, 0x33, &duration, 1);
    if (ret < 0)
    {
        LOG_ERR("Failed to set INT1 duration: %d", ret);
        return ret;
    }

    /* Step 8: Read REFERENCE - Dummy read to force HP filter to current acceleration value */
    uint8_t reference;
    ret = lis2dh12_platform_read(NULL, 0x26, &reference, 1);
    if (ret != 0)
    {
        LOG_ERR("Failed to read REFERENCE: %d", ret);
        return ret;
    }

    /* Step 9: Write 2Ah into INT1_CFG - Configure desired wake-up event */
    uint8_t int1_cfg = 0x2A; // 0b00101010: X, Y, Z high interrupts, OR combination
    ret = lis2dh12_platform_write(NULL, 0x30, &int1_cfg, 1);
    if (ret < 0)
    {
        LOG_ERR("Failed to configure INT1_CFG: %d", ret);
        return ret;
    }

    LOG_INF("LIS2DH: High-pass filtered motion detection configured: threshold=%d mg, duration=%d samples",
            threshold, duration);

    /* Clear any pending interrupts by reading INT1_SRC register */
    uint8_t int1_src_clear;
    ret = lis2dh12_platform_read(NULL, 0x31, &int1_src_clear, 1);
    if (ret != 0)
    {
        LOG_ERR("Failed to clear pending interrupt: %d", ret);
        return ret;
    }

    return 0;
}

int lis2dh12_read_device_id(struct lis2dh12_dev *dev, uint8_t *id)
{
    if (!dev || !dev->initialized || !id)
    {
        return -EINVAL;
    }

    return lis2dh12_platform_read(NULL, 0x0F, id, 1); // WHO_AM_I register
}

bool lis2dh12_is_ready(struct lis2dh12_dev *dev)
{
    return dev && dev->initialized;
}

int lis2dh12_read_int1_source(struct lis2dh12_dev *dev, uint8_t *source)
{
    if (!dev || !dev->initialized || !source)
    {
        return -EINVAL;
    }

    return lis2dh12_platform_read(NULL, 0x31, source, 1); // INT1_SRC register
}

int lis2dh12_clear_int1_interrupt(struct lis2dh12_dev *dev)
{
    if (!dev || !dev->initialized)
    {
        return -EINVAL;
    }

    uint8_t int1_src;
    int ret;

    // Read INT1_SRC register to clear the interrupt (per ST datasheet)
    ret = lis2dh12_platform_read(NULL, 0x31, &int1_src, 1);
    if (ret != 0)
    {
        LOG_ERR("Failed to clear INT1 interrupt: %d", ret);
        return ret;
    }

    return 0;
}

int lis2dh12_reset_motion_detection(struct lis2dh12_dev *dev)
{
    if (!dev || !dev->initialized)
    {
        return -EINVAL;
    }

    LOG_INF("LIS2DH: Resetting motion detection...");
    int ret;

    /* Step 1: Disable INT1 interrupt generation temporarily */
    uint8_t int1_cfg_disable = 0x00; // Disable all interrupts
    ret = lis2dh12_platform_write(NULL, 0x30, &int1_cfg_disable, 1);
    if (ret < 0)
    {
        LOG_ERR("Failed to disable INT1: %d", ret);
        return ret;
    }
    LOG_INF("LIS2DH: INT1 interrupts disabled");

    /* Step 2: Clear any pending interrupts */
    uint8_t int1_src;
    ret = lis2dh12_platform_read(NULL, 0x31, &int1_src, 1);
    if (ret == 0)
    {
        LOG_INF("LIS2DH: Cleared pending interrupt, INT1_SRC=0x%02X", int1_src);
    }

    /* Step 3: Wait a moment for the interrupt condition to settle */
    k_sleep(K_MSEC(100));
    LOG_INF("LIS2DH: Waited 100ms for interrupt condition to settle");

    /* Step 4: Re-enable INT1 with proper configuration */
    uint8_t int1_cfg_enable = 0x2A; // Enable X, Y, Z high interrupts with OR combination (HP filtered)
    ret = lis2dh12_platform_write(NULL, 0x30, &int1_cfg_enable, 1);
    if (ret < 0)
    {
        LOG_ERR("Failed to re-enable INT1: %d", ret);
        return ret;
    }
    LOG_INF("LIS2DH: INT1 interrupts re-enabled");

    /* Step 5: Verify the interrupt is not immediately active */
    k_sleep(K_MSEC(50));
    ret = lis2dh12_platform_read(NULL, 0x31, &int1_src, 1);
    if (ret == 0)
    {
        LOG_INF("LIS2DH: INT1_SRC after reset: 0x%02X (IA=%d)", int1_src, (int1_src & 0x40) ? 1 : 0);
        if (int1_src & 0x40)
        {
            LOG_WRN("LIS2DH: ⚠️ Interrupt still active after reset - threshold may be too low");
        }
        else
        {
            LOG_INF("LIS2DH: ✅ Motion detection reset successful - no active interrupt");
        }
    }

    LOG_INF("LIS2DH: Motion detection reset completed");
    return 0;
}

int lis2dh12_test_interrupt_clearing(struct lis2dh12_dev *dev)
{
    if (!dev || !dev->initialized)
    {
        return -EINVAL;
    }

    LOG_INF("LIS2DH: Testing interrupt clearing functionality...");

    uint8_t int1_src;
    int ret;

    // Read current interrupt status
    ret = lis2dh12_platform_read(NULL, 0x31, &int1_src, 1);
    if (ret == 0)
    {
        LOG_INF("LIS2DH: Current INT1_SRC: 0x%02X (IA=%d)", int1_src, (int1_src & 0x40) ? 1 : 0);
    }

    // Test clearing the interrupt
    LOG_INF("LIS2DH: Testing interrupt clear...");
    ret = lis2dh12_clear_int1_interrupt(dev);

    // Read again to verify clear (after a delay to let HP filter settle)
    k_sleep(K_MSEC(50));
    ret = lis2dh12_platform_read(NULL, 0x31, &int1_src, 1);
    if (ret == 0)
    {
        LOG_INF("LIS2DH: INT1_SRC after clear test: 0x%02X (IA=%d)", int1_src, (int1_src & 0x40) ? 1 : 0);

        if (int1_src & 0x40)
        {
            LOG_WRN("LIS2DH: ⚠️ Interrupt still active - condition still met");
            return -1;
        }
        else
        {
            LOG_INF("LIS2DH: ✅ Interrupt clear test successful");
            return 0;
        }
    }

    return ret;
}

/* ========================================================================
 * Motion System Management
 * ======================================================================== */

/* Motion system state */
static uint8_t motion_count = 0;
static volatile bool lis2dh_interrupt_pending = false;
static struct lis2dh12_dev motion_dev;
static struct k_work motion_work;
static struct k_timer motion_timer;
static bool motion_based_intervals = false;
static struct gpio_callback lis2dh_int_cb;

/* Motion work handler - runs in work queue context (safe for longer operations) */
static void motion_work_handler(struct k_work *work)
{
    if (lis2dh_interrupt_pending)
    {
        lis2dh_interrupt_pending = false;

        // Clear the interrupt so we can detect new ones
        lis2dh12_clear_int1_interrupt(&motion_dev);

        // Reset to default intervals when motion is detected
        motion_based_intervals = false;
        LOG_INF("🏃 Motion interrupt cleared (count: %d) - reset to default intervals", motion_count);
    }
}

/* Motion timer callback - starts work queue after 1 second delay */
static void motion_timer_callback(struct k_timer *timer)
{
    k_work_submit(&motion_work);
}

/* GPIO interrupt callback for LIS2DH motion detection */
static void lis2dh_int_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    motion_count++;
    lis2dh_interrupt_pending = true;
    printk("🏃 Motion detected via GPIO! Count: %d\n", motion_count);

    // Start 1-second timer to process interrupt (self-limiting to 60 events/minute)
    k_timer_start(&motion_timer, K_SECONDS(1), K_NO_WAIT);
}

int lis2dh12_init_motion_system(void)
{
    // Initialize LIS2DH device
    motion_dev.spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi0));

    // Initialize GPIO specs manually
    motion_dev.cs_gpio.port = DEVICE_DT_GET(DT_GPIO_CTLR_BY_IDX(DT_NODELABEL(spi0), cs_gpios, 1));
    motion_dev.cs_gpio.pin = DT_GPIO_PIN_BY_IDX(DT_NODELABEL(spi0), cs_gpios, 1);
    motion_dev.cs_gpio.dt_flags = DT_GPIO_FLAGS_BY_IDX(DT_NODELABEL(spi0), cs_gpios, 1);

    motion_dev.int_gpio.port = DEVICE_DT_GET(DT_GPIO_CTLR(DT_PATH(gpio_keys, accel_int), gpios));
    motion_dev.int_gpio.pin = DT_GPIO_PIN(DT_PATH(gpio_keys, accel_int), gpios);
    motion_dev.int_gpio.dt_flags = DT_GPIO_FLAGS(DT_PATH(gpio_keys, accel_int), gpios);

    int ret = lis2dh12_init(&motion_dev);
    if (ret < 0)
    {
        LOG_ERR("Failed to initialize LIS2DH: %d", ret);
        return ret;
    }

    // Configure motion detection with high-pass filter for mouse collar sensitivity (0.01g = ~10 in LIS2DH units)
    ret = lis2dh12_configure_motion_detection(&motion_dev, 10, 0);
    if (ret < 0)
    {
        LOG_ERR("Failed to configure LIS2DH motion detection: %d", ret);
        return ret;
    }

    // Configure GPIO interrupt for INT1
    if (!device_is_ready(motion_dev.int_gpio.port))
    {
        LOG_ERR("LIS2DH INT GPIO not ready");
        return -ENODEV;
    }

    ret = gpio_pin_configure(motion_dev.int_gpio.port, motion_dev.int_gpio.pin, GPIO_INPUT | GPIO_PULL_UP);
    if (ret < 0)
    {
        LOG_ERR("Failed to configure LIS2DH INT GPIO: %d", ret);
        return ret;
    }

    ret = gpio_pin_interrupt_configure(motion_dev.int_gpio.port, motion_dev.int_gpio.pin, GPIO_INT_EDGE_RISING);
    if (ret < 0)
    {
        LOG_ERR("Failed to configure LIS2DH INT interrupt: %d", ret);
        return ret;
    }

    gpio_init_callback(&lis2dh_int_cb, lis2dh_int_callback, BIT(motion_dev.int_gpio.pin));
    ret = gpio_add_callback(motion_dev.int_gpio.port, &lis2dh_int_cb);
    if (ret < 0)
    {
        LOG_ERR("Failed to add LIS2DH INT callback: %d", ret);
        return ret;
    }

    // Initialize work and timer
    k_work_init(&motion_work, motion_work_handler);
    k_timer_init(&motion_timer, motion_timer_callback, NULL);

    LOG_INF("✅ LIS2DH motion detection configured (ODR=100Hz, HP filtered, threshold=0.01g, duration=0)");

    // Test GPIO interrupt setup
    int gpio_state = gpio_pin_get(motion_dev.int_gpio.port, motion_dev.int_gpio.pin);
    LOG_INF("LIS2DH INT GPIO initial state: %d", gpio_state);

    return 0;
}

void lis2dh12_process_motion_events(void)
{
    // This function is called from main.c minute processing
    // Handle motion count reporting and interval adjustment
    if (motion_count > 0)
    {
        LOG_INF("Motion events in last minute: %d", motion_count);
        motion_count = 0;
        motion_based_intervals = false; // Reset to default intervals
    }
    else
    {
        // No motion detected in the last minute, switch to extended intervals
        motion_based_intervals = true;
        LOG_INF("No motion detected in last minute - switching to extended intervals (2x)");
    }
}

bool lis2dh12_should_use_extended_intervals(void)
{
    return motion_based_intervals;
}

uint8_t lis2dh12_get_motion_count(void)
{
    return motion_count;
}

void lis2dh12_reset_motion_count(void)
{
    motion_count = 0;
}

int lis2dh12_analyze_interrupt_trigger(struct lis2dh12_dev *dev)
{
    if (!dev || !dev->initialized)
    {
        return -EINVAL;
    }

    LOG_INF("LIS2DH: Analyzing interrupt trigger...");

    // Read current acceleration values
    float x, y, z;
    int ret = lis2dh12_read_accel(dev, &x, &y, &z);
    if (ret != 0)
    {
        LOG_ERR("Failed to read acceleration: %d", ret);
        return ret;
    }

    // Read current threshold
    uint8_t threshold;
    ret = lis2dh12_platform_read(NULL, 0x32, &threshold, 1);
    if (ret != 0)
    {
        LOG_ERR("Failed to read threshold: %d", ret);
        return ret;
    }

    // Read INT1_CFG to see which axes are enabled
    uint8_t int1_cfg;
    ret = lis2dh12_platform_read(NULL, 0x30, &int1_cfg, 1);
    if (ret != 0)
    {
        LOG_ERR("Failed to read INT1_CFG: %d", ret);
        return ret;
    }

    LOG_INF("LIS2DH: Current acceleration: X=%d mg, Y=%d mg, Z=%d mg", (int)x, (int)y, (int)z);
    LOG_INF("LIS2DH: Threshold: %d mg", threshold);
    LOG_INF("LIS2DH: INT1_CFG: 0x%02X", int1_cfg);

    // Check which axes are enabled for high interrupt
    bool xhie = (int1_cfg & 0x01) != 0;
    bool yhie = (int1_cfg & 0x02) != 0;
    bool zhie = (int1_cfg & 0x04) != 0;
    bool aoi = (int1_cfg & 0x40) != 0;

    LOG_INF("LIS2DH: X high interrupt enabled: %s", xhie ? "YES" : "NO");
    LOG_INF("LIS2DH: Y high interrupt enabled: %s", yhie ? "YES" : "NO");
    LOG_INF("LIS2DH: Z high interrupt enabled: %s", zhie ? "YES" : "NO");
    LOG_INF("LIS2DH: AOI (AND/OR): %s", aoi ? "AND" : "OR");

    // For HP filtered motion detection, we check if any axis exceeds threshold
    // The HP filter removes DC component, so we look at absolute values
    bool x_high = (abs((int)x) > threshold);
    bool y_high = (abs((int)y) > threshold);
    bool z_high = (abs((int)z) > threshold);

    LOG_INF("LIS2DH: |X| exceeds threshold (%d > %d): %s", abs((int)x), threshold, x_high ? "YES" : "NO");
    LOG_INF("LIS2DH: |Y| exceeds threshold (%d > %d): %s", abs((int)y), threshold, y_high ? "YES" : "NO");
    LOG_INF("LIS2DH: |Z| exceeds threshold (%d > %d): %s", abs((int)z), threshold, z_high ? "YES" : "NO");

    // Determine if interrupt should be triggered
    bool should_trigger = false;
    if (aoi) // AND combination
    {
        should_trigger = (xhie && x_high) && (yhie && y_high) && (zhie && z_high);
    }
    else // OR combination
    {
        should_trigger = (xhie && x_high) || (yhie && y_high) || (zhie && z_high);
    }

    LOG_INF("LIS2DH: Interrupt should be triggered: %s", should_trigger ? "YES" : "NO");

    if (should_trigger)
    {
        LOG_WRN("LIS2DH: ⚠️ Current HP filtered acceleration exceeds threshold - interrupt is expected");
        return 1; // Interrupt is expected
    }
    else
    {
        LOG_INF("LIS2DH: ✅ Current HP filtered acceleration is below threshold");
        return 0; // Interrupt is not expected
    }
}