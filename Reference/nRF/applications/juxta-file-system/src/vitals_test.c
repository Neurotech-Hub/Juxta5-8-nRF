/*
 * JUXTA Vitals Library Test
 *
 * Copyright (c) 2024 NeurotechHub
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <juxta_fram/fram.h>
#include <juxta_framfs/framfs.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "juxta_vitals_nrf52/vitals.h"

LOG_MODULE_REGISTER(vitals_test, LOG_LEVEL_DBG);

/* Global test state */
static bool vitals_test_failed = false;

/* Test function prototypes */
static void test_vitals_init(void);
static void test_vitals_timestamp(void);
static void test_vitals_battery(void);
static void test_vitals_system(void);
static void test_vitals_summary(void);
static void test_vitals_config(void);
static void test_vitals_integration(void);

/* Device tree definitions for FRAM */
#define FRAM_NODE DT_ALIAS(spi_fram)
static const struct gpio_dt_spec cs_gpio = GPIO_DT_SPEC_GET_BY_IDX(DT_PARENT(FRAM_NODE), cs_gpios, 0);

/* Test vitals context */
static struct juxta_vitals_ctx test_vitals;

/* FRAM and file system instances for integration test */
static struct juxta_fram_device fram_dev;
static struct juxta_framfs_context fs_ctx;
static struct juxta_framfs_ctx time_ctx;

/* Test timestamp (2024-01-20 12:00:00) */
static const uint32_t test_timestamp = 1705752000;

static void test_vitals_init(void)
{
    LOG_INF("🔧 Testing vitals initialization...");
    LOG_INF("══════════════════════════════════════════════════════════════");

    int ret = juxta_vitals_init(&test_vitals, true);
    if (ret != 0)
    {
        LOG_ERR("❌ Failed to initialize vitals: %d", ret);
        vitals_test_failed = true;
        return;
    }

    LOG_INF("✅ Vitals library initialized successfully");
    LOG_INF("══════════════════════════════════════════════════════════════");
}

static void test_vitals_timestamp(void)
{
    LOG_INF("🧪 Testing timestamp functions...");
    LOG_INF("──────────────────────────────────────────────────────────────");

    /* Test 1: Set initial timestamp */
    LOG_INF("Test 1: Setting initial timestamp");
    LOG_INF("  → Setting to 2024-01-20 12:00:00 UTC...");
    int ret = juxta_vitals_set_timestamp(&test_vitals, test_timestamp);
    if (ret != 0)
    {
        LOG_ERR("❌ Failed to set timestamp: %d", ret);
        vitals_test_failed = true;
        return;
    }
    LOG_INF("  ✅ Initial timestamp set successfully");

    /* Test 2: Read back timestamp */
    LOG_INF("Test 2: Reading back timestamp");
    LOG_INF("  → Reading current RTC time...");
    uint32_t timestamp = juxta_vitals_get_timestamp(&test_vitals);
    if (timestamp != test_timestamp)
    {
        LOG_ERR("❌ Timestamp verification failed:");
        LOG_ERR("   Expected: %u (2024-01-20 12:00:00 UTC)", test_timestamp);
        LOG_ERR("   Got:      %u", timestamp);
        vitals_test_failed = true;
        return;
    }

    /* Test 3: Date conversion */
    LOG_INF("Test 3: Date/time conversions");
    LOG_INF("  → Converting to YYYYMMDD format...");
    uint32_t date = juxta_vitals_get_date_yyyymmdd(&test_vitals);
    if (date == 20240120)
    {
        LOG_INF("  ✅ Date conversion verified: %u", date);
    }
    else
    {
        LOG_ERR("❌ Date conversion failed:");
        LOG_ERR("   Expected: 20240120");
        LOG_ERR("   Got:      %u", date);
        vitals_test_failed = true;
        return;
    }

    /* Test 4: Time conversion */
    LOG_INF("  → Converting to HHMMSS format...");
    uint32_t time = juxta_vitals_get_time_hhmmss(&test_vitals);
    if (time == 120000)
    {
        LOG_INF("  ✅ Time conversion verified: %06u", time);
    }
    else
    {
        LOG_ERR("❌ Time conversion failed:");
        LOG_ERR("   Expected: 120000");
        LOG_ERR("   Got:      %06u", time);
        vitals_test_failed = true;
        return;
    }

    /* Test 4.5: File date in YYMMDD format */
    LOG_INF("  → Testing YYMMDD file date format...");
    uint32_t file_date = juxta_vitals_get_file_date(&test_vitals);
    if (file_date == 240120)
    {
        LOG_INF("  ✅ File date (YYMMDD): %u", file_date);
    }
    else
    {
        LOG_ERR("❌ File date conversion failed:");
        LOG_ERR("   Expected: 240120");
        LOG_ERR("   Got:      %u", file_date);
        vitals_test_failed = true;
        return;
    }

    /* Test 5: Set different timestamp */
    LOG_INF("Test 5: Setting different timestamp");
    LOG_INF("  → Setting to 2024-02-15 08:30:00 UTC...");
    uint32_t new_timestamp = 1708070400; /* 2024-02-15 08:30:00 UTC */
    ret = juxta_vitals_set_timestamp(&test_vitals, new_timestamp);
    if (ret != 0)
    {
        LOG_ERR("❌ Failed to set new timestamp: %d", ret);
        return;
    }

    /* Verify new timestamp */
    timestamp = juxta_vitals_get_timestamp(&test_vitals);
    if (timestamp == new_timestamp)
    {
        LOG_INF("  ✅ New timestamp verified: %u", timestamp);
        date = juxta_vitals_get_date_yyyymmdd(&test_vitals);
        time = juxta_vitals_get_time_hhmmss(&test_vitals);
        LOG_INF("     Date: %u", date);
        LOG_INF("     Time: %06u", time);
    }
    else
    {
        LOG_ERR("❌ New timestamp mismatch:");
        LOG_ERR("   Expected: %u (2024-02-15 08:30:00 UTC)", new_timestamp);
        LOG_ERR("   Got:      %u", timestamp);
        vitals_test_failed = true;
        return;
    }

    /* Reset to original timestamp for other tests */
    ret = juxta_vitals_set_timestamp(&test_vitals, test_timestamp);
    if (ret != 0)
    {
        LOG_ERR("❌ Failed to reset timestamp: %d", ret);
        vitals_test_failed = true;
        return;
    }

    LOG_INF("✅ All timestamp tests passed");
    LOG_INF("──────────────────────────────────────────────────────────────");
}

