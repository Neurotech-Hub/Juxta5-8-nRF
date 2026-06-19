/*
 * JUXTA BLE Service Implementation
 * Implements BLE GATT service for JUXTA Hublink protocol
 *
 * Copyright (c) 2025 NeurotechHub
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/att.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "ble_service.h"
#include "juxta_framfs/framfs.h"
#include "juxta_vitals_nrf52/vitals.h"

LOG_MODULE_REGISTER(juxta_ble_service, LOG_LEVEL_DBG);

/* Forward declarations */
static int generate_file_listing(char *buffer, size_t buffer_size);
static int send_indication(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           const void *data, uint16_t len);
static int handle_timestamp_synchronization(uint32_t timestamp);
static int handle_memory_clearing(void);
static bool validate_timestamp(uint32_t timestamp);
static void filename_indication_confirmed(struct bt_conn *conn, struct bt_gatt_indicate_params *params, uint8_t err);
static void file_transfer_indication_confirmed(struct bt_conn *conn, struct bt_gatt_indicate_params *params, uint8_t err);

/* File transfer state machine */
typedef enum
{
    FILE_TRANSFER_STATE_IDLE = 0,
    FILE_TRANSFER_STATE_LISTING_REQUESTED,
    FILE_TRANSFER_STATE_TRANSFER_REQUESTED,
    FILE_TRANSFER_STATE_TRANSFERRING,
    FILE_TRANSFER_STATE_COMPLETE,
    FILE_TRANSFER_STATE_ERROR
} file_transfer_state_t;

/* Characteristic values - will be used in later phases */
static char node_response[JUXTA_NODE_RESPONSE_MAX_SIZE] __unused;
static char gateway_command[JUXTA_GATEWAY_COMMAND_MAX_SIZE] __unused;
static char filename_request[JUXTA_FILENAME_MAX_SIZE] __unused;
static uint8_t file_transfer_chunk[JUXTA_FILE_TRANSFER_CHUNK_SIZE] __unused;

/* CCC descriptors for indications - will be used in Phase IV */
static struct bt_gatt_ccc_cfg filename_ccc_cfg[BT_GATT_CCC_MAX] __unused;
static struct bt_gatt_ccc_cfg file_transfer_ccc_cfg[BT_GATT_CCC_MAX] __unused;

/* Current connection for indications */
static struct bt_conn *current_conn = NULL;

/* Service attribute references for indications */
static const struct bt_gatt_attr *filename_char_attr = NULL;
static const struct bt_gatt_attr *file_transfer_char_attr = NULL;

/* Persistent indicate params (must live until confirmation callback) */
static struct bt_gatt_indicate_params filename_ind_params;
static struct bt_gatt_indicate_params file_transfer_ind_params;

/* External framfs context - will be set during initialization */
static struct juxta_framfs_context *framfs_ctx = NULL;

/* External vitals context - will be set during initialization */
static struct juxta_vitals_ctx *vitals_ctx = NULL;

/* Battery check helper for FRAM operations */
static bool should_allow_fram_write(void)
{
    if (!vitals_ctx || !vitals_ctx->initialized)
    {
        LOG_WRN("⚠️ Vitals context not available - allowing FRAM write");
        return true;
    }

    if (juxta_vitals_is_low_battery(vitals_ctx))
    {
        LOG_WRN("⚠️ Battery critically low (%d mV) - preventing FRAM write",
                juxta_vitals_get_battery_mv(vitals_ctx));
        return false;
    }
    return true;
}

/* Datetime synchronization callback for production flow */
static void (*datetime_sync_callback)(void) = NULL;

/* File transfer state */
static file_transfer_state_t file_transfer_state = FILE_TRANSFER_STATE_IDLE;
static bool file_transfer_active = false;
static char current_transfer_filename[JUXTA_FRAMFS_FILENAME_LEN];
static uint32_t current_transfer_offset = 0;
static int current_transfer_file_size = -1;
static bool indication_pending = false;
static uint32_t indication_timeout = 0;

/* MTU and connection state */
static uint16_t current_mtu = 23; /* Default BLE MTU */
static bool mtu_negotiated = false;

/**
 * @brief Set the framfs context for user settings access
 */
void juxta_ble_set_framfs_context(struct juxta_framfs_context *ctx)
{
    framfs_ctx = ctx;
    LOG_INF("📁 BLE service linked to framfs context");
}

/**
 * @brief Set the vitals context for timestamp synchronization
 */
void juxta_ble_set_vitals_context(struct juxta_vitals_ctx *ctx)
{
    vitals_ctx = ctx;
    LOG_INF("⏰ BLE service linked to vitals context");
}

/**
 * @brief Set the datetime synchronization callback
 */
void juxta_ble_set_datetime_sync_callback(void (*callback)(void))
{
    datetime_sync_callback = callback;
    LOG_INF("⏰ Datetime synchronization callback set");
}

/**
 * @brief Validate timestamp for reasonable range
 *
 * @param timestamp Unix timestamp to validate
 * @return true if timestamp is valid, false otherwise
 */
static bool validate_timestamp(uint32_t timestamp)
{
    /* Check for reasonable range: 2020-01-01 to 2030-12-31 */
    const uint32_t MIN_TIMESTAMP = 1577836800; /* 2020-01-01 00:00:00 UTC */
    const uint32_t MAX_TIMESTAMP = 1924992000; /* 2030-12-31 23:59:59 UTC */

    if (timestamp < MIN_TIMESTAMP || timestamp > MAX_TIMESTAMP)
    {
        LOG_WRN("⏰ Timestamp out of valid range: %u (expected %u-%u)",
                timestamp, MIN_TIMESTAMP, MAX_TIMESTAMP);
        return false;
    }

    return true;
}

