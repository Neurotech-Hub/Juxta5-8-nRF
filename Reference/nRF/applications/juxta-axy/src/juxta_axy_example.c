/*
 * JUXTA-AXY Example Application
 *
 * This demonstrates how to use the LIS2DH12 accelerometer, GPIO interrupt, and LED
 * with low-power sleep until magnet sensor interrupt. This is an accelerometer
 * playground based on the juxta-mvp application.
 *
 * Key differences from juxta-mvp:
 * - Removed FRAM functionality (no P0.20 CS conflict)
 * - Removed ADC functionality (P0.04/P0.05 used for accelerometer)
 * - Added LIS2DH12 accelerometer support
 * - Kept magnet sensor interrupt and LED functionality
 * - Kept low-power sleep functionality
 *
 * Future FRAM integration: This application can be extended to include FRAM
 * functionality from the juxta_fram library for data logging purposes.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/pm.h>
#include <string.h>

#include "lis2dh12_zephyr.h"

LOG_MODULE_REGISTER(juxta_axy_example, LOG_LEVEL_DBG);

/* Device tree definitions */
#define MAGNET_SENSOR_NODE DT_ALIAS(magnet_sensor)
#define LED_NODE DT_ALIAS(led0)
#define ACCEL_NODE DT_ALIAS(spi_accel)
#define ACCEL_INT_NODE DT_ALIAS(accel_int)

/* GPIO specifications */
static const struct gpio_dt_spec magnet_sensor = GPIO_DT_SPEC_GET(MAGNET_SENSOR_NODE, gpios);
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED_NODE, gpios);
static const struct gpio_dt_spec accel_int = GPIO_DT_SPEC_GET(ACCEL_INT_NODE, gpios);

/* Callback data */
static struct gpio_callback magnet_cb_data;

/* Semaphore for signaling magnet sensor interrupt */
static K_SEM_DEFINE(magnet_sem, 0, 1);

/* Counter for magnet sensor events */
static uint32_t magnet_event_count = 0;

/* Accelerometer device */
static struct lis2dh12_zephyr_dev accel_dev;

/**
 * @brief Magnet sensor interrupt callback
 */
void magnet_sensor_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    magnet_event_count++;

    LOG_INF("🧲 Magnet sensor interrupt triggered! (Event #%u)", magnet_event_count);

    /* Signal the main thread that an interrupt occurred */
    k_sem_give(&magnet_sem);
}

/**
 * @brief Initialize magnet sensor interrupt
 */
static int init_magnet_sensor(void)
{
    int ret;

    if (!gpio_is_ready_dt(&magnet_sensor))
    {
        LOG_ERR("Magnet sensor GPIO not ready");
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&magnet_sensor, GPIO_INPUT);
    if (ret < 0)
    {
        LOG_ERR("Failed to configure magnet sensor pin: %d", ret);
        return ret;
    }

    ret = gpio_pin_interrupt_configure_dt(&magnet_sensor, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0)
    {
        LOG_ERR("Failed to configure magnet sensor interrupt: %d", ret);
        return ret;
    }

    gpio_init_callback(&magnet_cb_data, magnet_sensor_callback, BIT(magnet_sensor.pin));
    gpio_add_callback(magnet_sensor.port, &magnet_cb_data);

    LOG_INF("Magnet sensor initialized on pin %d (interrupt on rising edge)", magnet_sensor.pin);
    return 0;
}

/**
 * @brief Initialize LED (no longer shared with FRAM CS)
 */
