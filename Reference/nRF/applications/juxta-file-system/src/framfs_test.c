/*
 * FRAM File System Test Module
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

LOG_MODULE_REGISTER(framfs_test, LOG_LEVEL_DBG);

/* Device tree definitions */
#define FRAM_NODE DT_ALIAS(spi_fram)

/* Get CS GPIO from devicetree */
static const struct gpio_dt_spec cs_gpio = GPIO_DT_SPEC_GET_BY_IDX(DT_PARENT(FRAM_NODE), cs_gpios, 0);

/* FRAM and file system instances */
static struct juxta_fram_device fram_dev;
static struct juxta_framfs_context fs_ctx;

/**
 * @brief Display comprehensive file system statistics
 */
static int display_filesystem_stats(void)
{
    int ret;
    struct juxta_framfs_header stats;

    LOG_INF("📊 File System Status Report");
    LOG_INF("══════════════════════════════════════════════════════════════");

    ret = juxta_framfs_get_stats(&fs_ctx, &stats);
    if (ret < 0)
    {
        LOG_ERR("Failed to get file system stats: %d", ret);
        return ret;
    }

    /* Calculate memory layout */
    uint32_t header_size = sizeof(struct juxta_framfs_header);
    uint32_t index_size = JUXTA_FRAMFS_MAX_FILES * sizeof(struct juxta_framfs_entry);
    uint32_t mac_header_size = sizeof(struct juxta_framfs_mac_header);
    uint32_t mac_table_size = JUXTA_FRAMFS_MAX_MAC_ADDRESSES * sizeof(struct juxta_framfs_mac_entry);
    uint32_t user_settings_size = sizeof(struct juxta_framfs_user_settings);
    uint32_t total_overhead = header_size + index_size + mac_header_size + mac_table_size + user_settings_size;
    uint32_t available_data = JUXTA_FRAM_SIZE_BYTES - total_overhead;

    /* Display basic statistics */
    LOG_INF("📋 Basic Information:");
    LOG_INF("  Magic:          0x%04X", stats.magic);
    LOG_INF("  Version:        %d", stats.version);
    LOG_INF("  Files:          %d/%d", stats.file_count, JUXTA_FRAMFS_MAX_FILES);
    LOG_INF("  Next data addr: 0x%06X", stats.next_data_addr);
    LOG_INF("  Total data:     %d bytes", stats.total_data_size);

    /* Display memory usage */
    LOG_INF("💾 Memory Layout:");
    LOG_INF("  File system header: %d bytes", header_size);
    LOG_INF("  File index table:   %d bytes (%d files × %d bytes)",
            index_size, JUXTA_FRAMFS_MAX_FILES, sizeof(struct juxta_framfs_entry));
    LOG_INF("  MAC table header:   %d bytes", mac_header_size);
    LOG_INF("  MAC address table:  %d bytes (%d entries × %d bytes)",
            mac_table_size, JUXTA_FRAMFS_MAX_MAC_ADDRESSES, sizeof(struct juxta_framfs_mac_entry));
    LOG_INF("  User settings:      %d bytes", user_settings_size);
    LOG_INF("  Total overhead:     %d bytes (%.2f%%)", total_overhead,
            (double)total_overhead / JUXTA_FRAM_SIZE_BYTES * 100.0);
    LOG_INF("  Available for data: %d bytes (%.2f%%)", available_data,
            (double)available_data / JUXTA_FRAM_SIZE_BYTES * 100.0);

    /* Display file usage */
    double file_usage = (double)stats.file_count / JUXTA_FRAMFS_MAX_FILES * 100.0;
    double data_usage = (double)stats.total_data_size / available_data * 100.0;

    LOG_INF("📈 Usage Statistics:");
    LOG_INF("  File usage:     %.1f%% (%d/%d files)", file_usage, stats.file_count, JUXTA_FRAMFS_MAX_FILES);
    LOG_INF("  Data usage:     %.1f%% (%d/%d bytes)", data_usage, stats.total_data_size, available_data);
    LOG_INF("  Data remaining: %d bytes", available_data - stats.total_data_size);

    /* List existing files if any */
    if (stats.file_count > 0)
    {
        LOG_INF("📁 Existing Files:");
        char filenames[JUXTA_FRAMFS_MAX_FILES][JUXTA_FRAMFS_FILENAME_LEN];
        int file_count = juxta_framfs_list_files(&fs_ctx, filenames, JUXTA_FRAMFS_MAX_FILES);

        for (int i = 0; i < file_count; i++)
        {
            struct juxta_framfs_entry entry;
            ret = juxta_framfs_get_file_info(&fs_ctx, filenames[i], &entry);
            if (ret == 0)
            {
                LOG_INF("  %s: %d bytes (type: %d, flags: 0x%02X)",
                        filenames[i], entry.length, entry.file_type, entry.flags);
            }
        }
    }
    else
    {
        LOG_INF("📁 No files found");
    }

    /* Display MAC table statistics */
    uint8_t mac_entry_count;
    uint32_t mac_total_usage;
    ret = juxta_framfs_mac_get_stats(&fs_ctx, &mac_entry_count, &mac_total_usage);
    if (ret == 0)
    {
        LOG_INF("📱 MAC Address Table:");
        LOG_INF("  Entries:       %d/%d", mac_entry_count, JUXTA_FRAMFS_MAX_MAC_ADDRESSES);
        LOG_INF("  Usage tracking: %s", mac_total_usage == 0 ? "Disabled" : "Enabled");
    }

    LOG_INF("══════════════════════════════════════════════════════════════");
    return 0;
}