/**
 * @brief Handle timestamp synchronization
 *
 * @param timestamp Unix timestamp to set
 * @return 0 on success, negative error code on failure
 */
static int handle_timestamp_synchronization(uint32_t timestamp)
{
    if (!vitals_ctx)
    {
        LOG_ERR("⏰ Vitals context not available for timestamp synchronization");
        return -ENODEV;
    }

    if (!validate_timestamp(timestamp))
    {
        LOG_ERR("⏰ Invalid timestamp: %u", timestamp);
        return -EINVAL;
    }

    /* Get current timestamp for comparison */
    uint32_t current_timestamp = juxta_vitals_get_timestamp(vitals_ctx);

    /* Set the new timestamp */
    int ret = juxta_vitals_set_timestamp(vitals_ctx, timestamp);
    if (ret < 0)
    {
        LOG_ERR("⏰ Failed to set timestamp: %d", ret);
        return ret;
    }

    /* Log the timestamp change */
    if (current_timestamp > 0)
    {
        LOG_INF("⏰ Timestamp synchronized: %u → %u (delta: %d seconds)",
                current_timestamp, timestamp, (int)(timestamp - current_timestamp));
    }
    else
    {
        LOG_INF("⏰ Timestamp set to: %u", timestamp);
    }

    /* Mark datetime as synchronized for production flow */
    LOG_INF("✅ Datetime synchronization completed");

    /* Call datetime synchronization callback if set */
    if (datetime_sync_callback)
    {
        datetime_sync_callback();
    }

    return 0;
}

/**
 * @brief Handle memory clearing (file system format)
 *
 * @return 0 on success, negative error code on failure
 */
static int handle_memory_clearing(void)
{
    if (!framfs_ctx || !framfs_ctx->initialized)
    {
        LOG_ERR("🧹 Framfs not available for memory clearing");
        return -ENODEV;
    }

    LOG_INF("🧹 Starting memory clearing operation...");

    /* Check battery before FRAM operations */
    if (!should_allow_fram_write())
    {
        LOG_WRN("⚠️ Skipping memory clearing due to low battery");
        return -EAGAIN;
    }

    /* Format the file system */
    int ret = juxta_framfs_format(framfs_ctx);
    if (ret < 0)
    {
        LOG_ERR("🧹 Failed to format file system: %d", ret);
        return ret;
    }

    /* Clear MAC address table */
    ret = juxta_framfs_mac_clear(framfs_ctx);
    if (ret < 0)
    {
        LOG_ERR("🧹 Failed to clear MAC table: %d", ret);
        return ret;
    }

    /* Clear user settings (reset to defaults) */
    ret = juxta_framfs_clear_user_settings(framfs_ctx);
    if (ret < 0)
    {
        LOG_ERR("🧹 Failed to clear user settings: %d", ret);
        return ret;
    }

    LOG_INF("✅ Memory clearing completed successfully");
    return 0;
}

/**
 * @brief Generate Node characteristic JSON response
 */
static int generate_node_response(char *buffer, size_t buffer_size)
{
    char device_id[16];
    char upload_path[32];
    uint8_t battery_level = 0;
    const char *alert = ""; /* TODO: Phase 3 - Implement alert system */

    /* Get device ID */
    if (juxta_ble_get_device_id(device_id) < 0)
    {
        strcpy(device_id, "JX_ERROR");
    }

    /* Get upload path from framfs user settings */
    if (framfs_ctx && framfs_ctx->initialized)
    {
        if (juxta_framfs_get_upload_path(framfs_ctx, upload_path) == 0)
        {
            LOG_DBG("📁 Using upload path from framfs: %s", upload_path);
        }
        else
        {
            strcpy(upload_path, "/TEST");
            LOG_WRN("📁 Failed to get upload path from framfs, using default");
        }
    }
    else
    {
        strcpy(upload_path, "/TEST");
        LOG_WRN("📁 Framfs not available, using default upload path");
    }

    /* Get battery level from vitals library */
    if (vitals_ctx && vitals_ctx->initialized)
    {
        /* Force a vitals update to get fresh battery reading */
        (void)juxta_vitals_update(vitals_ctx);

        /* Get validated battery level (0-100) */
        int ret = juxta_vitals_get_validated_battery_level(vitals_ctx, &battery_level);
        if (ret == 0)
        {
            LOG_DBG("📊 Battery level: %d%%", battery_level);
        }
        else
        {
            LOG_WRN("📊 Failed to get battery level: %d, using 0", ret);
            battery_level = 0; /* Default to 0 if read fails */
        }
    }
    else
    {
        LOG_WRN("📊 Vitals context not available, battery level = 0");
        battery_level = 0;
    }

    /* Get operating mode from framfs user settings */
    uint8_t operating_mode = 0x00;
    if (framfs_ctx && framfs_ctx->initialized)
    {
        if (juxta_framfs_get_operating_mode(framfs_ctx, &operating_mode) != 0)
        {
            LOG_WRN("🎛️ Failed to get operating mode from framfs, using default");
            operating_mode = 0x00;
        }
    }

    /* Generate JSON response */
    int written = snprintf(buffer, buffer_size,
                           "{\"upload_path\":\"%s\",\"firmware_version\":\"%s\",\"battery_level\":%d,\"device_id\":\"%s\",\"operating_mode\":%d,\"alert\":\"%s\"}",
                           upload_path, JUXTA_FIRMWARE_VERSION, battery_level, device_id, operating_mode, alert);

    if (written >= buffer_size)
    {
        LOG_ERR("📊 Node response too large (%d >= %zu)", written, buffer_size);
        return -1;
    }

    LOG_DBG("📊 Generated node response: %s", buffer);
    return written;
}

