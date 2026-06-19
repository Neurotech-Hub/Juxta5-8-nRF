/*
 * FRAM File System Time-Aware API Test Module
 *
 * Copyright (c) 2024 NeurotechHub
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <juxta_fram/fram.h>
#include <juxta_framfs/framfs.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(framfs_time_test, LOG_LEVEL_DBG);

/* Device tree definitions */
#define FRAM_NODE DT_ALIAS(spi_fram)

/* Get CS GPIO from devicetree */
static const struct gpio_dt_spec cs_gpio = GPIO_DT_SPEC_GET_BY_IDX(DT_PARENT(FRAM_NODE), cs_gpios, 0);

/* FRAM and file system instances */
static struct juxta_fram_device fram_dev;
static struct juxta_framfs_context fs_ctx;
static struct juxta_framfs_ctx time_ctx;

/* Mock RTC function for testing */
static uint32_t get_test_rtc_date(void)
{
    /* Return YYMMDD format for 2024-01-20 */
    return 240120; /* 24 = year, 01 = month, 20 = day */
}

/**
 * @brief Test time-aware API initialization
 */
static int test_time_api_init(void)
{
    int ret;

    LOG_INF("🔧 Testing time-aware API initialization...");

    /* Initialize FRAM first */
    const struct device *spi_dev = DEVICE_DT_GET(DT_BUS(FRAM_NODE));
    if (!device_is_ready(spi_dev))
    {
        LOG_ERR("SPI device not ready");
        return JUXTA_FRAMFS_ERROR_INIT;
    }

    ret = juxta_fram_init(&fram_dev, spi_dev, 8000000, &cs_gpio);
    if (ret < 0)
    {
        LOG_ERR("Failed to initialize FRAM: %d", ret);
        return ret;
    }

    /* Initialize basic file system */
    ret = juxta_framfs_init(&fs_ctx, &fram_dev);
    if (ret < 0)
    {
        LOG_ERR("Failed to initialize file system: %d", ret);
        return ret;
    }

    /* Initialize time-aware context */
    ret = juxta_framfs_init_with_time(&time_ctx, &fs_ctx, get_test_rtc_date, true);
    if (ret < 0)
    {
        LOG_ERR("Failed to initialize time-aware context: %d", ret);
        return ret;
    }

    LOG_INF("✅ Time-aware API initialized successfully");
    LOG_INF("  Current date: %s", time_ctx.current_filename);
    LOG_INF("  Auto management: %s", time_ctx.auto_file_management ? "enabled" : "disabled");

    return 0;
}

/**
 * @brief Test file management operations
 */
