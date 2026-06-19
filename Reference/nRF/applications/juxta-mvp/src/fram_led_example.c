/*
 * Example demonstrating shared CS/LED pin usage
 * Shows how to safely switch between FRAM and LED modes
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <juxta_fram/fram.h>
#include <string.h>

LOG_MODULE_REGISTER(fram_led_example, LOG_LEVEL_DBG);

/* Device tree definitions */
#define LED_NODE DT_ALIAS(led0)
#define FRAM_NODE DT_ALIAS(spi_fram)

/* GPIO specifications */
static const struct gpio_dt_spec led_gpio = GPIO_DT_SPEC_GET(LED_NODE, gpios);

/* FRAM device instance */
static struct juxta_fram_device fram_dev;

/**
 * @brief Demonstrate FRAM operations
 */
static int demo_fram_operations(void)
{
    int ret;

    LOG_INF("=== FRAM Operations Demo ===");

    /* FRAM operations automatically switch to SPI mode */
    uint8_t test_data[] = {0x11, 0x22, 0x33, 0x44};
    uint32_t test_addr = 0x2000;

    ret = juxta_fram_write(&fram_dev, test_addr, test_data, sizeof(test_data));
    if (ret < 0)
    {
        LOG_ERR("FRAM write failed: %d", ret);
        return ret;
    }

    uint8_t read_data[sizeof(test_data)] = {0};
    ret = juxta_fram_read(&fram_dev, test_addr, read_data, sizeof(read_data));
    if (ret < 0)
    {
        LOG_ERR("FRAM read failed: %d", ret);
        return ret;
    }

    /* Verify data */
    bool match = true;
    for (size_t i = 0; i < sizeof(test_data); i++)
    {
        if (test_data[i] != read_data[i])
        {
            match = false;
            break;
        }
    }

    if (match)
    {
        LOG_INF("✅ FRAM read/write successful");
    }
    else
    {
        LOG_ERR("❌ FRAM data mismatch");
        return -1;
    }

    return 0;
}

/**
 * @brief Demonstrate LED operations
 */
static int demo_led_operations(void)
{
    int ret;

    LOG_INF("=== LED Operations Demo ===");

    /* Switch to LED mode */
    ret = juxta_fram_led_mode_enable(&fram_dev);
    if (ret < 0)
    {
        LOG_ERR("Failed to enable LED mode: %d", ret);
        return ret;
    }

    LOG_INF("LED mode enabled - testing LED functions...");

    /* Test individual LED functions */
    LOG_INF("Turning LED ON");
    ret = juxta_fram_led_on(&fram_dev);
    if (ret < 0)
    {
        LOG_ERR("LED on failed: %d", ret);
        return ret;
    }
    k_sleep(K_MSEC(500));

    LOG_INF("Turning LED OFF");
    ret = juxta_fram_led_off(&fram_dev);
    if (ret < 0)
    {
        LOG_ERR("LED off failed: %d", ret);
        return ret;
    }
    k_sleep(K_MSEC(500));

    /* Test toggle function */
    LOG_INF("Toggling LED 5 times...");
    for (int i = 0; i < 5; i++)
    {
        ret = juxta_fram_led_toggle(&fram_dev);
        if (ret < 0)
        {
            LOG_ERR("LED toggle failed: %d", ret);
            return ret;
        }
        k_sleep(K_MSEC(200));
    }

    /* Test set function */
    LOG_INF("Using led_set function...");
    ret = juxta_fram_led_set(&fram_dev, true); /* ON */
    if (ret < 0)
    {
        LOG_ERR("LED set failed: %d", ret);
        return ret;
    }
    k_sleep(K_MSEC(300));

    ret = juxta_fram_led_set(&fram_dev, false); /* OFF */
    if (ret < 0)
    {
        LOG_ERR("LED set failed: %d", ret);
        return ret;
    }

    LOG_INF("✅ LED operations successful");
    return 0;
}

/**
 * @brief Demonstrate mixed FRAM and LED usage
 */
static int demo_mixed_operations(void)
{
    int ret;

    LOG_INF("=== Mixed FRAM/LED Operations Demo ===");

    for (int cycle = 0; cycle < 3; cycle++)
    {
        LOG_INF("--- Cycle %d ---", cycle + 1);

        /* Store cycle count in FRAM */
        uint32_t cycle_addr = 0x3000 + (cycle * 4);
        ret = juxta_fram_write(&fram_dev, cycle_addr, (uint8_t *)&cycle, sizeof(cycle));
        if (ret < 0)
        {
            LOG_ERR("Failed to write cycle to FRAM: %d", ret);
            return ret;
        }
        LOG_INF("Stored cycle %d in FRAM at address 0x%06X", cycle, cycle_addr);

        /* Switch to LED mode and blink */
        ret = juxta_fram_led_mode_enable(&fram_dev);
        if (ret < 0)
        {
            LOG_ERR("Failed to enable LED mode: %d", ret);
            return ret;
        }

        /* Blink LED (cycle + 1) times */
        LOG_INF("Blinking LED %d times", cycle + 1);
        for (int blink = 0; blink < (cycle + 1); blink++)
        {
            juxta_fram_led_on(&fram_dev);
            k_sleep(K_MSEC(200));
            juxta_fram_led_off(&fram_dev);
            k_sleep(K_MSEC(200));
        }

        /* Read back from FRAM (automatically switches to SPI mode) */
        int read_cycle = -1;
        ret = juxta_fram_read(&fram_dev, cycle_addr, (uint8_t *)&read_cycle, sizeof(read_cycle));
        if (ret < 0)
        {
            LOG_ERR("Failed to read cycle from FRAM: %d", ret);
            return ret;
        }

        if (read_cycle == cycle)
        {
            LOG_INF("✅ Cycle %d verified from FRAM", read_cycle);
        }
        else
        {
            LOG_ERR("❌ FRAM verification failed: expected %d, got %d", cycle, read_cycle);
            return -1;
        }

        k_sleep(K_SECONDS(1));
    }

    LOG_INF("✅ Mixed operations successful");
    return 0;
}