static int init_led(void)
{
    int ret;

    if (!gpio_is_ready_dt(&led))
    {
        LOG_ERR("LED GPIO not ready");
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    if (ret < 0)
    {
        LOG_ERR("Failed to configure LED pin: %d", ret);
        return ret;
    }

    LOG_INF("LED initialized on pin %d", led.pin);
    return 0;
}

/**
 * @brief Initialize LIS2DH12 accelerometer
 */
static int init_accelerometer(void)
{
    int ret;

    LOG_INF("Initializing LIS2DH12 accelerometer...");

    /* Initialize the accelerometer device */
    ret = lis2dh12_zephyr_init(&accel_dev, NULL, &accel_int);
    if (ret < 0)
    {
        LOG_ERR("Failed to initialize LIS2DH12: %d", ret);
        return ret;
    }

    /* Test basic communication */
    ret = lis2dh12_zephyr_test(&accel_dev);
    if (ret < 0)
    {
        LOG_ERR("LIS2DH12 test failed: %d", ret);
        return ret;
    }

    LOG_INF("LIS2DH12 accelerometer initialized successfully");
    return 0;
}

/**
 * @brief Test accelerometer communication
 */
static int test_accelerometer(void)
{
    int ret;

    LOG_INF("Testing LIS2DH12 accelerometer communication...");

    /* Verify WHO_AM_I register */
    ret = lis2dh12_zephyr_verify_who_am_i(&accel_dev);
    if (ret < 0)
    {
        LOG_ERR("WHO_AM_I verification failed: %d", ret);
        return ret;
    }

    /* Read device ID directly */
    uint8_t device_id = 0;
    ret = lis2dh12_zephyr_read_device_id(&accel_dev, &device_id);
    if (ret < 0)
    {
        LOG_ERR("Failed to read device ID: %d", ret);
        return ret;
    }

    LOG_INF("✅ LIS2DH12 communication verified - Device ID: 0x%02X", device_id);
    return 0;
}

/**
 * @brief Flash LED to indicate activity
 */
static void flash_led(void)
{
    LOG_DBG("Flashing LED to indicate activity");
    gpio_pin_set_dt(&led, 1);
    k_msleep(100);
    gpio_pin_set_dt(&led, 0);
}

/**
 * @brief Handle wake-up activities after magnet sensor interrupt
 */
static void handle_magnet_event(void)
{
    LOG_INF("🔋 Device woke up from sleep due to magnet sensor!");

    /* Test accelerometer communication */
    LOG_INF("📊 Testing accelerometer communication after wake-up...");
    test_accelerometer();

    /* Flash LED to indicate activity */
    flash_led();

    /*
     * Future FRAM integration point:
     * Here you would store the event data and accelerometer readings
     * to FRAM for persistence across power cycles:
     *
     * if (CONFIG_JUXTA_FRAM) {
     *     fram_store_event(magnet_event_count, accel_data);
     * }
     */

    LOG_INF("✅ Event processing complete. Returning to sleep...");
}

/**
 * @brief Main application entry point
 */
int juxta_axy_example_main(void)
{
    int ret;

    LOG_INF("Starting JUXTA-AXY Low-Power Accelerometer Example");
    LOG_INF("Board: Juxta5-1_AXY (Accelerometer variant)");

    /* Initialize peripherals */
    ret = init_magnet_sensor();
    if (ret < 0)
    {
        LOG_ERR("Failed to initialize magnet sensor: %d", ret);
        return ret;
    }

    ret = init_led();
    if (ret < 0)
    {
        LOG_ERR("Failed to initialize LED: %d", ret);
        return ret;
    }

    ret = init_accelerometer();
    if (ret < 0)
    {
        LOG_ERR("Failed to initialize accelerometer: %d", ret);
        return ret;
    }

    LOG_INF("All peripherals initialized successfully");

    /* Initial accelerometer test */
    LOG_INF("🧪 Performing initial accelerometer test...");
    ret = test_accelerometer();
    if (ret < 0)
    {
        LOG_ERR("Initial accelerometer test failed: %d", ret);
        return ret;
    }

    /* Flash LED to indicate successful initialization */
    flash_led();
    k_msleep(200);
    flash_led();

    LOG_INF("🔋 Entering low-power mode - device will sleep until magnet sensor interrupt");
    LOG_INF("🧲 Trigger the magnet sensor (P0.%02d) to wake the device", magnet_sensor.pin);
    LOG_INF("🚀 Each wake-up will test LIS2DH12 WHO_AM_I communication");

    /* Main loop - sleep until interrupt */
    while (1)
    {
        /* Wait for magnet sensor interrupt (blocks until interrupt occurs) */
        ret = k_sem_take(&magnet_sem, K_FOREVER);
        if (ret == 0)
        {
            /* Process the magnet sensor event */
            handle_magnet_event();
        }

        /* Brief delay before going back to sleep */
        k_msleep(100);

        LOG_INF("💤 Going back to sleep... (Event count: %u)", magnet_event_count);
    }

    return 0;
}