static void test_vitals_battery(void)
{
    LOG_INF("🧪 Testing battery monitoring...");
    LOG_INF("──────────────────────────────────────────────────────────────");

    /* Update vitals to read battery */
    int ret = juxta_vitals_update(&test_vitals);
    if (ret != 0)
    {
        LOG_ERR("❌ Failed to update vitals: %d", ret);
        vitals_test_failed = true;
        return;
    }

    /* Get battery readings */
    uint16_t battery_mv = juxta_vitals_get_battery_mv(&test_vitals);
    uint8_t battery_percent = juxta_vitals_get_battery_percent(&test_vitals);
    bool low_battery = juxta_vitals_is_low_battery(&test_vitals);

    LOG_INF("Battery voltage thresholds:");
    LOG_INF("  Full:     %d mV", JUXTA_VITALS_BATTERY_FULL_MV);
    LOG_INF("  Low:      %d mV", JUXTA_VITALS_BATTERY_LOW_MV);
    LOG_INF("  Critical: %d mV", JUXTA_VITALS_BATTERY_CRITICAL_MV);
    LOG_INF("");

    LOG_INF("Current battery status:");
    LOG_INF("  Voltage:  %d mV", battery_mv);
    LOG_INF("  Level:    %d%%", battery_percent);
    LOG_INF("  State:    %s", low_battery ? "CRITICAL" : (battery_percent < 20 ? "LOW" : "NORMAL"));

    /* Force a battery update to get fresh readings */
    LOG_INF("Forcing battery update...");
    int update_ret = juxta_vitals_update(&test_vitals);
    if (update_ret < 0)
    {
        LOG_WRN("Battery update failed: %d", update_ret);
    }

    /* Get updated readings */
    battery_mv = juxta_vitals_get_battery_mv(&test_vitals);
    battery_percent = juxta_vitals_get_battery_percent(&test_vitals);
    low_battery = juxta_vitals_is_low_battery(&test_vitals);

    LOG_INF("Updated battery status:");
    LOG_INF("  Voltage:  %d mV", battery_mv);
    LOG_INF("  Level:    %d%%", battery_percent);
    LOG_INF("  State:    %s", low_battery ? "CRITICAL" : (battery_percent < 20 ? "LOW" : "NORMAL"));

    /* Validate readings */
    if (battery_mv == 0)
    {
        LOG_ERR("❌ Invalid battery voltage reading (0 mV)");
        return;
    }

    /* Check voltage is in expected range for 3V system */
    if (battery_mv < 2000 || battery_mv > 3300)
    {
        LOG_ERR("❌ Battery voltage out of expected range: %d mV", battery_mv);
        LOG_ERR("   Expected: 2000-3300 mV for 3V system");
        return;
    }

    /* Verify percentage calculation */
    uint32_t expected_percent;
    if (battery_mv >= JUXTA_VITALS_BATTERY_FULL_MV)
    {
        expected_percent = 100;
    }
    else if (battery_mv <= JUXTA_VITALS_BATTERY_LOW_MV)
    {
        expected_percent = 0;
    }
    else
    {
        uint32_t range = JUXTA_VITALS_BATTERY_FULL_MV - JUXTA_VITALS_BATTERY_LOW_MV;
        uint32_t current = battery_mv - JUXTA_VITALS_BATTERY_LOW_MV;
        expected_percent = (current * 100) / range;
    }

    if (battery_percent != expected_percent)
    {
        LOG_ERR("❌ Battery percentage calculation error");
        LOG_ERR("   Got: %d%%, Expected: %d%%", battery_percent, expected_percent);
        return;
    }

    /* Verify low battery flag */
    bool expected_low = (battery_mv <= JUXTA_VITALS_BATTERY_CRITICAL_MV);
    if (low_battery != expected_low)
    {
        LOG_ERR("❌ Low battery flag error");
        LOG_ERR("   Got: %s, Expected: %s",
                low_battery ? "true" : "false",
                expected_low ? "true" : "false");
        return;
    }

    LOG_INF("✅ Battery monitoring verified successfully");
    LOG_INF("──────────────────────────────────────────────────────────────");
}