/**
 * @brief Demonstrate error handling when using wrong mode
 */
static int demo_error_handling(void)
{
    int ret;

    LOG_INF("=== Error Handling Demo ===");

    /* Try LED operation without enabling LED mode */
    LOG_INF("Attempting LED operation in SPI mode (should fail)...");
    ret = juxta_fram_led_on(&fram_dev);
    if (ret == JUXTA_FRAM_ERROR_MODE)
    {
        LOG_INF("✅ Correctly rejected LED operation in SPI mode");
    }
    else
    {
        LOG_ERR("❌ Should have failed with mode error");
        return -1;
    }

    /* Enable LED mode */
    ret = juxta_fram_led_mode_enable(&fram_dev);
    if (ret < 0)
    {
        LOG_ERR("Failed to enable LED mode: %d", ret);
        return ret;
    }

    /* Now LED operations should work */
    ret = juxta_fram_led_on(&fram_dev);
    if (ret == JUXTA_FRAM_OK)
    {
        LOG_INF("✅ LED operation successful after enabling LED mode");
    }
    else
    {
        LOG_ERR("❌ LED operation failed: %d", ret);
        return ret;
    }

    /* FRAM operation will automatically switch modes */
    LOG_INF("Performing FRAM operation (should auto-switch modes)...");
    uint8_t test_byte = 0x55;
    ret = juxta_fram_write_byte(&fram_dev, 0x4000, test_byte);
    if (ret == JUXTA_FRAM_OK)
    {
        LOG_INF("✅ FRAM operation successful (auto mode switch)");
    }
    else
    {
        LOG_ERR("❌ FRAM operation failed: %d", ret);
        return ret;
    }

    /* Check current mode */
    if (juxta_fram_is_led_mode(&fram_dev))
    {
        LOG_INF("Currently in LED mode");
    }
    else
    {
        LOG_INF("Currently in SPI mode");
    }

    LOG_INF("✅ Error handling demo successful");
    return 0;
}

/**
 * @brief Main demonstration function
 */
int fram_led_example_main(void)
{
    int ret;

    LOG_INF("Starting FRAM/LED Shared Pin Demo");

    /* Initialize FRAM */
    ret = juxta_fram_init_dt(&fram_dev, DEVICE_DT_GET(FRAM_NODE), &led_gpio);
    if (ret < 0)
    {
        LOG_ERR("Failed to initialize FRAM: %d", ret);
        return ret;
    }

    /* Verify FRAM is working */
    ret = juxta_fram_test(&fram_dev, 0x1000);
    if (ret < 0)
    {
        LOG_ERR("FRAM test failed: %d", ret);
        return ret;
    }

    LOG_INF("🎯 FRAM Library initialized successfully");
    LOG_INF("Pin P0.%02d is shared between FRAM CS and LED", led_gpio.pin);

    /* Run demonstration functions */
    ret = demo_fram_operations();
    if (ret < 0)
        return ret;

    k_sleep(K_SECONDS(1));

    ret = demo_led_operations();
    if (ret < 0)
        return ret;

    k_sleep(K_SECONDS(1));

    ret = demo_mixed_operations();
    if (ret < 0)
        return ret;

    k_sleep(K_SECONDS(1));

    ret = demo_error_handling();
    if (ret < 0)
        return ret;

    LOG_INF("🎉 All demonstrations completed successfully!");

    /* Final demonstration: continuous operation */
    LOG_INF("Starting continuous demo (Ctrl+C to stop)...");

    uint32_t counter = 0;
    while (1)
    {
        /* Store counter in FRAM */
        uint32_t addr = 0x5000;
        ret = juxta_fram_write(&fram_dev, addr, (uint8_t *)&counter, sizeof(counter));
        if (ret < 0)
        {
            LOG_ERR("FRAM write failed: %d", ret);
            break;
        }

        /* Switch to LED mode and flash */
        juxta_fram_led_mode_enable(&fram_dev);
        juxta_fram_led_on(&fram_dev);
        k_sleep(K_MSEC(100));
        juxta_fram_led_off(&fram_dev);

        /* Read back from FRAM */
        uint32_t read_counter;
        ret = juxta_fram_read(&fram_dev, addr, (uint8_t *)&read_counter, sizeof(read_counter));
        if (ret < 0)
        {
            LOG_ERR("FRAM read failed: %d", ret);
            break;
        }

        LOG_INF("Counter: %u (FRAM verified)", read_counter);

        counter++;
        k_sleep(K_SECONDS(2));
    }

    return 0;
}