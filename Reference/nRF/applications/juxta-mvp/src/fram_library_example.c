/*
 * Example usage of the JUXTA FRAM Library
 * This demonstrates the clean API for FRAM operations
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <juxta_fram/fram.h>
#include <string.h>

LOG_MODULE_REGISTER(fram_example, LOG_LEVEL_DBG);

/* Device tree definitions */
#define LED_NODE DT_ALIAS(led0)
#define FRAM_NODE DT_ALIAS(spi_fram)

/* GPIO specifications */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED_NODE, gpios);

/* FRAM device instance */
static struct juxta_fram_device fram_dev;

/**
 * @brief Example of FRAM library usage
 */
int fram_library_example_main(void)
{
    int ret;

    LOG_INF("Starting FRAM Library Example");

    /* Initialize FRAM using the library */
    ret = juxta_fram_init_dt(&fram_dev, DEVICE_DT_GET(FRAM_NODE), &led);
    if (ret < 0)
    {
        LOG_ERR("Failed to initialize FRAM: %d", ret);
        return ret;
    }

    /* Verify FRAM device ID */
    struct juxta_fram_id id;
    ret = juxta_fram_read_id(&fram_dev, &id);
    if (ret < 0)
    {
        LOG_ERR("Failed to verify FRAM ID: %d", ret);
        return ret;
    }

    LOG_INF("FRAM Device ID verified:");
    LOG_INF("  Manufacturer: 0x%02X", id.manufacturer_id);
    LOG_INF("  Continuation: 0x%02X", id.continuation_code);
    LOG_INF("  Product ID 1: 0x%02X", id.product_id_1);
    LOG_INF("  Product ID 2: 0x%02X", id.product_id_2);

    /* Run built-in test */
    ret = juxta_fram_test(&fram_dev, 0x1000); /* Test at address 0x1000 */
    if (ret < 0)
    {
        LOG_ERR("FRAM test failed: %d", ret);
        return ret;
    }

    /* Example: Write a string to FRAM */
    const char *test_string = "Hello FRAM Library!";
    uint32_t string_addr = 0x2000;

    ret = juxta_fram_write(&fram_dev, string_addr,
                           (const uint8_t *)test_string,
                           strlen(test_string) + 1); /* +1 for null terminator */
    if (ret < 0)
    {
        LOG_ERR("Failed to write string to FRAM: %d", ret);
        return ret;
    }

    LOG_INF("Wrote string to FRAM at address 0x%06X", string_addr);

    /* Example: Read the string back */
    char read_buffer[64] = {0};
    ret = juxta_fram_read(&fram_dev, string_addr,
                          (uint8_t *)read_buffer,
                          strlen(test_string) + 1);
    if (ret < 0)
    {
        LOG_ERR("Failed to read string from FRAM: %d", ret);
        return ret;
    }

    LOG_INF("Read string from FRAM: '%s'", read_buffer);

    /* Verify the data matches */
    if (strcmp(test_string, read_buffer) == 0)
    {
        LOG_INF("✅ String read/write test passed!");
    }
    else
    {
        LOG_ERR("❌ String read/write test failed!");
        return -1;
    }

    /* Example: Write and read individual bytes */
    uint32_t data_addr = 0x3000;
    uint8_t test_data[] = {0xAA, 0x55, 0xFF, 0x00, 0x12, 0x34, 0x56, 0x78};
    uint8_t read_data[sizeof(test_data)] = {0};

    ret = juxta_fram_write(&fram_dev, data_addr, test_data, sizeof(test_data));
    if (ret < 0)
    {
        LOG_ERR("Failed to write test data: %d", ret);
        return ret;
    }

    ret = juxta_fram_read(&fram_dev, data_addr, read_data, sizeof(read_data));
    if (ret < 0)
    {
        LOG_ERR("Failed to read test data: %d", ret);
        return ret;
    }

    /* Verify byte data */
    bool data_match = true;
    for (size_t i = 0; i < sizeof(test_data); i++)
    {
        if (test_data[i] != read_data[i])
        {
            LOG_ERR("Data mismatch at index %zu: wrote 0x%02X, read 0x%02X",
                    i, test_data[i], read_data[i]);
            data_match = false;
        }
    }

    if (data_match)
    {
        LOG_INF("✅ Byte array read/write test passed!");
    }
    else
    {
        LOG_ERR("❌ Byte array read/write test failed!");
        return -1;
    }

    /* Example: Use convenience functions for single bytes */
    uint32_t byte_addr = 0x4000;
    uint8_t write_byte = 0xA5;
    uint8_t read_byte;

    ret = juxta_fram_write_byte(&fram_dev, byte_addr, write_byte);
    if (ret < 0)
    {
        LOG_ERR("Failed to write single byte: %d", ret);
        return ret;
    }

    ret = juxta_fram_read_byte(&fram_dev, byte_addr, &read_byte);
    if (ret < 0)
    {
        LOG_ERR("Failed to read single byte: %d", ret);
        return ret;
    }

    if (write_byte == read_byte)
    {
        LOG_INF("✅ Single byte read/write test passed! (0x%02X)", read_byte);
    }
    else
    {
        LOG_ERR("❌ Single byte test failed: wrote 0x%02X, read 0x%02X",
                write_byte, read_byte);
        return -1;
    }

    LOG_INF("🎉 All FRAM library tests completed successfully!");

    /* Main loop - demonstrate periodic FRAM operations */
    uint32_t counter = 0;
    uint32_t counter_addr = 0x5000;

    while (1)
    {
        /* Write counter value to FRAM */
        ret = juxta_fram_write(&fram_dev, counter_addr,
                               (uint8_t *)&counter, sizeof(counter));
        if (ret < 0)
        {
            LOG_ERR("Failed to write counter: %d", ret);
        }
        else
        {
            LOG_INF("Stored counter value %u to FRAM", counter);
        }

        counter++;
        k_sleep(K_SECONDS(5));

        /* Every 10 iterations, read back and verify */
        if ((counter % 10) == 0)
        {
            uint32_t read_counter;
            ret = juxta_fram_read(&fram_dev, counter_addr,
                                  (uint8_t *)&read_counter, sizeof(read_counter));
            if (ret < 0)
            {
                LOG_ERR("Failed to read counter: %d", ret);
            }
            else
            {
                LOG_INF("Read counter from FRAM: %u", read_counter);
                if (read_counter == (counter - 1))
                {
                    LOG_INF("✅ Counter verification passed");
                }
                else
                {
                    LOG_ERR("❌ Counter verification failed");
                }
            }
        }
    }

    return 0;
}