/**
 * @brief Node characteristic read callback
 * Returns device status and configuration information in JSON format
 */
static ssize_t read_node_char(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len, uint16_t offset)
{
    LOG_DBG("📊 Node characteristic read request");

    /* Generate the JSON response */
    int response_len = generate_node_response(node_response, sizeof(node_response));
    if (response_len < 0)
    {
        LOG_ERR("📊 Failed to generate node response");
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    if (offset >= response_len)
    {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    size_t copy_len = MIN(len, response_len - offset);
    memcpy(buf, node_response + offset, copy_len);

    LOG_INF("📊 Node characteristic read, returned %zu bytes", copy_len);
    return copy_len;
}

/**
 * @brief Parse JSON command from gateway characteristic
 * Expected format: {"timestamp":1234567890,"sendFilenames":true,"clearMemory":true,"operatingMode":0,"advInterval":5,"scanInterval":15,"subjectId":"vole001","uploadPath":"/TEST"}
 */
static int parse_gateway_command(const char *json_cmd, struct juxta_framfs_user_settings *settings)
{
    if (!json_cmd || !settings)
    {
        LOG_ERR("🎛️ Invalid parameters for gateway command parsing");
        return -EINVAL;
    }

    if (strlen(json_cmd) == 0)
    {
        LOG_ERR("🎛️ Empty gateway command received");
        return -EINVAL;
    }

    LOG_DBG("🎛️ Parsing gateway command: %s", json_cmd);

    /* Initialize settings with current values */
    if (framfs_ctx && framfs_ctx->initialized)
    {
        if (juxta_framfs_get_user_settings(framfs_ctx, settings) != 0)
        {
            LOG_WRN("🎛️ Failed to get current settings, using defaults");
            /* Use defaults if framfs not available */
            settings->operating_mode = 0x00;
            settings->adv_interval = 5;
            settings->scan_interval = 15;
            strcpy(settings->subject_id, "");
            strcpy(settings->upload_path, "/TEST");
        }
    }
    else
    {
        /* Use defaults if framfs not available */
        settings->operating_mode = 0x00;
        settings->adv_interval = 5;
        settings->scan_interval = 15;
        strcpy(settings->subject_id, "");
        strcpy(settings->upload_path, "/TEST");
    }

    /* Simple JSON parsing - look for key-value pairs */
    const char *p = json_cmd;
    bool settings_changed = false;
    bool send_filenames_requested = false;
    bool clear_memory_requested = false;

    /* Look for timestamp */
    p = strstr(json_cmd, "\"timestamp\":");
    if (p)
    {
        uint32_t timestamp;
        if (sscanf(p, "\"timestamp\":%u", &timestamp) == 1)
        {
            LOG_INF("🎛️ Timestamp command: %u", timestamp);
            int ret = handle_timestamp_synchronization(timestamp);
            if (ret < 0)
            {
                LOG_ERR("❌ Timestamp synchronization failed: %d", ret);
                return ret;
            }
        }
    }

    /* Look for sendFilenames */
    p = strstr(json_cmd, "\"sendFilenames\":");
    if (p)
    {
        if (strstr(p, "\"sendFilenames\":true") || strstr(p, "\"sendFilenames\": true"))
        {
            send_filenames_requested = true;
            LOG_INF("🎛️ Send filenames command received");
            /* Trigger file listing via filename characteristic */
            if (current_conn)
            {
                char file_listing[JUXTA_NODE_RESPONSE_MAX_SIZE];
                int listing_len = generate_file_listing(file_listing, sizeof(file_listing));

                if (listing_len > 0)
                {
                    /* Send file listing via indication */
                    if (filename_char_attr && current_conn)
                    {
                        send_indication(current_conn, filename_char_attr, file_listing, listing_len);
                    }
                }
                else
                {
                    /* Send error response */
                    if (filename_char_attr && current_conn)
                    {
                        const char *error = "NFF";
                        send_indication(current_conn, filename_char_attr, error, strlen(error));
                    }
                }
            }
            else
            {
                LOG_WRN("🎛️ No active connection for file listing");
            }
        }
    }

    /* Look for clearMemory */
    p = strstr(json_cmd, "\"clearMemory\":");
    if (p)
    {
        if (strstr(p, "\"clearMemory\":true") || strstr(p, "\"clearMemory\": true"))
        {
            clear_memory_requested = true;
            LOG_INF("🎛️ Clear memory command received");
        }
    }

    /* Check for command conflicts */
    if (send_filenames_requested && clear_memory_requested)
    {
        LOG_ERR("❌ Command conflict: sendFilenames and clearMemory cannot be used together");
        return -EINVAL;
    }

    /* Execute clearMemory if requested */
    if (clear_memory_requested)
    {
        int ret = handle_memory_clearing();
        if (ret < 0)
        {
            LOG_ERR("❌ Memory clearing failed: %d", ret);
            return ret;
        }
    }

    /* Look for operatingMode */
    p = strstr(json_cmd, "\"operatingMode\":");
    if (p)
    {
        uint8_t operating_mode;
        if (sscanf(p, "\"operatingMode\":%hhu", &operating_mode) == 1)
        {
            LOG_INF("🎛️ Operating mode command: %d", operating_mode);
            settings->operating_mode = operating_mode;
            settings_changed = true;
        }
        else
        {
            LOG_WRN("🎛️ Invalid operatingMode format in command");
        }
    }

    /* Look for advInterval */
    p = strstr(json_cmd, "\"advInterval\":");
    if (p)
    {
        uint8_t adv_interval;
        if (sscanf(p, "\"advInterval\":%hhu", &adv_interval) == 1)
        {
            LOG_INF("🎛️ Advertising interval command: %d", adv_interval);
            settings->adv_interval = adv_interval;
            settings_changed = true;
        }
        else
        {
            LOG_WRN("🎛️ Invalid advInterval format in command");
        }
    }

    /* Look for scanInterval */
    p = strstr(json_cmd, "\"scanInterval\":");
    if (p)
    {
        uint8_t scan_interval;
        if (sscanf(p, "\"scanInterval\":%hhu", &scan_interval) == 1)
        {
            LOG_INF("🎛️ Scanning interval command: %d", scan_interval);
            settings->scan_interval = scan_interval;
            settings_changed = true;
        }
    }

    /* Look for subjectId */
    p = strstr(json_cmd, "\"subjectId\":");
    if (p)
    {
        char subject_id[JUXTA_FRAMFS_SUBJECT_ID_LEN];
        if (sscanf(p, "\"subjectId\":\"%[^\"]\"", subject_id) == 1)
        {
            LOG_INF("🎛️ Subject ID command: %s", subject_id);
            strncpy(settings->subject_id, subject_id, JUXTA_FRAMFS_SUBJECT_ID_LEN - 1);
            settings->subject_id[JUXTA_FRAMFS_SUBJECT_ID_LEN - 1] = '\0';
            settings_changed = true;
        }
    }

    /* Look for uploadPath */
    p = strstr(json_cmd, "\"uploadPath\":");
    if (p)
    {
        char upload_path[JUXTA_FRAMFS_UPLOAD_PATH_LEN];
        if (sscanf(p, "\"uploadPath\":\"%[^\"]\"", upload_path) == 1)
        {
            LOG_INF("🎛️ Upload path command: %s", upload_path);
            strncpy(settings->upload_path, upload_path, JUXTA_FRAMFS_UPLOAD_PATH_LEN - 1);
            settings->upload_path[JUXTA_FRAMFS_UPLOAD_PATH_LEN - 1] = '\0';
            settings_changed = true;
        }
    }

    if (settings_changed)
    {
        LOG_INF("🎛️ Settings updated - saving to framfs");
        if (framfs_ctx && framfs_ctx->initialized)
        {
            if (!should_allow_fram_write())
            {
                LOG_WRN("⚠️ Skipping settings save due to low battery");
                return 0;
            }

            if (juxta_framfs_set_user_settings(framfs_ctx, settings) == 0)
            {
                LOG_INF("✅ Settings saved successfully");

                /* Trigger timing update in main.c */
                juxta_ble_timing_update_trigger();

                return 0;
            }
            else
            {
                LOG_ERR("❌ Failed to save settings to framfs");
                return -1;
            }
        }
        else
        {
            LOG_WRN("⚠️ Framfs not available, settings not persisted");
            return 0;
        }
    }

    return 0;
}

/**
 * @brief Gateway characteristic write callback
 * Accepts JSON commands to control device behavior
 */
static ssize_t write_gateway_char(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  const void *buf, uint16_t len, uint16_t offset,
                                  uint8_t flags)
{
    LOG_DBG("🎛️ Gateway characteristic write request, len=%d", len);

    if (offset + len > sizeof(gateway_command))
    {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(gateway_command + offset, buf, len);
    gateway_command[offset + len] = '\0';

    LOG_INF("🎛️ Gateway command received: %s", gateway_command);

    /* Parse and execute the command */
    struct juxta_framfs_user_settings new_settings;
    int ret = parse_gateway_command(gateway_command, &new_settings);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to parse gateway command");
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    /* TODO: Phase 4 - Trigger main.c timing updates */
    /* For now, the settings are saved to framfs and will be read on next restart */

    return len;
}

/**
 * @brief Enhanced error handling for file operations
 */
static int handle_file_error(int error_code, const char *operation, const char *filename)
{
    switch (error_code)
    {
    case JUXTA_FRAMFS_ERROR_NOT_FOUND:
        LOG_ERR("📁 File not found: %s (%s)", filename, operation);
        return -ENOENT;
    case JUXTA_FRAMFS_ERROR_INVALID:
        LOG_ERR("📁 Invalid file operation: %s (%s)", filename, operation);
        return -EINVAL;
    case JUXTA_FRAMFS_ERROR_FULL:
        LOG_ERR("📁 File system full: %s (%s)", filename, operation);
        return -ENOSPC;
    default:
        LOG_ERR("📁 File operation failed: %s (%s) - error %d", filename, operation, error_code);
        return -EIO;
    }
}

/**
 * @brief Generate file listing response with enhanced error handling
 * Format: "filename1.txt|1234;filename2.csv|5678;EOF"
 */
static int generate_file_listing(char *buffer, size_t buffer_size)
{
    if (!framfs_ctx || !framfs_ctx->initialized)
    {
        LOG_ERR("📁 Framfs not available for file listing");
        return -1;
    }

    char filenames[JUXTA_FRAMFS_MAX_FILES][JUXTA_FRAMFS_FILENAME_LEN];
    int file_count = juxta_framfs_list_files(framfs_ctx, filenames, JUXTA_FRAMFS_MAX_FILES);

    if (file_count < 0)
    {
        LOG_ERR("📁 Failed to list files: %d", file_count);
        return -1;
    }

    LOG_INF("📁 Generating file listing for %d files", file_count);

    int written = 0;
    for (int i = 0; i < file_count; i++)
    {
        struct juxta_framfs_entry entry;
        int ret = juxta_framfs_get_file_info(framfs_ctx, filenames[i], &entry);
        if (ret == 0)
        {
            int len = snprintf(buffer + written, buffer_size - written,
                               "%s|%d;", filenames[i], entry.length);
            if (len >= 0 && written + len < buffer_size)
            {
                written += len;
            }
            else
            {
                LOG_WRN("📁 File listing buffer full, truncating at %d files", i);
                break;
            }
        }
        else
        {
            handle_file_error(ret, "get_file_info", filenames[i]);
        }
    }

    /* Add EOF marker */
    int eof_len = snprintf(buffer + written, buffer_size - written, "EOF");
    if (eof_len >= 0 && written + eof_len < buffer_size)
    {
        written += eof_len;
    }

    /* Add MAC table as special "file" if it has data */
    uint32_t mac_table_size;
    if (juxta_framfs_get_mac_table_data_size(framfs_ctx, &mac_table_size) == 0 && mac_table_size > 0)
    {
        int mac_len = snprintf(buffer + written, buffer_size - written,
                               ";MACIDX|%u", mac_table_size);
        if (mac_len >= 0 && written + mac_len < buffer_size)
        {
            written += mac_len;
            LOG_DBG("📁 Added MAC table to listing: MACIDX|%u", mac_table_size);
        }
        else
        {
            LOG_WRN("📁 MAC table listing truncated due to buffer size");
        }
    }

    LOG_INF("📁 Generated file listing (%d bytes): %s", written, buffer);
    return written;
}

/**
 * @brief Start file transfer for requested filename with enhanced error handling
 */
static int start_file_transfer(const char *filename)
{
    if (!framfs_ctx || !framfs_ctx->initialized)
    {
        LOG_ERR("📁 Framfs not available for file transfer");
        return -1;
    }

    if (!filename || strlen(filename) == 0)
    {
        LOG_ERR("📁 Invalid filename for file transfer");
        return -EINVAL;
    }

    /* Check if this is a MAC table request */
    if (strcmp(filename, "MACIDX") == 0)
    {
        /* Handle MAC table transfer */
        uint32_t mac_table_size;
        int ret = juxta_framfs_get_mac_table_data_size(framfs_ctx, &mac_table_size);
        if (ret < 0)
        {
            LOG_ERR("📁 Failed to get MAC table size: %d", ret);
            return ret;
        }

        if (mac_table_size == 0)
        {
            LOG_WRN("📁 MAC table is empty");
            return -ENOENT;
        }

        /* Initialize transfer state for MAC table */
        strncpy(current_transfer_filename, "MACIDX", JUXTA_FRAMFS_FILENAME_LEN - 1);
        current_transfer_filename[JUXTA_FRAMFS_FILENAME_LEN - 1] = '\0';
        current_transfer_offset = 0;
        current_transfer_file_size = mac_table_size;
        file_transfer_active = true;

        LOG_INF("📁 Started MAC table transfer: %d bytes, MTU: %d",
                mac_table_size, current_mtu);
        return 0;
    }

    /* Get file info for regular files */
    struct juxta_framfs_entry entry;
    int ret = juxta_framfs_get_file_info(framfs_ctx, filename, &entry);
    if (ret != 0)
    {
        return handle_file_error(ret, "get_file_info", filename);
    }

    /* Validate file size */
    if (entry.length <= 0)
    {
        LOG_ERR("📁 Invalid file size: %d bytes", entry.length);
        return -EINVAL;
    }

    /* Initialize transfer state */
    strncpy(current_transfer_filename, filename, JUXTA_FRAMFS_FILENAME_LEN - 1);
    current_transfer_filename[JUXTA_FRAMFS_FILENAME_LEN - 1] = '\0';
    current_transfer_offset = 0;
    current_transfer_file_size = entry.length;
    file_transfer_active = true;

    LOG_INF("📁 Started file transfer: %s (%d bytes, MTU: %d)",
            filename, entry.length, current_mtu);
    return 0;
}

/**
 * @brief Get next chunk of file data for transfer with MTU optimization
 */
static int get_file_transfer_chunk(uint8_t *buffer, size_t buffer_size, size_t *bytes_read)
{
    if (!file_transfer_active || !framfs_ctx || !framfs_ctx->initialized)
    {
        return -1;
    }

    /* Calculate optimal chunk size based on MTU */
    size_t max_chunk_size = current_mtu - 3; /* Account for ATT header */
    size_t remaining = current_transfer_file_size - current_transfer_offset;
    size_t chunk_size = MIN(MIN(buffer_size, max_chunk_size), remaining);

    if (chunk_size == 0)
    {
        *bytes_read = 0;
        return 0; /* Transfer complete */
    }

    /* Check if this is MAC table transfer */
    if (strcmp(current_transfer_filename, "MACIDX") == 0)
    {
        /* Read MAC table chunk */
        int ret = juxta_framfs_read_mac_table_data(framfs_ctx, current_transfer_offset,
                                                   buffer, chunk_size);
        if (ret < 0)
        {
            LOG_ERR("📁 Failed to read MAC table chunk: %d", ret);
            return ret;
        }

        current_transfer_offset += ret;
        *bytes_read = ret;

        LOG_DBG("📁 MAC table chunk: offset=%u/%d, bytes=%zu, progress=%.1f%%",
                current_transfer_offset, current_transfer_file_size, *bytes_read,
                (double)current_transfer_offset / current_transfer_file_size * 100.0);
        return 0;
    }

    /* Read file chunk for regular files */
    int ret = juxta_framfs_read(framfs_ctx, current_transfer_filename,
                                current_transfer_offset, buffer, chunk_size);
    if (ret < 0)
    {
        return handle_file_error(ret, "read_file_chunk", current_transfer_filename);
    }

    current_transfer_offset += ret;
    *bytes_read = ret;

    LOG_DBG("📁 File transfer chunk: offset=%u/%d, bytes=%zu, progress=%.1f%%",
            current_transfer_offset, current_transfer_file_size, *bytes_read,
            (double)current_transfer_offset / current_transfer_file_size * 100.0);
    return 0;
}

/**
 * @brief End current file transfer
 */
static void end_file_transfer(void)
{
    file_transfer_active = false;
    current_transfer_offset = 0;
    current_transfer_file_size = -1;
    memset(current_transfer_filename, 0, sizeof(current_transfer_filename));
    file_transfer_state = FILE_TRANSFER_STATE_IDLE;
    LOG_INF("📁 File transfer ended");
}

/**
 * @brief Send indication to connected client
 */
static int send_indication(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           const void *data, uint16_t len)
{
    if (!conn)
    {
        LOG_ERR("📤 No connection available for indication");
        return -ENOTCONN;
    }

    if (!attr || !data || len == 0)
    {
        LOG_ERR("📤 Invalid params for indication");
        return -EINVAL;
    }

    if (indication_pending)
    {
        LOG_WRN("📤 Indication already pending");
        return -EBUSY;
    }

    struct bt_gatt_indicate_params *params = NULL;
    if (attr == filename_char_attr)
    {
        params = &filename_ind_params;
        params->func = filename_indication_confirmed;
    }
    else if (attr == file_transfer_char_attr)
    {
        params = &file_transfer_ind_params;
        params->func = file_transfer_indication_confirmed;
    }
    else
    {
        /* Default to file transfer callback if unknown */
        params = &file_transfer_ind_params;
        params->func = file_transfer_indication_confirmed;
    }

    params->attr = attr;
    params->data = data;
    params->len = len;

    indication_pending = true;
    indication_timeout = k_uptime_get_32() + 5000; /* 5s */

    int ret = bt_gatt_indicate(conn, params);
    if (ret < 0)
    {
        indication_pending = false;
        LOG_ERR("📤 Failed to send indication: %d", ret);
        return ret;
    }

    LOG_DBG("📤 Indication sent: %u bytes", len);
    return 0;
}

/**
 * @brief Filename indication confirmation callback
 */
static void filename_indication_confirmed(struct bt_conn *conn, struct bt_gatt_indicate_params *params, uint8_t err)
{
    indication_pending = false;

    if (err)
    {
        LOG_ERR("📤 Filename indication failed: %d", err);
        file_transfer_state = FILE_TRANSFER_STATE_ERROR;
    }
    else
    {
        LOG_DBG("📤 Filename indication confirmed");
        if (file_transfer_state == FILE_TRANSFER_STATE_LISTING_REQUESTED)
        {
            file_transfer_state = FILE_TRANSFER_STATE_IDLE;
        }
    }
}

/**
 * @brief Continue file transfer with next chunk
 */
static void continue_file_transfer(void)
{
    if (!current_conn || !file_transfer_char_attr || file_transfer_state != FILE_TRANSFER_STATE_TRANSFERRING)
    {
        return;
    }

    /* Get next chunk */
    size_t bytes_read = 0;
    int ret = get_file_transfer_chunk(file_transfer_chunk, sizeof(file_transfer_chunk), &bytes_read);
    if (ret == 0 && bytes_read > 0)
    {
        /* Send next chunk */
        send_indication(current_conn, file_transfer_char_attr, file_transfer_chunk, bytes_read);
    }
    else
    {
        /* Transfer complete - send EOF */
        const char *eof = "EOF";
        send_indication(current_conn, file_transfer_char_attr, eof, strlen(eof));
        file_transfer_state = FILE_TRANSFER_STATE_COMPLETE;
        end_file_transfer();
    }
}

/**
 * @brief File transfer indication confirmation callback
 */
static void file_transfer_indication_confirmed(struct bt_conn *conn, struct bt_gatt_indicate_params *params, uint8_t err)
{
    indication_pending = false;

    if (err)
    {
        LOG_ERR("📤 File transfer indication failed: %d", err);
        file_transfer_state = FILE_TRANSFER_STATE_ERROR;
    }
    else
    {
        LOG_DBG("📤 File transfer indication confirmed");
        if (file_transfer_state == FILE_TRANSFER_STATE_TRANSFERRING)
        {
            /* Continue with next chunk or complete transfer */
            if (current_transfer_offset >= current_transfer_file_size)
            {
                file_transfer_state = FILE_TRANSFER_STATE_COMPLETE;
                end_file_transfer();
            }
            else
            {
                /* Continue with next chunk */
                continue_file_transfer();
            }
        }
    }
}

/**
 * @brief Filename characteristic read callback
 */
static ssize_t read_filename_char(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  void *buf, uint16_t len, uint16_t offset)
{
    LOG_DBG("📁 Filename characteristic read request");

    /* Return current filename or empty string */
    const char *response = current_transfer_filename;
    size_t response_len = strlen(response);

    if (offset >= response_len)
    {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    size_t copy_len = MIN(len, response_len - offset);
    memcpy(buf, response + offset, copy_len);

    return copy_len;
}

/**
 * @brief Filename characteristic write callback
 */
static ssize_t write_filename_char(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   const void *buf, uint16_t len, uint16_t offset,
                                   uint8_t flags)
{
    LOG_DBG("📁 Filename characteristic write request, len=%d", len);

    if (offset + len > sizeof(filename_request))
    {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(filename_request + offset, buf, len);
    filename_request[offset + len] = '\0';

    LOG_INF("📁 Filename request received: %s", filename_request);

    /* Process filename request */
    if (strcmp(filename_request, "LIST") == 0)
    {
        /* File listing request */
        file_transfer_state = FILE_TRANSFER_STATE_LISTING_REQUESTED;

        char file_listing[JUXTA_NODE_RESPONSE_MAX_SIZE];
        int listing_len = generate_file_listing(file_listing, sizeof(file_listing));

        if (listing_len > 0)
        {
            /* Send file listing via indication */
            if (filename_char_attr && conn)
            {
                send_indication(conn, filename_char_attr, file_listing, listing_len);
            }
        }
        else
        {
            /* Send error response */
            if (filename_char_attr && conn)
            {
                const char *error = "NFF";
                send_indication(conn, filename_char_attr, error, strlen(error));
            }
        }
    }
    else
    {
        /* File transfer request */
        file_transfer_state = FILE_TRANSFER_STATE_TRANSFER_REQUESTED;

        int ret = start_file_transfer(filename_request);
        if (ret == 0)
        {
            /* Start file transfer immediately - no filename confirmation */
            file_transfer_state = FILE_TRANSFER_STATE_TRANSFERRING;

            /* Send first chunk via file transfer characteristic */
            if (file_transfer_char_attr && conn)
            {
                size_t bytes_read = 0;
                ret = get_file_transfer_chunk(file_transfer_chunk, sizeof(file_transfer_chunk), &bytes_read);
                if (ret == 0 && bytes_read > 0)
                {
                    send_indication(conn, file_transfer_char_attr, file_transfer_chunk, bytes_read);
                }
                else
                {
                    /* File is empty or error - send EOF */
                    const char *eof = "EOF";
                    send_indication(conn, file_transfer_char_attr, eof, strlen(eof));
                    file_transfer_state = FILE_TRANSFER_STATE_COMPLETE;
                    end_file_transfer();
                }
            }
        }
        else
        {
            /* Send error response via filename characteristic */
            if (filename_char_attr && conn)
            {
                const char *error = "NFF";
                send_indication(conn, filename_char_attr, error, strlen(error));
            }
            file_transfer_state = FILE_TRANSFER_STATE_ERROR;
        }
    }

    return len;
}

/**
 * @brief File transfer characteristic read callback
 */
static ssize_t read_file_transfer_char(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                       void *buf, uint16_t len, uint16_t offset)
{
    LOG_DBG("📤 File transfer characteristic read request");

    if (!file_transfer_active)
    {
        /* No active transfer */
        const char *response = "";
        size_t response_len = strlen(response);

        if (offset >= response_len)
        {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
        }

        size_t copy_len = MIN(len, response_len - offset);
        memcpy(buf, response + offset, copy_len);
        return copy_len;
    }

    /* Get file transfer chunk */
    size_t bytes_read = 0;
    int ret = get_file_transfer_chunk(buf, len, &bytes_read);
    if (ret < 0)
    {
        LOG_ERR("📤 Failed to get file transfer chunk: %d", ret);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    /* Check if transfer is complete */
    if (bytes_read == 0)
    {
        /* Transfer complete, send EOF */
        const char *eof = "EOF";
        size_t eof_len = MIN(len, strlen(eof));
        memcpy(buf, eof, eof_len);
        end_file_transfer();
        return eof_len;
    }

    return bytes_read;
}

/**
 * @brief CCC changed callback for filename characteristic
 */
static void filename_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY) || (value == BT_GATT_CCC_INDICATE);
    LOG_INF("📱 BLE: Filename CCC changed, notifications %s", notif_enabled ? "enabled" : "disabled");
}

/**
 * @brief CCC changed callback for file transfer characteristic
 */
static void file_transfer_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY) || (value == BT_GATT_CCC_INDICATE);
    LOG_INF("📤 File transfer CCC changed, notifications %s", notif_enabled ? "enabled" : "disabled");
}

/**
 * @brief Connection established callback
 */
void juxta_ble_connection_established(struct bt_conn *conn)
{
    current_conn = conn;
    LOG_INF("🔗 BLE connection established for file transfer");

    // SoftDevice controller does not support GATT client MTU exchange.
    // The peer (e.g., mobile app) must initiate MTU exchange for larger MTU.
    current_mtu = bt_gatt_get_mtu(conn); // Will be 23 unless peer requests larger
    mtu_negotiated = (current_mtu > 23);
    LOG_INF("📏 Current MTU: %d bytes (negotiated=%s)", current_mtu, mtu_negotiated ? "yes" : "no");

    // Reset file transfer state
    file_transfer_state = FILE_TRANSFER_STATE_IDLE;
    indication_pending = false;
}

/**
 * @brief Connection terminated callback
 */
void juxta_ble_connection_terminated(void)
{
    current_conn = NULL;
    mtu_negotiated = false;
    current_mtu = 23;
    indication_pending = false;
    end_file_transfer(); /* Clean up any active transfer */
    LOG_INF("🔌 BLE connection terminated, file transfer cleaned up");
}

/* JUXTA Hublink BLE Service Definition */
BT_GATT_SERVICE_DEFINE(juxta_hublink_svc,
                       /* Service Declaration */
                       BT_GATT_PRIMARY_SERVICE(BT_UUID_JUXTA_HUBLINK_SERVICE),

                       /* Node Characteristic (READ) */
                       BT_GATT_CHARACTERISTIC(BT_UUID_JUXTA_NODE_CHAR,
                                              BT_GATT_CHRC_READ,
                                              BT_GATT_PERM_READ,
                                              read_node_char, NULL, NULL),

                       /* Node Characteristic User Description */
                       BT_GATT_CUD("Node Status", BT_GATT_PERM_READ),

                       /* Gateway Characteristic (WRITE) */
                       BT_GATT_CHARACTERISTIC(BT_UUID_JUXTA_GATEWAY_CHAR,
                                              BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                                              BT_GATT_PERM_WRITE,
                                              NULL, write_gateway_char, NULL),

                       /* Gateway Characteristic User Description */
                       BT_GATT_CUD("Gateway Commands", BT_GATT_PERM_READ),

                       /* Filename Characteristic (READ/WRITE/INDICATE) */
                       BT_GATT_CHARACTERISTIC(BT_UUID_JUXTA_FILENAME_CHAR,
                                              BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_INDICATE,
                                              BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                                              read_filename_char, write_filename_char, NULL),

                       /* Filename Characteristic CCC */
                       BT_GATT_CCC(filename_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

                       /* Filename Characteristic User Description */
                       BT_GATT_CUD("Filename Operations", BT_GATT_PERM_READ),

                       /* File Transfer Characteristic (READ/INDICATE) */
                       BT_GATT_CHARACTERISTIC(BT_UUID_JUXTA_FILE_TRANSFER_CHAR,
                                              BT_GATT_CHRC_READ | BT_GATT_CHRC_INDICATE,
                                              BT_GATT_PERM_READ,
                                              read_file_transfer_char, NULL, NULL),

                       /* File Transfer Characteristic CCC */
                       BT_GATT_CCC(file_transfer_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

                       /* File Transfer Characteristic User Description */
                       BT_GATT_CUD("File Transfer", BT_GATT_PERM_READ), );

/**
 * @brief Initialize the JUXTA Hublink BLE service
 */
int juxta_ble_service_init(void)
{
    LOG_INF("🔵 JUXTA Hublink BLE Service initialized");
    LOG_INF("📋 Service: 57617368-5501-0001-8000-00805f9b34fb");
    LOG_INF("📊 Node: 57617368-5505-0001-8000-00805f9b34fb");
    LOG_INF("🎛️ Gateway: 57617368-5504-0001-8000-00805f9b34fb");
    LOG_INF("📁 Filename: 57617368-5502-0001-8000-00805f9b34fb");
    LOG_INF("📤 File Transfer: 57617368-5503-0001-8000-00805f9b34fb");
    LOG_INF("📏 MTU: %d bytes, Chunk: %d bytes, Node: %d bytes, Gateway: %d bytes",
            JUXTA_FILE_TRANSFER_CHUNK_SIZE + 3, JUXTA_FILE_TRANSFER_CHUNK_SIZE,
            JUXTA_NODE_RESPONSE_MAX_SIZE, JUXTA_GATEWAY_COMMAND_MAX_SIZE);

    /* Locate characteristic value attributes in our service */
    filename_char_attr = bt_gatt_find_by_uuid(juxta_hublink_svc.attrs,
                                              juxta_hublink_svc.attr_count,
                                              BT_UUID_JUXTA_FILENAME_CHAR);
    file_transfer_char_attr = bt_gatt_find_by_uuid(juxta_hublink_svc.attrs,
                                                   juxta_hublink_svc.attr_count,
                                                   BT_UUID_JUXTA_FILE_TRANSFER_CHAR);

    if (!filename_char_attr || !file_transfer_char_attr)
    {
        LOG_WRN("⚠️ Could not resolve characteristic attributes (indications may fail)");
    }

    return 0;
}

/**
 * @brief Get the current device ID (JX_XXXXXX format)
 */
int juxta_ble_get_device_id(char *device_id)
{
    if (!device_id)
    {
        return -1;
    }

    bt_addr_le_t addr;
    size_t count = 1;

    bt_id_get(&addr, &count);
    if (count > 0)
    {
        snprintf(device_id, 10, "JX_%02X%02X%02X",
                 addr.a.val[3], addr.a.val[2], addr.a.val[1]);
        return 0;
    }

    strcpy(device_id, "JX_ERROR");
    return -1;
}

/**
 * @brief Get current service status for debugging
 */
int juxta_ble_get_status(uint16_t *mtu, bool *connected, bool *transfer_active)
{
    if (!mtu || !connected || !transfer_active)
    {
        return -EINVAL;
    }

    *mtu = current_mtu;
    *connected = (current_conn != NULL);
    *transfer_active = file_transfer_active;

    LOG_DBG("📊 Service status: MTU=%d, Connected=%s, Transfer=%s, State=%d, Indication=%s",
            *mtu, *connected ? "yes" : "no", *transfer_active ? "active" : "idle",
            file_transfer_state, indication_pending ? "pending" : "idle");
    return 0;
}

/**
 * @brief Test function for timestamp synchronization and clearMemory functionality
 * This function can be called during development to verify the implementation
 */
int juxta_ble_test_gateway_commands(void)
{
    LOG_INF("🧪 Testing gateway command functionality...");

    /* Test 1: Timestamp synchronization */
    LOG_INF("Test 1: Timestamp synchronization");
    uint32_t test_timestamp = 1705752000; /* 2024-01-20 12:00:00 UTC */
    int ret = handle_timestamp_synchronization(test_timestamp);
    if (ret < 0)
    {
        LOG_ERR("❌ Timestamp synchronization test failed: %d", ret);
        return ret;
    }
    LOG_INF("✅ Timestamp synchronization test passed");

    /* Test 2: Invalid timestamp validation */
    LOG_INF("Test 2: Invalid timestamp validation");
    uint32_t invalid_timestamp = 1000000000; /* Too old */
    ret = handle_timestamp_synchronization(invalid_timestamp);
    if (ret >= 0)
    {
        LOG_ERR("❌ Invalid timestamp validation test failed - should have rejected timestamp");
        return -1;
    }
    LOG_INF("✅ Invalid timestamp validation test passed");

    /* Test 3: ClearMemory functionality (only if framfs is available) */
    if (framfs_ctx && framfs_ctx->initialized)
    {
        LOG_INF("Test 3: ClearMemory functionality");
        ret = handle_memory_clearing();
        if (ret < 0)
        {
            LOG_ERR("❌ ClearMemory test failed: %d", ret);
            return ret;
        }
        LOG_INF("✅ ClearMemory test passed");
    }
    else
    {
        LOG_WRN("⚠️ Skipping ClearMemory test - framfs not available");
    }

    LOG_INF("✅ All gateway command tests passed");
    return 0;
}