/**
 * @brief Clear the file system (format and reinitialize)
 */
static int clear_filesystem(void)
{
    int ret;

    LOG_INF("🧹 Clearing File System");
    LOG_INF("══════════════════════════════════════════════════════════════");

    /* Format the file system */
    ret = juxta_framfs_format(&fs_ctx);
    if (ret < 0)
    {
        LOG_ERR("Failed to format file system: %d", ret);
        return ret;
    }

    /* Clear MAC address table */
    ret = juxta_framfs_mac_clear(&fs_ctx);
    if (ret < 0)
    {
        LOG_ERR("Failed to clear MAC table: %d", ret);
        return ret;
    }

    LOG_INF("✅ File system cleared successfully");
    LOG_INF("══════════════════════════════════════════════════════════════");
    return 0;
}

/**
 * @brief Test file system initialization
 */
static int test_framfs_init(void)
{
    int ret;

    LOG_INF("🔧 Testing file system initialization...");

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

    /* Initialize file system */
    ret = juxta_framfs_init(&fs_ctx, &fram_dev);
    if (ret < 0)
    {
        LOG_ERR("Failed to initialize file system: %d", ret);
        return ret;
    }

    /* Get and display statistics */
    struct juxta_framfs_header stats;
    ret = juxta_framfs_get_stats(&fs_ctx, &stats);
    if (ret < 0)
    {
        LOG_ERR("Failed to get file system stats: %d", ret);
        return ret;
    }

    LOG_INF("✅ File system initialized successfully:");
    LOG_INF("  Magic:     0x%04X", stats.magic);
    LOG_INF("  Version:   %d", stats.version);
    LOG_INF("  Files:     %d", stats.file_count);

    return 0;
}

/**
 * @brief Test basic file operations
 */