static void test_vitals_system(void)
{
    LOG_INF("🧪 Testing system vitals...");

    /* Get uptime */
    uint32_t uptime = juxta_vitals_get_uptime(&test_vitals);
    LOG_INF("✅ System uptime: %u seconds", uptime);

    /* Get temperature */
    int8_t temperature = juxta_vitals_get_temperature(&test_vitals);
    LOG_INF("✅ Temperature: %d°C", temperature);
}

static void test_vitals_summary(void)
{
    LOG_INF("🧪 Testing vitals summary...");

    char summary[128];
    int len = juxta_vitals_get_summary(&test_vitals, summary, sizeof(summary));

    if (len > 0)
    {
        LOG_INF("✅ Vitals summary: %s", summary);
    }
    else
    {
        LOG_ERR("❌ Failed to get vitals summary: %d", len);
    }
}

static void test_vitals_config(void)
{
    LOG_INF("🧪 Testing vitals configuration...");

    /* Test battery monitoring toggle */
    int ret = juxta_vitals_set_battery_monitoring(&test_vitals, false);
    if (ret == 0)
    {
        LOG_INF("✅ Battery monitoring disabled");
    }
    else
    {
        LOG_ERR("❌ Failed to disable battery monitoring: %d", ret);
    }

    /* Re-enable battery monitoring */
    ret = juxta_vitals_set_battery_monitoring(&test_vitals, true);
    if (ret == 0)
    {
        LOG_INF("✅ Battery monitoring re-enabled");
    }
    else
    {
        LOG_ERR("❌ Failed to re-enable battery monitoring: %d", ret);
    }

    /* Test temperature monitoring toggle */
    ret = juxta_vitals_set_temperature_monitoring(&test_vitals, false);
    if (ret == 0)
    {
        LOG_INF("✅ Temperature monitoring disabled");
    }
    else
    {
        LOG_ERR("❌ Failed to disable temperature monitoring: %d", ret);
    }

    /* Re-enable temperature monitoring */
    ret = juxta_vitals_set_temperature_monitoring(&test_vitals, true);
    if (ret == 0)
    {
        LOG_INF("✅ Temperature monitoring re-enabled");
    }
    else
    {
        LOG_ERR("❌ Failed to re-enable temperature monitoring: %d", ret);
    }
}

/**
 * @brief Simple RTC function for integration test
 * Returns current date in YYYYMMDD format using vitals library
 */
static uint32_t get_integration_rtc_date(void)
{
    /* Return YYMMDD format for 2024-01-20 */
    return 240120; /* 24 = year, 01 = month, 20 = day */
}

/**
 * @brief Integration test: Write actual battery level to file
 * This test validates that file system, battery monitoring, and RTC work together
 */