static int test_time_file_management(void)
{
    int ret;
    struct juxta_framfs_header header;

    LOG_INF("📁 Testing time-aware file management...");
    LOG_INF("══════════════════════════════════════════════════════════════");

    /* Show initial file system state */
    ret = juxta_framfs_get_stats(&fs_ctx, &header);
    if (ret == 0)
    {
        LOG_INF("Initial file system state - Files: %d, Next addr: 0x%08X",
                header.file_count, header.next_data_addr);
    }

    /* Format the file system to start fresh */
    LOG_INF("Test 1: Format and initialize");
    ret = juxta_framfs_format(&fs_ctx);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to format file system: %d", ret);
        return ret;
    }

    /* Verify format worked */
    ret = juxta_framfs_get_stats(&fs_ctx, &header);
    if (ret < 0 || header.file_count != 0)
    {
        LOG_ERR("❌ File system format verification failed - Files: %d", header.file_count);
        return ret < 0 ? ret : -1;
    }
    LOG_INF("  ✅ File system formatted successfully");

    /* Initialize time-aware context with mock RTC date */
    ret = juxta_framfs_init_with_time(&time_ctx, &fs_ctx, get_test_rtc_date, true);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to initialize time context: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ Time-aware context initialized");

    /* Ensure initial file is created */
    ret = juxta_framfs_ensure_current_file(&time_ctx);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to ensure current file: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ Initial file created");

    /* Test basic file operations with time-aware API */
    LOG_INF("  → Testing basic file operations...");

    /* Write some test data - should create file "240120" */
    uint8_t test_data[] = {0x01, 0x02, 0x03, 0x04};
    ret = juxta_framfs_append_data(&time_ctx, test_data, sizeof(test_data));
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to append test data: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ Test data written to file 240120");

    /* Read and verify data */
    uint8_t read_buffer[32];
    char current_file[JUXTA_FRAMFS_FILENAME_LEN];
    ret = juxta_framfs_get_current_filename(&time_ctx, current_file);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to get current filename: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ Current filename: %s", current_file);

    ret = juxta_framfs_read(&fs_ctx, current_file, 0, read_buffer, sizeof(test_data));
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to read data: %d", ret);
        return ret;
    }

    if (memcmp(test_data, read_buffer, sizeof(test_data)) != 0)
    {
        LOG_ERR("❌ Data verification failed");
        return -1;
    }
    LOG_INF("  ✅ Data verified successfully");

    /* Test 3: Record type handling */
    LOG_INF("Test 3: Record type handling");
    LOG_INF("──────────────────────────────────────────────────────────────");

    /* Write boot record */
    LOG_INF("  → Writing boot record...");
    ret = juxta_framfs_append_simple_record_data(&time_ctx, 456, JUXTA_FRAMFS_RECORD_TYPE_BOOT);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to append boot record: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ Boot record written: minute=456, type=BOOT");

    /* Write battery record */
    LOG_INF("  → Writing battery record...");
    ret = juxta_framfs_append_battery_record_data(&time_ctx, 789, 85);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to append battery record: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ Battery record written: minute=789, level=85%%");

    /* Write device scan record */
    LOG_INF("  → Writing device scan record...");
    uint8_t mac_ids[][3] = {
        {0x55, 0x66, 0x77},  /* Last 3 bytes of MAC: 0x556677 */
        {0xEE, 0xFF, 0x00}}; /* Last 3 bytes of MAC: 0xEEFF00 */
    int8_t rssi_values[] = {-45, -60};
    ret = juxta_framfs_append_device_scan_data(&time_ctx, 123, 5,
                                               mac_ids, rssi_values, 2);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to append device scan: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ Device scan record written:");
    LOG_INF("     - Minute: 123");
    LOG_INF("     - Motion count: 5");
    LOG_INF("     - Device 1: MAC ID %02X%02X%02X (RSSI: %d)",
            mac_ids[0][0], mac_ids[0][1], mac_ids[0][2], rssi_values[0]);
    LOG_INF("     - Device 2: MAC ID %02X%02X%02X (RSSI: %d)",
            mac_ids[1][0], mac_ids[1][1], mac_ids[1][2], rssi_values[1]);

    /* Test 4: Record decoding */
    LOG_INF("Test 4: Record decoding");
    LOG_INF("──────────────────────────────────────────────────────────────");

    /* Get file size */
    ret = juxta_framfs_get_file_size(&fs_ctx, current_file);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to get file size: %d", ret);
        return ret;
    }

    /* Read entire file */
    uint8_t file_buffer[1024];
    ret = juxta_framfs_read(&fs_ctx, current_file, 0, file_buffer, ret);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to read file: %d", ret);
        return ret;
    }

    /* Decode and verify records */
    uint32_t offset = sizeof(test_data); /* Skip test data */

    /* Verify boot record */
    struct juxta_framfs_simple_record simple_record;
    ret = juxta_framfs_decode_simple_record(file_buffer + offset, &simple_record);
    if (ret < 0 || simple_record.minute != 456 || simple_record.type != JUXTA_FRAMFS_RECORD_TYPE_BOOT)
    {
        LOG_ERR("❌ Boot record verification failed");
        return ret < 0 ? ret : -1;
    }
    offset += ret;
    LOG_INF("  ✅ Boot record verified successfully");

    /* Verify battery record */
    struct juxta_framfs_battery_record battery_record;
    ret = juxta_framfs_decode_battery_record(file_buffer + offset, &battery_record);
    if (ret < 0 || battery_record.minute != 789 || battery_record.level != 85)
    {
        LOG_ERR("❌ Battery record verification failed");
        return ret < 0 ? ret : -1;
    }
    offset += ret;
    LOG_INF("  ✅ Battery record verified successfully");

    /* Verify device scan record */
    struct juxta_framfs_device_record device_record;
    ret = juxta_framfs_decode_device_record(file_buffer + offset,
                                            sizeof(file_buffer) - offset, /* Use remaining buffer size */
                                            &device_record);
    if (ret < 0 || device_record.minute != 123 || device_record.motion_count != 5)
    {
        LOG_ERR("❌ Device scan record verification failed");
        return ret < 0 ? ret : -1;
    }
    LOG_INF("  ✅ Device scan record verified successfully");

    /* Verify final state */
    ret = juxta_framfs_get_stats(&fs_ctx, &header);
    if (ret == 0)
    {
        LOG_INF("Final file system state - Files: %d, Next addr: 0x%08X",
                header.file_count, header.next_data_addr);
    }

    LOG_INF("══════════════════════════════════════════════════════════════");
    LOG_INF("✅ All time-aware file management tests passed!");
    return 0;
}

/**
 * @brief Test error handling and edge cases
 */