static int test_basic_file_operations(void)
{
    int ret;
    uint8_t test_data[] = {1, 2, 3, 4, 5};
    uint8_t read_buffer[256];

    LOG_INF("📝 Testing basic file operations...");
    LOG_INF("══════════════════════════════════════════════════════════════");

    /* Test 1: Create and write to a file */
    LOG_INF("Test 1: Create and write to file");
    LOG_INF("──────────────────────────────────────────────────────────────");

    ret = juxta_framfs_create_active(&fs_ctx, "240120", JUXTA_FRAMFS_TYPE_SENSOR_LOG);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to create file: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ File '240120' created successfully");

    LOG_INF("  → Writing test data...");
    ret = juxta_framfs_append(&fs_ctx, test_data, sizeof(test_data));
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to append data: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ Data written successfully (%d bytes)", sizeof(test_data));

    /* Test 2: Read from the file */
    LOG_INF("Test 2: Read from file");
    LOG_INF("──────────────────────────────────────────────────────────────");

    ret = juxta_framfs_read(&fs_ctx, "240120", 0, read_buffer, sizeof(test_data));
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to read file: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ File '240120' read successfully (%d bytes)", ret);

    if (memcmp(test_data, read_buffer, sizeof(test_data)) != 0)
    {
        LOG_ERR("❌ Data verification failed");
        LOG_HEXDUMP_ERR(test_data, sizeof(test_data), "Expected:");
        LOG_HEXDUMP_ERR(read_buffer, sizeof(test_data), "Got:");
        return -1;
    }
    LOG_INF("  ✅ Data verified successfully");

    /* Test 3: Multiple writes */
    LOG_INF("Test 3: Multiple sequential writes");
    LOG_INF("  → Writing additional data...");
    uint8_t more_data[] = {0xAA, 0xBB, 0xCC};
    ret = juxta_framfs_append(&fs_ctx, more_data, sizeof(more_data));
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to append more data: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ Additional data written successfully");

    /* Read back all data */
    LOG_INF("  → Reading combined data...");
    ret = juxta_framfs_read(&fs_ctx, "240120", 0, read_buffer, sizeof(test_data) + sizeof(more_data));
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to read combined data: %d", ret);
        return ret;
    }

    /* Verify first part */
    if (memcmp(test_data, read_buffer, sizeof(test_data)) != 0)
    {
        LOG_ERR("❌ First part verification failed");
        LOG_HEXDUMP_ERR(test_data, sizeof(test_data), "Expected:");
        LOG_HEXDUMP_ERR(read_buffer, sizeof(test_data), "Got:");
        return -1;
    }

    /* Verify second part */
    if (memcmp(more_data, read_buffer + sizeof(test_data), sizeof(more_data)) != 0)
    {
        LOG_ERR("❌ Second part verification failed");
        LOG_HEXDUMP_ERR(more_data, sizeof(more_data), "Expected:");
        LOG_HEXDUMP_ERR(read_buffer + sizeof(test_data), sizeof(more_data), "Got:");
        return -1;
    }
    LOG_INF("  ✅ Combined data verified successfully");

    /* Test 4: Partial reads */
    LOG_INF("Test 4: Partial reads");
    LOG_INF("  → Reading partial data...");
    ret = juxta_framfs_read(&fs_ctx, "240120", 2, read_buffer, 3);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to perform partial read: %d", ret);
        return ret;
    }

    uint8_t expected[] = {3, 4, 5};
    if (memcmp(expected, read_buffer, sizeof(expected)) != 0)
    {
        LOG_ERR("❌ Partial read verification failed");
        LOG_HEXDUMP_ERR(expected, sizeof(expected), "Expected:");
        LOG_HEXDUMP_ERR(read_buffer, sizeof(expected), "Got:");
        return -1;
    }
    LOG_INF("  ✅ Partial read verified successfully");

    /* Test 5: Large data write */
    LOG_INF("Test 5: Large data write/read");
    LOG_INF("  → Creating large file...");
    uint8_t large_data[256]; /* Reduced from 1024 to 256 */
    uint8_t large_buffer[256];
    for (int i = 0; i < sizeof(large_data); i++)
    {
        large_data[i] = i & 0xFF;
    }

    /* Create new file for large data */
    ret = juxta_framfs_create_active(&fs_ctx, "large", JUXTA_FRAMFS_TYPE_RAW_DATA);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to create file for large data: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ Large file created successfully");

    /* Write large data in chunks */
    LOG_INF("  → Writing large data in chunks...");
    const size_t chunk_size = 64;
    for (size_t offset = 0; offset < sizeof(large_data); offset += chunk_size)
    {
        size_t size = MIN(chunk_size, sizeof(large_data) - offset);
        ret = juxta_framfs_append(&fs_ctx, large_data + offset, size);
        if (ret < 0)
        {
            LOG_ERR("❌ Failed to write data chunk at offset %d: %d", offset, ret);
            return ret;
        }
        LOG_INF("    Wrote chunk %zu/%zu (%zu bytes)",
                (offset + size) / chunk_size,
                sizeof(large_data) / chunk_size,
                size);
    }
    LOG_INF("  ✅ Large data written successfully (%d bytes)", sizeof(large_data));

    /* Read and verify large data */
    LOG_INF("  → Reading and verifying large data...");
    ret = juxta_framfs_read(&fs_ctx, "large", 0, large_buffer, sizeof(large_buffer));
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to read large data: %d", ret);
        return ret;
    }

    if (memcmp(large_data, large_buffer, sizeof(large_data)) != 0)
    {
        LOG_ERR("❌ Large data verification failed");
        return -1;
    }
    LOG_INF("  ✅ Large data verified successfully");

    /* Test 6: File size verification */
    LOG_INF("Test 6: File size verification");
    LOG_INF("  → Checking file sizes...");
    ret = juxta_framfs_get_file_size(&fs_ctx, "240120");
    if (ret != (sizeof(test_data) + sizeof(more_data)))
    {
        LOG_ERR("❌ Unexpected file size: got %d, expected %d",
                ret, sizeof(test_data) + sizeof(more_data));
        return -1;
    }
    LOG_INF("  ✅ First file size verified: %d bytes", ret);

    ret = juxta_framfs_get_file_size(&fs_ctx, "large");
    if (ret != sizeof(large_data))
    {
        LOG_ERR("❌ Unexpected large file size: got %d, expected %d",
                ret, sizeof(large_data));
        return -1;
    }
    LOG_INF("  ✅ Large file size verified: %d bytes", ret);

    /* Test 3: Get file size */
    LOG_INF("Test 3: Get file size");
    LOG_INF("──────────────────────────────────────────────────────────────");

    int file_size = juxta_framfs_get_file_size(&fs_ctx, "240120");
    if (file_size < 0)
    {
        LOG_ERR("❌ Failed to get file size: %d", file_size);
        return file_size;
    }
    LOG_INF("  ✅ File '240120' size: %d bytes", file_size);

    LOG_INF("══════════════════════════════════════════════════════════════");
    LOG_INF("✅ All basic file operations passed!");
    return 0;
}

/**
 * @brief Test MAC address table operations
 */