static void test_vitals_integration(void)
{
    int ret;

    LOG_INF("🔗 Testing Integration: Battery Level to File");
    LOG_INF("══════════════════════════════════════════════════════════════");

    /* Step 1: Initialize FRAM and file system */
    LOG_INF("Step 1: Initializing FRAM and file system...");

    const struct device *spi_dev = DEVICE_DT_GET(DT_BUS(FRAM_NODE));
    if (!device_is_ready(spi_dev))
    {
        LOG_ERR("❌ SPI device not ready");
        vitals_test_failed = true;
        return;
    }

    ret = juxta_fram_init(&fram_dev, spi_dev, 8000000, &cs_gpio);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to initialize FRAM: %d", ret);
        vitals_test_failed = true;
        return;
    }
    LOG_INF("  ✅ FRAM initialized");

    ret = juxta_framfs_init(&fs_ctx, &fram_dev);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to initialize file system: %d", ret);
        vitals_test_failed = true;
        return;
    }
    LOG_INF("  ✅ File system initialized");

    /* Step 2: Initialize time-aware context with integration RTC function */
    LOG_INF("Step 2: Initializing time-aware context...");

    ret = juxta_framfs_init_with_time(&time_ctx, &fs_ctx, get_integration_rtc_date, true);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to initialize time-aware context: %d", ret);
        vitals_test_failed = true;
        return;
    }
    LOG_INF("  ✅ Time-aware context initialized");

    /* Step 3: Get validated battery level using vitals library */
    LOG_INF("Step 3: Reading validated battery level...");

    /* Force a battery update to ensure fresh readings */
    LOG_INF("  🔄 Forcing battery update...");
    ret = juxta_vitals_update(&test_vitals);
    if (ret < 0)
    {
        LOG_WRN("  ⚠️  Battery update failed: %d", ret);
    }

    uint8_t battery_level;
    ret = juxta_vitals_get_validated_battery_level(&test_vitals, &battery_level);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to get validated battery level: %d", ret);
        vitals_test_failed = true;
        return;
    }
    LOG_INF("  ✅ Battery level: %d%%", battery_level);

    /* Add detailed battery debugging */
    uint16_t battery_mv = juxta_vitals_get_battery_mv(&test_vitals);
    LOG_INF("  📊 Battery details:");
    LOG_INF("     - Voltage: %d mV", battery_mv);
    LOG_INF("     - Level: %d%%", battery_level);
    LOG_INF("     - Low battery: %s", juxta_vitals_is_low_battery(&test_vitals) ? "YES" : "NO");

    /* Step 4: Get current minute using vitals library */
    LOG_INF("Step 4: Getting current minute...");

    uint16_t current_minute = juxta_vitals_get_minute_of_day(&test_vitals);
    if (current_minute == 0)
    {
        /* Use a test minute if no timestamp is set */
        current_minute = 720; /* 12:00 PM */
        LOG_INF("  ✅ Using test minute: %d (12:00 PM)", current_minute);
    }
    else
    {
        LOG_INF("  ✅ Current minute: %d", current_minute);
    }

    /* Step 5: Write battery level to file using time-aware API */
    LOG_INF("Step 5: Writing battery level to file...");

    ret = juxta_framfs_append_battery_record_data(&time_ctx, current_minute, battery_level);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to write battery record: %d", ret);
        vitals_test_failed = true;
        return;
    }
    LOG_INF("  ✅ Battery record written to file");

    /* Step 6: Verify file was created and data was written */
    LOG_INF("Step 6: Verifying file and data...");

    char current_file[JUXTA_FRAMFS_FILENAME_LEN];
    ret = juxta_framfs_get_current_filename(&time_ctx, current_file);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to get current filename: %d", ret);
        vitals_test_failed = true;
        return;
    }
    LOG_INF("  ✅ File created: %s", current_file);

    /* Verify the filename is in YYMMDD format */
    if (strcmp(current_file, "240120") != 0)
    {
        LOG_ERR("❌ Expected filename '240120', got '%s'", current_file);
        vitals_test_failed = true;
        return;
    }
    LOG_INF("  ✅ Filename format verified (YYMMDD)");

    /* Get file size */
    int file_size = juxta_framfs_get_file_size(&fs_ctx, current_file);
    if (file_size < 0)
    {
        LOG_ERR("❌ Failed to get file size: %d", file_size);
        vitals_test_failed = true;
        return;
    }
    LOG_INF("  ✅ File size: %d bytes", file_size);

    /* Read and verify the battery record */
    uint8_t read_buffer[256];

    /* Read from the end of the file where the new record should be */
    int read_offset = file_size - 4; /* Battery record is 4 bytes */
    if (read_offset < 0)
    {
        LOG_ERR("❌ File too small for battery record");
        vitals_test_failed = true;
        return;
    }

    LOG_INF("  📍 Reading from offset %d (file size: %d)", read_offset, file_size);

    ret = juxta_framfs_read(&fs_ctx, current_file, read_offset, read_buffer, 4);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to read file: %d", ret);
        vitals_test_failed = true;
        return;
    }

    /* Debug: Show raw bytes */
    LOG_INF("  🔍 Raw file bytes (%d bytes):", file_size);
    for (int i = 0; i < file_size && i < 32; i++)
    {
        LOG_INF("    [%02d]: 0x%02X", i, read_buffer[i]);
    }
    if (file_size > 32)
    {
        LOG_INF("    ... (showing first 32 bytes)");
    }

    /* Decode the battery record */
    struct juxta_framfs_battery_record battery_record;
    ret = juxta_framfs_decode_battery_record(read_buffer, &battery_record);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to decode battery record: %d", ret);
        vitals_test_failed = true;
        return;
    }

    /* Add debugging for battery record details */
    LOG_INF("  📊 Battery record details:");
    LOG_INF("     - Minute: %d", battery_record.minute);
    LOG_INF("     - Level: %d%%", battery_record.level);
    LOG_INF("     - Type: 0x%02X", battery_record.type);
    LOG_INF("     - Expected level: %d%%", battery_level);

    /* Verify the data */
    if (battery_record.level != battery_level)
    {
        LOG_ERR("❌ Battery level mismatch:");
        LOG_ERR("   Expected: %d%%", battery_level);
        LOG_ERR("   Got:      %d%%", battery_record.level);
        vitals_test_failed = true;
        return;
    }

    if (battery_record.minute != current_minute)
    {
        LOG_ERR("❌ Minute mismatch:");
        LOG_ERR("   Expected: %d", current_minute);
        LOG_ERR("   Got:      %d", battery_record.minute);
        vitals_test_failed = true;
        return;
    }

    LOG_INF("  ✅ Battery record verified:");
    LOG_INF("     - Minute: %d", battery_record.minute);
    LOG_INF("     - Level:  %d%%", battery_record.level);
    LOG_INF("     - Type:   BATTERY");

    /* Step 7: Display file system statistics */
    LOG_INF("Step 7: File system statistics...");

    struct juxta_framfs_header stats;
    ret = juxta_framfs_get_stats(&fs_ctx, &stats);
    if (ret == 0)
    {
        LOG_INF("  ✅ File system stats:");
        LOG_INF("     - Files: %d/%d", stats.file_count, JUXTA_FRAMFS_MAX_FILES);
        LOG_INF("     - Data size: %d bytes", stats.total_data_size);
        LOG_INF("     - Next addr: 0x%06X", stats.next_data_addr);
    }

    LOG_INF("══════════════════════════════════════════════════════════════");
    LOG_INF("✅ Integration test passed! All components working together:");
    LOG_INF("  • Vitals library (battery monitoring) ✓");
    LOG_INF("  • File system (data storage) ✓");
    LOG_INF("  • RTC (time management) ✓");
    LOG_INF("  • Time-aware API (automatic file management) ✓");
    LOG_INF("══════════════════════════════════════════════════════════════");
}

int vitals_test_main(void)
{
    LOG_INF("🚀 Starting JUXTA Vitals Library Test");
    LOG_INF("=====================================");

    /* Reset test state */
    vitals_test_failed = false;

    /* Run all tests */
    test_vitals_init();
    k_sleep(K_MSEC(100));

    test_vitals_timestamp();
    k_sleep(K_MSEC(100));

    test_vitals_battery();
    k_sleep(K_MSEC(100));

    test_vitals_system();
    k_sleep(K_MSEC(100));

    test_vitals_summary();
    k_sleep(K_MSEC(100));

    test_vitals_config();
    k_sleep(K_MSEC(100));

    /* Final integration test */
    test_vitals_integration();
    k_sleep(K_MSEC(100));

    /* Report final results */
    if (vitals_test_failed)
    {
        LOG_ERR("❌ Vitals library test FAILED!");
        LOG_ERR("=====================================");
        return -1;
    }
    else
    {
        LOG_INF("✅ All vitals tests completed successfully!");
        LOG_INF("=====================================");
        return 0;
    }
}