static int test_time_error_handling(void)
{
    int ret;

    LOG_INF("⚠️  Testing time-aware error handling (errors below are EXPECTED)...");
    LOG_INF("──────────────────────────────────────────────────────────────");

    /* Test 1: Invalid data parameters */
    LOG_INF("📝 Test 1: Invalid data parameters");

    /* Test null data */
    LOG_INF("  → Testing null data (expect ERROR)...");
    ret = juxta_framfs_append_data(&time_ctx, NULL, 10);
    if (ret >= 0)
    {
        LOG_ERR("❌ UNEXPECTED: Null data was accepted");
        return -1;
    }
    LOG_WRN("  ✓ Expected error: Null data rejected");

    /* Test zero length */
    LOG_INF("  → Testing zero length (expect ERROR)...");
    uint8_t dummy_data[] = {1, 2, 3};
    ret = juxta_framfs_append_data(&time_ctx, dummy_data, 0);
    if (ret >= 0)
    {
        LOG_ERR("❌ UNEXPECTED: Zero length was accepted");
        return -1;
    }
    LOG_WRN("  ✓ Expected error: Zero length rejected");

    /* Test 2: Invalid file operations */
    LOG_INF("📝 Test 2: Invalid file operations");

    /* Try to read non-existent file */
    LOG_INF("  → Testing non-existent file read (expect ERROR)...");
    uint8_t read_buffer[10];
    ret = juxta_framfs_read(&fs_ctx, "nonexistent", 0, read_buffer, sizeof(read_buffer));
    if (ret != JUXTA_FRAMFS_ERROR_NOT_FOUND)
    {
        LOG_ERR("❌ UNEXPECTED: Wrong error code for non-existent file");
        return -1;
    }
    LOG_WRN("  ✓ Expected error: File not found");

    /* Try to read past end of file */
    LOG_INF("  → Testing read beyond file size (expect ERROR)...");
    char current_file[JUXTA_FRAMFS_FILENAME_LEN];
    ret = juxta_framfs_get_current_filename(&time_ctx, current_file);
    if (ret == 0)
    {
        ret = juxta_framfs_read(&fs_ctx, current_file, 0xFFFF, read_buffer, sizeof(read_buffer));
        if (ret >= 0)
        {
            LOG_ERR("❌ UNEXPECTED: Read beyond file size was accepted");
            return -1;
        }
        LOG_WRN("  ✓ Expected error: Read beyond file size");
    }

    /* Test 3: File creation constraints */
    LOG_INF("📝 Test 3: File creation constraints");

    /* Try to create duplicate file */
    LOG_INF("  → Testing duplicate file creation (expect ERROR)...");
    ret = juxta_framfs_create_active(&fs_ctx, current_file, JUXTA_FRAMFS_TYPE_RAW_DATA);
    if (ret != JUXTA_FRAMFS_ERROR_EXISTS)
    {
        LOG_ERR("❌ UNEXPECTED: Wrong error code for duplicate file");
        return -1;
    }
    LOG_WRN("  ✓ Expected error: File already exists");

    /* Test direct append to sealed file */
    LOG_INF("  → Testing append to sealed file (expect ERROR)...");
    ret = juxta_framfs_seal_active(&fs_ctx);
    if (ret < 0)
    {
        LOG_ERR("❌ UNEXPECTED: Failed to seal file: %d", ret);
        return -1;
    }
    LOG_INF("  ℹ️  File sealed successfully");

    ret = juxta_framfs_append(&fs_ctx, dummy_data, sizeof(dummy_data));
    if (ret != JUXTA_FRAMFS_ERROR_NO_ACTIVE)
    {
        LOG_ERR("❌ UNEXPECTED: Wrong error code for sealed file append");
        return -1;
    }
    LOG_WRN("  ✓ Expected error: No active file");

    /* Test file name length limits */
    LOG_INF("  → Testing filename length limit (expect ERROR)...");
    ret = juxta_framfs_create_active(&fs_ctx, "this_filename_is_way_too_long", JUXTA_FRAMFS_TYPE_RAW_DATA);
    if (ret != JUXTA_FRAMFS_ERROR_SIZE)
    {
        LOG_ERR("❌ UNEXPECTED: Wrong error code for long filename");
        return -1;
    }
    LOG_WRN("  ✓ Expected error: Filename too long");

    LOG_INF("──────────────────────────────────────────────────────────────");
    LOG_INF("✅ All error handling tests passed (expected errors verified)");
    return 0;
}

/**
 * @brief Main time-aware API test function
 */
int framfs_time_test_main(void)
{
    int ret;

    LOG_INF("⏰ Starting Time-Aware API Test Suite");
    LOG_INF("══════════════════════════════════════════════════════════════");

    /* Step 1: Initialize time-aware API with mock RTC */
    ret = test_time_api_init();
    if (ret < 0)
        return ret;

    /* Step 2: Test basic file operations */
    ret = test_time_file_management();
    if (ret < 0)
        return ret;

    /* Step 3: Test file system error handling */
    ret = test_time_error_handling();
    if (ret < 0)
        return ret;

    LOG_INF("🎉 All time-aware API tests completed!");
    LOG_INF("══════════════════════════════════════════════════════════════");

    return 0;
}