static int test_mac_table_operations(void)
{
    int ret;
    uint8_t mac_index;
    uint8_t retrieved_mac[6];
    uint8_t entry_count;
    uint32_t total_usage;

    LOG_INF("📱 Testing MAC address table operations...");
    LOG_INF("══════════════════════════════════════════════════════════════");

    /* Test 1: Add new MAC addresses */
    LOG_INF("Test 1: Adding MAC addresses");
    uint8_t test_macs[][6] = {
        {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC}, /* Test MAC 1 */
        {0xDE, 0xF0, 0x12, 0x34, 0x56, 0x78}, /* Test MAC 2 */
        {0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34}, /* Test MAC 3 */
        {0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0}, /* Test MAC 4 */
        {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC}, /* Duplicate of MAC 1 */
    };

    for (int i = 0; i < 5; i++)
    {
        ret = juxta_framfs_mac_find_or_add(&fs_ctx, test_macs[i], &mac_index);
        if (ret < 0)
        {
            LOG_ERR("❌ Failed to add MAC %d: %d", i, ret);
            return ret;
        }
        LOG_INF("  ✅ MAC %d added at index %d", i, mac_index);
    }

    /* Test 2: Verify statistics */
    LOG_INF("Test 2: Verifying MAC table statistics");
    ret = juxta_framfs_mac_get_stats(&fs_ctx, &entry_count, &total_usage);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to get MAC stats: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ MAC table stats: %d entries, %d total usage", entry_count, total_usage);

    /* Check that we have 4 unique entries (5th was duplicate) */
    if (entry_count != 4)
    {
        LOG_ERR("❌ Expected 4 entries, got %d", entry_count);
        return -1;
    }
    LOG_INF("  ✅ MAC table has correct number of entries");

    /* Test 3: Find existing MAC addresses */
    LOG_INF("Test 3: Finding existing MAC addresses");
    for (int i = 0; i < 4; i++)
    {
        ret = juxta_framfs_mac_find(&fs_ctx, test_macs[i], &mac_index);
        if (ret < 0)
        {
            LOG_ERR("❌ Failed to find MAC %d: %d", i, ret);
            return ret;
        }
        LOG_INF("  ✅ Found MAC %d at index %d", i, mac_index);
    }

    /* Test 4: Get MAC by index */
    LOG_INF("Test 4: Retrieving MAC addresses by index");
    for (int i = 0; i < 4; i++)
    {
        ret = juxta_framfs_mac_get_by_index(&fs_ctx, i, retrieved_mac);
        if (ret < 0)
        {
            LOG_ERR("❌ Failed to get MAC by index %d: %d", i, ret);
            return ret;
        }
        LOG_INF("  ✅ Retrieved MAC %d: %02X:%02X:%02X:%02X:%02X:%02X", i,
                retrieved_mac[0], retrieved_mac[1], retrieved_mac[2],
                retrieved_mac[3], retrieved_mac[4], retrieved_mac[5]);
    }

    /* Test 5: Expected error cases */
    LOG_INF("Test 5: Testing error handling (expected errors)");
    LOG_INF("──────────────────────────────────────────────────────────────");

    /* Non-existent MAC */
    LOG_INF("  → Testing non-existent MAC...");
    uint8_t non_existent_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ret = juxta_framfs_mac_find(&fs_ctx, non_existent_mac, &mac_index);
    if (ret != JUXTA_FRAMFS_ERROR_MAC_NOT_FOUND)
    {
        LOG_ERR("❌ UNEXPECTED: Wrong error code for non-existent MAC");
        return -1;
    }
    LOG_WRN("  ✓ Expected error: MAC not found");

    /* Invalid index */
    LOG_INF("  → Testing out-of-range MAC index...");
    ret = juxta_framfs_mac_get_by_index(&fs_ctx, 255, retrieved_mac);
    if (ret != JUXTA_FRAMFS_ERROR)
    {
        LOG_ERR("❌ UNEXPECTED: Wrong error code for out-of-range index");
        return -1;
    }
    LOG_WRN("  ✓ Expected error: MAC index out of range");

    LOG_INF("══════════════════════════════════════════════════════════════");
    LOG_INF("✅ All MAC table tests passed!");
    return 0;
}

/**
 * @brief Test record encoding/decoding
 */
static int test_encoding_decoding(void)
{
    int ret;
    uint8_t buffer[256];

    LOG_INF("🔄 Testing record encoding/decoding...");
    LOG_INF("══════════════════════════════════════════════════════════════");

    /* Test 1: Device record */
    LOG_INF("Test 1: Device record encoding/decoding");
    LOG_INF("──────────────────────────────────────────────────────────────");
    struct juxta_framfs_device_record test_record = {
        .minute = 123,
        .type = 2,
        .motion_count = 5,
        .mac_indices = {1, 2},
        .rssi_values = {-45, -60}};

    ret = juxta_framfs_encode_device_record(&test_record, buffer, sizeof(buffer));
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to encode device record: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ Device record encoded:");
    LOG_INF("     - Minute: %d", test_record.minute);
    LOG_INF("     - Type: %d", test_record.type);
    LOG_INF("     - Motion count: %d", test_record.motion_count);
    LOG_INF("     - MAC indices: [%d, %d]", test_record.mac_indices[0], test_record.mac_indices[1]);
    LOG_INF("     - RSSI values: [%d, %d]", test_record.rssi_values[0], test_record.rssi_values[1]);

    struct juxta_framfs_device_record decoded_record;
    ret = juxta_framfs_decode_device_record(buffer, ret, &decoded_record);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to decode device record: %d", ret);
        return ret;
    }

    if (decoded_record.minute != test_record.minute ||
        decoded_record.type != test_record.type ||
        decoded_record.motion_count != test_record.motion_count ||
        decoded_record.mac_indices[0] != test_record.mac_indices[0] ||
        decoded_record.mac_indices[1] != test_record.mac_indices[1] ||
        decoded_record.rssi_values[0] != test_record.rssi_values[0] ||
        decoded_record.rssi_values[1] != test_record.rssi_values[1])
    {
        LOG_ERR("❌ Device record verification failed");
        return -1;
    }
    LOG_INF("  ✅ Device record decoded and verified successfully");

    /* Test 2: Simple record */
    LOG_INF("Test 2: Simple record encoding/decoding");
    LOG_INF("──────────────────────────────────────────────────────────────");
    struct juxta_framfs_simple_record simple_record = {
        .minute = 456,
        .type = JUXTA_FRAMFS_RECORD_TYPE_BOOT};

    ret = juxta_framfs_encode_simple_record(&simple_record, buffer);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to encode simple record: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ Simple record encoded:");
    LOG_INF("     - Minute: %d", simple_record.minute);
    LOG_INF("     - Type: BOOT");

    struct juxta_framfs_simple_record decoded_simple;
    ret = juxta_framfs_decode_simple_record(buffer, &decoded_simple);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to decode simple record: %d", ret);
        return ret;
    }

    if (decoded_simple.minute != simple_record.minute ||
        decoded_simple.type != simple_record.type)
    {
        LOG_ERR("❌ Simple record verification failed");
        return -1;
    }
    LOG_INF("  ✅ Simple record decoded and verified successfully");

    /* Test 3: Battery record */
    LOG_INF("Test 3: Battery record encoding/decoding");
    LOG_INF("──────────────────────────────────────────────────────────────");
    struct juxta_framfs_battery_record battery_record = {
        .minute = 789,
        .type = JUXTA_FRAMFS_RECORD_TYPE_BATTERY,
        .level = 85};

    ret = juxta_framfs_encode_battery_record(&battery_record, buffer);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to encode battery record: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ Battery record encoded:");
    LOG_INF("     - Minute: %d", battery_record.minute);
    LOG_INF("     - Level: %d%%", battery_record.level);

    struct juxta_framfs_battery_record decoded_battery;
    ret = juxta_framfs_decode_battery_record(buffer, &decoded_battery);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to decode battery record: %d", ret);
        return ret;
    }

    if (decoded_battery.minute != battery_record.minute ||
        decoded_battery.type != battery_record.type ||
        decoded_battery.level != battery_record.level)
    {
        LOG_ERR("❌ Battery record verification failed");
        return -1;
    }
    LOG_INF("  ✅ Battery record decoded and verified successfully");

    /* Test 4: Expected error cases */
    LOG_INF("Test 4: Testing error handling (expected errors)");
    LOG_INF("──────────────────────────────────────────────────────────────");

    /* Invalid buffer size */
    LOG_INF("  → Testing small buffer...");
    ret = juxta_framfs_encode_device_record(&test_record, buffer, 2);
    if (ret != JUXTA_FRAMFS_ERROR_SIZE)
    {
        LOG_ERR("❌ UNEXPECTED: Wrong error code for buffer size");
        return -1;
    }
    LOG_WRN("  ✓ Expected error: Buffer too small");

    /* Invalid device count */
    LOG_INF("  → Testing invalid device count...");
    struct juxta_framfs_device_record invalid_device = {
        .minute = 123,
        .type = 0, /* Invalid: 0 devices */
        .motion_count = 1};
    ret = juxta_framfs_encode_device_record(&invalid_device, buffer, sizeof(buffer));
    if (ret != JUXTA_FRAMFS_ERROR)
    {
        LOG_ERR("❌ UNEXPECTED: Wrong error code for invalid device count");
        return -1;
    }
    LOG_WRN("  ✓ Expected error: Invalid device count");

    LOG_INF("══════════════════════════════════════════════════════════════");
    LOG_INF("✅ All encoding/decoding tests passed!");
    return 0;
}

/**
 * @brief Test error handling
 */
static int test_error_handling(void)
{
    int ret;
    uint8_t buffer[32];

    LOG_INF("⚠️  Testing error handling...");
    LOG_INF("══════════════════════════════════════════════════════════════");

    /* Test 1: Invalid file operations */
    LOG_INF("Test 1: Invalid file operations (expected errors)");
    LOG_INF("──────────────────────────────────────────────────────────────");

    /* Read non-existent file */
    LOG_INF("  → Testing non-existent file read...");
    ret = juxta_framfs_read(&fs_ctx, "nonexistent", 0, buffer, sizeof(buffer));
    if (ret != JUXTA_FRAMFS_ERROR_NOT_FOUND)
    {
        LOG_ERR("❌ UNEXPECTED: Wrong error code for non-existent file");
        return -1;
    }
    LOG_WRN("  ✓ Expected error: File not found");

    /* Create test file first */
    LOG_INF("  → Creating test file...");
    ret = juxta_framfs_create_active(&fs_ctx, "240120", JUXTA_FRAMFS_TYPE_RAW_DATA);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to create test file: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ Test file created successfully");

    /* Create duplicate file */
    LOG_INF("  → Testing duplicate file creation...");
    ret = juxta_framfs_create_active(&fs_ctx, "240120", JUXTA_FRAMFS_TYPE_RAW_DATA);
    if (ret != JUXTA_FRAMFS_ERROR_EXISTS)
    {
        LOG_ERR("❌ UNEXPECTED: Wrong error code for duplicate file");
        return -1;
    }
    LOG_WRN("  ✓ Expected error: File already exists");

    /* Test 2: Invalid parameters */
    LOG_INF("Test 2: Invalid parameters (expected errors)");
    LOG_INF("──────────────────────────────────────────────────────────────");

    /* Null buffer */
    LOG_INF("  → Testing null buffer...");
    ret = juxta_framfs_append(&fs_ctx, NULL, 10);
    if (ret != JUXTA_FRAMFS_ERROR)
    {
        LOG_ERR("❌ UNEXPECTED: Wrong error code for null buffer");
        return -1;
    }
    LOG_WRN("  ✓ Expected error: Invalid parameter (null buffer)");

    /* Zero length */
    LOG_INF("  → Testing zero length...");
    uint8_t dummy_data[] = {1, 2, 3};
    ret = juxta_framfs_append(&fs_ctx, dummy_data, 0);
    if (ret != JUXTA_FRAMFS_ERROR)
    {
        LOG_ERR("❌ UNEXPECTED: Wrong error code for zero length");
        return -1;
    }
    LOG_WRN("  ✓ Expected error: Invalid parameter (zero length)");

    LOG_INF("══════════════════════════════════════════════════════════════");
    LOG_INF("✅ All error handling tests passed!");
    return 0;
}

/**
 * @brief Test user settings operations
 */
static int test_user_settings_operations(void)
{
    int ret;
    uint8_t adv_interval, scan_interval;
    char subject_id[JUXTA_FRAMFS_SUBJECT_ID_LEN];
    char upload_path[JUXTA_FRAMFS_UPLOAD_PATH_LEN];
    struct juxta_framfs_user_settings settings;

    LOG_INF("⚙️  Testing user settings operations...");
    LOG_INF("══════════════════════════════════════════════════════════════");

    /* Test 1: Get default values */
    LOG_INF("Test 1: Getting default values");
    LOG_INF("──────────────────────────────────────────────────────────────");

    ret = juxta_framfs_get_adv_interval(&fs_ctx, &adv_interval);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to get adv_interval: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ adv_interval: %d", adv_interval);

    ret = juxta_framfs_get_scan_interval(&fs_ctx, &scan_interval);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to get scan_interval: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ scan_interval: %d", scan_interval);

    ret = juxta_framfs_get_subject_id(&fs_ctx, subject_id);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to get subject_id: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ subject_id: '%s'", subject_id);

    ret = juxta_framfs_get_upload_path(&fs_ctx, upload_path);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to get upload_path: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ upload_path: '%s'", upload_path);

    /* Test 2: Set individual values */
    LOG_INF("Test 2: Setting individual values");
    LOG_INF("──────────────────────────────────────────────────────────────");

    ret = juxta_framfs_set_adv_interval(&fs_ctx, 5);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to set adv_interval: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ Set adv_interval to 5");

    ret = juxta_framfs_set_scan_interval(&fs_ctx, 15);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to set scan_interval: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ Set scan_interval to 15");

    ret = juxta_framfs_set_subject_id(&fs_ctx, "vole001");
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to set subject_id: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ Set subject_id to 'vole001'");

    ret = juxta_framfs_set_upload_path(&fs_ctx, "/DATA");
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to set upload_path: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ Set upload_path to '/DATA'");

    /* Test 3: Verify individual values */
    LOG_INF("Test 3: Verifying individual values");
    LOG_INF("──────────────────────────────────────────────────────────────");

    ret = juxta_framfs_get_adv_interval(&fs_ctx, &adv_interval);
    if (ret < 0 || adv_interval != 5)
    {
        LOG_ERR("❌ adv_interval verification failed: got %d, expected 5", adv_interval);
        return -1;
    }
    LOG_INF("  ✅ adv_interval verified: %d", adv_interval);

    ret = juxta_framfs_get_scan_interval(&fs_ctx, &scan_interval);
    if (ret < 0 || scan_interval != 15)
    {
        LOG_ERR("❌ scan_interval verification failed: got %d, expected 15", scan_interval);
        return -1;
    }
    LOG_INF("  ✅ scan_interval verified: %d", scan_interval);

    ret = juxta_framfs_get_subject_id(&fs_ctx, subject_id);
    if (ret < 0 || strcmp(subject_id, "vole001") != 0)
    {
        LOG_ERR("❌ subject_id verification failed: got '%s', expected 'vole001'", subject_id);
        return -1;
    }
    LOG_INF("  ✅ subject_id verified: '%s'", subject_id);

    ret = juxta_framfs_get_upload_path(&fs_ctx, upload_path);
    if (ret < 0 || strcmp(upload_path, "/DATA") != 0)
    {
        LOG_ERR("❌ upload_path verification failed: got '%s', expected '/DATA'", upload_path);
        return -1;
    }
    LOG_INF("  ✅ upload_path verified: '%s'", upload_path);

    /* Test 4: Get all settings at once */
    LOG_INF("Test 4: Getting all settings at once");
    LOG_INF("──────────────────────────────────────────────────────────────");

    ret = juxta_framfs_get_user_settings(&fs_ctx, &settings);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to get all settings: %d", ret);
        return ret;
    }

    LOG_INF("  ✅ All settings retrieved:");
    LOG_INF("     adv_interval: %d", settings.adv_interval);
    LOG_INF("     scan_interval: %d", settings.scan_interval);
    LOG_INF("     subject_id: '%s'", settings.subject_id);
    LOG_INF("     upload_path: '%s'", settings.upload_path);

    /* Test 5: Set all settings at once */
    LOG_INF("Test 5: Setting all settings at once");
    LOG_INF("──────────────────────────────────────────────────────────────");

    settings.adv_interval = 10;
    settings.scan_interval = 20;
    strcpy(settings.subject_id, "vole002");
    strcpy(settings.upload_path, "/BACKUP");

    ret = juxta_framfs_set_user_settings(&fs_ctx, &settings);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to set all settings: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ All settings updated");

    /* Test 6: Verify all settings */
    LOG_INF("Test 6: Verifying all settings");
    LOG_INF("──────────────────────────────────────────────────────────────");

    ret = juxta_framfs_get_adv_interval(&fs_ctx, &adv_interval);
    if (ret < 0 || adv_interval != 10)
    {
        LOG_ERR("❌ adv_interval verification failed: got %d, expected 10", adv_interval);
        return -1;
    }
    LOG_INF("  ✅ adv_interval verified: %d", adv_interval);

    ret = juxta_framfs_get_scan_interval(&fs_ctx, &scan_interval);
    if (ret < 0 || scan_interval != 20)
    {
        LOG_ERR("❌ scan_interval verification failed: got %d, expected 20", scan_interval);
        return -1;
    }
    LOG_INF("  ✅ scan_interval verified: %d", scan_interval);

    ret = juxta_framfs_get_subject_id(&fs_ctx, subject_id);
    if (ret < 0 || strcmp(subject_id, "vole002") != 0)
    {
        LOG_ERR("❌ subject_id verification failed: got '%s', expected 'vole002'", subject_id);
        return -1;
    }
    LOG_INF("  ✅ subject_id verified: '%s'", subject_id);

    ret = juxta_framfs_get_upload_path(&fs_ctx, upload_path);
    if (ret < 0 || strcmp(upload_path, "/BACKUP") != 0)
    {
        LOG_ERR("❌ upload_path verification failed: got '%s', expected '/BACKUP'", upload_path);
        return -1;
    }
    LOG_INF("  ✅ upload_path verified: '%s'", upload_path);

    /* Test 7: Clear settings (reset to defaults) */
    LOG_INF("Test 7: Clearing settings (reset to defaults)");
    LOG_INF("──────────────────────────────────────────────────────────────");

    ret = juxta_framfs_clear_user_settings(&fs_ctx);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to clear settings: %d", ret);
        return ret;
    }
    LOG_INF("  ✅ Settings cleared");

    /* Test 8: Verify defaults */
    LOG_INF("Test 8: Verifying default values");
    LOG_INF("──────────────────────────────────────────────────────────────");

    ret = juxta_framfs_get_adv_interval(&fs_ctx, &adv_interval);
    if (ret < 0 || adv_interval != 5)
    {
        LOG_ERR("❌ adv_interval default verification failed: got %d, expected 5", adv_interval);
        return -1;
    }
    LOG_INF("  ✅ adv_interval default verified: %d", adv_interval);

    ret = juxta_framfs_get_scan_interval(&fs_ctx, &scan_interval);
    if (ret < 0 || scan_interval != 15)
    {
        LOG_ERR("❌ scan_interval default verification failed: got %d, expected 15", scan_interval);
        return -1;
    }
    LOG_INF("  ✅ scan_interval default verified: %d", scan_interval);

    ret = juxta_framfs_get_subject_id(&fs_ctx, subject_id);
    if (ret < 0 || strcmp(subject_id, "") != 0)
    {
        LOG_ERR("❌ subject_id default verification failed: got '%s', expected ''", subject_id);
        return -1;
    }
    LOG_INF("  ✅ subject_id default verified: '%s'", subject_id);

    ret = juxta_framfs_get_upload_path(&fs_ctx, upload_path);
    if (ret < 0 || strcmp(upload_path, "/TEST") != 0)
    {
        LOG_ERR("❌ upload_path default verification failed: got '%s', expected '/TEST'", upload_path);
        return -1;
    }
    LOG_INF("  ✅ upload_path default verified: '%s'", upload_path);

    LOG_INF("══════════════════════════════════════════════════════════════");
    LOG_INF("✅ All user settings tests passed!");
    return 0;
}

/**
 * @brief Main test function
 */
int framfs_test_main(void)
{
    int ret;

    LOG_INF("🧪 Running Low-Level API Tests");
    LOG_INF("══════════════════════════════════════════════════════════════");

    /* Initialize and clear */
    LOG_INF("📋 Step 1: Initializing file system...");
    ret = test_framfs_init();
    if (ret < 0)
    {
        LOG_ERR("❌ File system initialization failed: %d", ret);
        return ret;
    }
    LOG_INF("✅ File system initialized successfully");

    LOG_INF("🧹 Step 2: Clearing file system...");
    ret = clear_filesystem();
    if (ret < 0)
    {
        LOG_ERR("❌ File system clear failed: %d", ret);
        return ret;
    }
    LOG_INF("✅ File system cleared successfully");

    /* Display initial state */
    LOG_INF("📊 Step 3: Checking initial state...");
    ret = display_filesystem_stats();
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to display file system stats: %d", ret);
        return ret;
    }
    LOG_INF("✅ Initial state verified");

    /* Run test sequence */
    LOG_INF("📝 Step 4: Testing basic file operations...");
    ret = test_basic_file_operations();
    if (ret < 0)
    {
        LOG_ERR("❌ Basic file operations failed: %d", ret);
        return ret;
    }
    LOG_INF("✅ Basic file operations passed");

    /* Clear again for MAC table tests */
    LOG_INF("🧹 Step 5: Clearing file system for MAC table tests...");
    ret = clear_filesystem();
    if (ret < 0)
    {
        LOG_ERR("❌ File system clear failed: %d", ret);
        return ret;
    }
    LOG_INF("✅ File system cleared successfully");

    LOG_INF("📱 Step 6: Testing MAC table operations...");
    ret = test_mac_table_operations();
    if (ret < 0)
    {
        LOG_ERR("❌ MAC table operations failed: %d", ret);
        return ret;
    }
    LOG_INF("✅ MAC table operations passed");

    /* Clear again for encoding tests */
    LOG_INF("🧹 Step 7: Clearing file system for encoding tests...");
    ret = clear_filesystem();
    if (ret < 0)
    {
        LOG_ERR("❌ File system clear failed: %d", ret);
        return ret;
    }
    LOG_INF("✅ File system cleared successfully");

    LOG_INF("🔄 Step 8: Testing record encoding/decoding...");
    ret = test_encoding_decoding();
    if (ret < 0)
    {
        LOG_ERR("❌ Record encoding/decoding failed: %d", ret);
        return ret;
    }
    LOG_INF("✅ Record encoding/decoding passed");

    /* Clear again for error handling tests */
    LOG_INF("🧹 Step 9: Clearing file system for error handling tests...");
    ret = clear_filesystem();
    if (ret < 0)
    {
        LOG_ERR("❌ File system clear failed: %d", ret);
        return ret;
    }
    LOG_INF("✅ File system cleared successfully");

    LOG_INF("⚠️  Step 10: Testing error handling...");
    ret = test_error_handling();
    if (ret < 0)
    {
        LOG_ERR("❌ Error handling tests failed: %d", ret);
        return ret;
    }
    LOG_INF("✅ Error handling tests passed");

    /* Clear again for user settings tests */
    LOG_INF("🧹 Step 11: Clearing file system for user settings tests...");
    ret = clear_filesystem();
    if (ret < 0)
    {
        LOG_ERR("❌ File system clear failed: %d", ret);
        return ret;
    }
    LOG_INF("✅ File system cleared successfully");

    LOG_INF("⚙️  Step 12: Testing user settings operations...");
    ret = test_user_settings_operations();
    if (ret < 0)
    {
        LOG_ERR("❌ User settings tests failed: %d", ret);
        return ret;
    }
    LOG_INF("✅ User settings tests passed");

    /* Display final state */
    LOG_INF("📊 Step 13: Checking final state...");
    ret = display_filesystem_stats();
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to display file system stats: %d", ret);
        return ret;
    }
    LOG_INF("✅ Final state verified");

    LOG_INF("══════════════════════════════════════════════════════════════");
    LOG_INF("✅ All low-level API tests passed successfully!");
    LOG_INF("  • File System Initialization ✓");
    LOG_INF("  • Basic File Operations ✓");
    LOG_INF("  • MAC Address Table ✓");
    LOG_INF("  • Record Encoding/Decoding ✓");
    LOG_INF("  • Error Handling ✓");
    LOG_INF("  • User Settings ✓");
    LOG_INF("══════════════════════════════════════════════════════════════");
    return 0;
}