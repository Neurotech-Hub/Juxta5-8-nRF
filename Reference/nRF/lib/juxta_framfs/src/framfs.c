/*
 * JUXTA FRAM File System Library Implementation
 *
 * Copyright (c) 2024 NeurotechHub
 * SPDX-License-Identifier: Apache-2.0
 */

#include <juxta_framfs/framfs.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(juxta_framfs, CONFIG_JUXTA_FRAMFS_LOG_LEVEL);

/* Internal helper functions */
static int framfs_read_header(struct juxta_framfs_context *ctx);
static int framfs_write_header(struct juxta_framfs_context *ctx);
static int framfs_read_entry(struct juxta_framfs_context *ctx, uint16_t index,
                             struct juxta_framfs_entry *entry);
static int framfs_write_entry(struct juxta_framfs_context *ctx, uint16_t index,
                              const struct juxta_framfs_entry *entry);
static int framfs_find_file(struct juxta_framfs_context *ctx, const char *filename);
static int framfs_find_active_file(struct juxta_framfs_context *ctx);
static uint32_t framfs_get_entry_addr(uint16_t index);
static uint32_t framfs_get_data_start_addr(void);

/* MAC table helper functions */
static int framfs_read_mac_header(struct juxta_framfs_context *ctx);
static int framfs_write_mac_header(struct juxta_framfs_context *ctx);
static int framfs_read_mac_entry(struct juxta_framfs_context *ctx, uint8_t index,
                                 struct juxta_framfs_mac_entry *entry);
static int framfs_write_mac_entry(struct juxta_framfs_context *ctx, uint8_t index,
                                  const struct juxta_framfs_mac_entry *entry);
static uint32_t framfs_get_mac_header_addr(void);
static uint32_t framfs_get_mac_entry_addr(uint8_t index);

/* User settings helper functions */
static int framfs_read_user_settings(struct juxta_framfs_context *ctx);
static int framfs_write_user_settings(struct juxta_framfs_context *ctx);
static uint32_t framfs_get_user_settings_addr(void);

/* ========================================================================
 * File System Management Functions
 * ======================================================================== */

int juxta_framfs_init(struct juxta_framfs_context *ctx,
                      struct juxta_fram_device *fram_dev)
{
    if (!ctx || !fram_dev)
    {
        LOG_ERR("Invalid parameters");
        return JUXTA_FRAMFS_ERROR;
    }

    if (!fram_dev->initialized)
    {
        LOG_ERR("FRAM device not initialized");
        return JUXTA_FRAMFS_ERROR_INIT;
    }

    /* Initialize context */
    memset(ctx, 0, sizeof(*ctx));
    ctx->fram_dev = fram_dev;
    ctx->active_file_index = -1;

    /* Try to read existing header */
    int ret = framfs_read_header(ctx);
    if (ret < 0)
    {
        LOG_WRN("Failed to read file system header: %d", ret);
        LOG_INF("Initializing new file system");

        /* Format new file system */
        ret = juxta_framfs_format(ctx);
        if (ret < 0)
        {
            LOG_ERR("Failed to format file system: %d", ret);
            return ret;
        }
    }
    else
    {
        /* Header read successfully, check magic number */
        if (ctx->header.magic != JUXTA_FRAMFS_MAGIC)
        {
            LOG_WRN("Invalid file system magic: 0x%04X (expected 0x%04X), formatting new filesystem",
                    ctx->header.magic, JUXTA_FRAMFS_MAGIC);
            LOG_INF("Initializing new file system");

            /* Format new file system */
            ret = juxta_framfs_format(ctx);
            if (ret < 0)
            {
                LOG_ERR("Failed to format file system: %d", ret);
                return ret;
            }
        }
    }

    /* Try to read existing MAC header */
    ret = framfs_read_mac_header(ctx);
    if (ret < 0 || ctx->mac_header.magic != JUXTA_FRAMFS_MAC_MAGIC)
    {
        LOG_WRN("MAC table header not found or invalid, initializing new MAC table");
        LOG_INF("Initializing new MAC table");

        /* Initialize MAC table */
        ret = juxta_framfs_mac_clear(ctx);
        if (ret < 0)
        {
            LOG_ERR("Failed to initialize MAC table: %d", ret);
            return ret;
        }
    }

    /* Try to read existing user settings */
    ret = framfs_read_user_settings(ctx);
    if (ret < 0 || ctx->user_settings.magic != JUXTA_FRAMFS_USER_SETTINGS_MAGIC)
    {
        LOG_WRN("User settings not found or invalid, initializing new user settings");
        LOG_INF("Initializing new user settings");

        /* Initialize user settings */
        ret = juxta_framfs_clear_user_settings(ctx);
        if (ret < 0)
        {
            LOG_ERR("Failed to initialize user settings: %d", ret);
            return ret;
        }
    }

    if (ctx->header.version != JUXTA_FRAMFS_VERSION)
    {
        LOG_WRN("File system version mismatch: %d (expected %d)",
                ctx->header.version, JUXTA_FRAMFS_VERSION);
    }

    /* Validate MAC header (after ensuring it's initialized) */
    if (ctx->mac_header.magic != JUXTA_FRAMFS_MAC_MAGIC)
    {
        LOG_ERR("Invalid MAC table magic: 0x%04X (expected 0x%04X)",
                ctx->mac_header.magic, JUXTA_FRAMFS_MAC_MAGIC);
        return JUXTA_FRAMFS_ERROR_INVALID;
    }

    if (ctx->mac_header.version != JUXTA_FRAMFS_MAC_VERSION)
    {
        LOG_WRN("MAC table version mismatch: %d (expected %d)",
                ctx->mac_header.version, JUXTA_FRAMFS_MAC_VERSION);
    }

    /* Validate user settings (after ensuring it's initialized) */
    if (ctx->user_settings.magic != JUXTA_FRAMFS_USER_SETTINGS_MAGIC)
    {
        LOG_ERR("Invalid user settings magic: 0x%04X (expected 0x%04X)",
                ctx->user_settings.magic, JUXTA_FRAMFS_USER_SETTINGS_MAGIC);
        return JUXTA_FRAMFS_ERROR_INVALID;
    }

    if (ctx->user_settings.version != JUXTA_FRAMFS_USER_SETTINGS_VERSION)
    {
        LOG_WRN("User settings version mismatch: %d (expected %d)",
                ctx->user_settings.version, JUXTA_FRAMFS_USER_SETTINGS_VERSION);
    }

    /* Find active file if any */
    ctx->active_file_index = framfs_find_active_file(ctx);

    ctx->initialized = true;

    LOG_INF("FRAM file system initialized: %d files, next_addr=0x%06X",
            ctx->header.file_count, ctx->header.next_data_addr);

    return JUXTA_FRAMFS_OK;
}

int juxta_framfs_format(struct juxta_framfs_context *ctx)
{
    if (!ctx || !ctx->fram_dev)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    LOG_INF("Formatting FRAM file system");

    /* Initialize header */
    memset(&ctx->header, 0, sizeof(ctx->header));
    ctx->header.magic = JUXTA_FRAMFS_MAGIC;
    ctx->header.version = JUXTA_FRAMFS_VERSION;
    ctx->header.file_count = 0;
    ctx->header.next_data_addr = framfs_get_data_start_addr();
    ctx->header.total_data_size = 0;

    /* Write header to FRAM */
    int ret = framfs_write_header(ctx);
    if (ret < 0)
    {
        LOG_ERR("Failed to write file system header: %d", ret);
        return ret;
    }

    /* Zero out the file entry table */
    uint8_t zero_buffer[32] = {0};
    for (int i = 0; i < JUXTA_FRAMFS_MAX_FILES; i++)
    {
        uint32_t addr = framfs_get_entry_addr(i);
        ret = juxta_fram_write(ctx->fram_dev, addr, zero_buffer, sizeof(zero_buffer));
        if (ret < 0)
        {
            LOG_ERR("Failed to clear entry %d: %d", i, ret);
            return JUXTA_FRAMFS_ERROR;
        }
    }

    ctx->active_file_index = -1;

    LOG_INF("File system formatted successfully");
    return JUXTA_FRAMFS_OK;
}

int juxta_framfs_get_stats(struct juxta_framfs_context *ctx,
                           struct juxta_framfs_header *header)
{
    if (!ctx || !ctx->initialized || !header)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    /* Read fresh header from FRAM */
    int ret = framfs_read_header(ctx);
    if (ret < 0)
    {
        return ret;
    }

    *header = ctx->header;
    return JUXTA_FRAMFS_OK;
}

/* ========================================================================
 * File Operations Functions
 * ======================================================================== */

int juxta_framfs_create_active(struct juxta_framfs_context *ctx,
                               const char *filename,
                               uint8_t file_type)
{
    if (!ctx || !ctx->initialized || !filename)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    /* Validate filename length first */
    if (strlen(filename) >= JUXTA_FRAMFS_FILENAME_LEN)
    {
        LOG_WRN("Filename too long: %s", filename);
        return JUXTA_FRAMFS_ERROR_SIZE;
    }

    /* Check if file already exists */
    int existing_index = framfs_find_file(ctx, filename);
    if (existing_index >= 0)
    {
        LOG_WRN("File already exists: %s", filename);
        return JUXTA_FRAMFS_ERROR_EXISTS;
    }

    /* Check if we have space for another file */
    if (ctx->header.file_count >= JUXTA_FRAMFS_MAX_FILES)
    {
        LOG_WRN("File system full (%d/%d files)",
                ctx->header.file_count, JUXTA_FRAMFS_MAX_FILES);
        return JUXTA_FRAMFS_ERROR_FULL;
    }

    /* Seal any existing active file */
    if (ctx->active_file_index >= 0)
    {
        int ret = juxta_framfs_seal_active(ctx);
        if (ret < 0)
        {
            LOG_ERR("Failed to seal previous active file: %d", ret);
            return ret;
        }
    }

    /* Create new file entry */
    struct juxta_framfs_entry new_entry;
    memset(&new_entry, 0, sizeof(new_entry));

    strncpy(new_entry.filename, filename, JUXTA_FRAMFS_FILENAME_LEN - 1);
    new_entry.start_addr = ctx->header.next_data_addr;
    new_entry.length = 0;
    new_entry.flags = JUXTA_FRAMFS_FLAG_VALID | JUXTA_FRAMFS_FLAG_ACTIVE;
    new_entry.file_type = file_type;

    /* Write entry to FRAM */
    uint16_t entry_index = ctx->header.file_count;
    int ret = framfs_write_entry(ctx, entry_index, &new_entry);
    if (ret < 0)
    {
        LOG_ERR("Failed to write file entry: %d", ret);
        return ret;
    }

    /* Update header */
    ctx->header.file_count++;
    ret = framfs_write_header(ctx);
    if (ret < 0)
    {
        LOG_ERR("Failed to update header: %d", ret);
        return ret;
    }

    ctx->active_file_index = entry_index;

    LOG_INF("Created active file: %s (index %d, addr 0x%06X)",
            filename, entry_index, new_entry.start_addr);

    return JUXTA_FRAMFS_OK;
}

int juxta_framfs_append(struct juxta_framfs_context *ctx,
                        const uint8_t *data,
                        size_t length)
{
    if (!ctx || !ctx->initialized || !data || length == 0)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    /* Check active file */
    if (ctx->active_file_index < 0)
    {
        LOG_WRN("No active file for append operation");
        return JUXTA_FRAMFS_ERROR_NO_ACTIVE;
    }

    /* Read current active file entry */
    struct juxta_framfs_entry entry;
    int ret = framfs_read_entry(ctx, ctx->active_file_index, &entry);
    if (ret < 0)
    {
        LOG_ERR("Failed to read active file entry: %d", ret);
        return ret;
    }

    /* Verify it's still active */
    if (!(entry.flags & JUXTA_FRAMFS_FLAG_ACTIVE))
    {
        LOG_WRN("File is not active: %s", entry.filename);
        return JUXTA_FRAMFS_ERROR_READ_ONLY;
    }

    /* Check FRAM bounds */
    uint32_t write_addr = entry.start_addr + entry.length;
    if (write_addr + length > JUXTA_FRAM_SIZE_BYTES)
    {
        LOG_WRN("Append would exceed FRAM size");
        return JUXTA_FRAMFS_ERROR_FULL;
    }

    /* Write data to FRAM */
    ret = juxta_fram_write(ctx->fram_dev, write_addr, data, length);
    if (ret < 0)
    {
        LOG_ERR("Failed to write data to FRAM: %d", ret);
        return ret;
    }

    /* Update entry with new length */
    entry.length += length;
    ret = framfs_write_entry(ctx, ctx->active_file_index, &entry);
    if (ret < 0)
    {
        LOG_ERR("Failed to update file entry: %d", ret);
        return ret;
    }

    /* Update header statistics */
    ctx->header.total_data_size += length;
    ctx->header.next_data_addr = write_addr + length;
    ret = framfs_write_header(ctx);
    if (ret < 0)
    {
        LOG_ERR("Failed to update header: %d", ret);
        return ret;
    }

    LOG_DBG("Appended %zu bytes to %s (total: %d bytes)",
            length, entry.filename, entry.length);

    return JUXTA_FRAMFS_OK;
}

int juxta_framfs_seal_active(struct juxta_framfs_context *ctx)
{
    if (!ctx || !ctx->initialized)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    if (ctx->active_file_index < 0)
    {
        LOG_DBG("No active file to seal");
        return JUXTA_FRAMFS_OK;
    }

    /* Read current active file entry */
    struct juxta_framfs_entry entry;
    int ret = framfs_read_entry(ctx, ctx->active_file_index, &entry);
    if (ret < 0)
    {
        LOG_ERR("Failed to read active file entry: %d", ret);
        return ret;
    }

    /* Update flags to sealed */
    entry.flags &= ~JUXTA_FRAMFS_FLAG_ACTIVE;
    entry.flags |= JUXTA_FRAMFS_FLAG_SEALED;

    /* Write updated entry */
    ret = framfs_write_entry(ctx, ctx->active_file_index, &entry);
    if (ret < 0)
    {
        LOG_ERR("Failed to seal file entry: %d", ret);
        return ret;
    }

    LOG_INF("Sealed file: %s (%d bytes)", entry.filename, entry.length);

    ctx->active_file_index = -1;
    return JUXTA_FRAMFS_OK;
}

int juxta_framfs_read(struct juxta_framfs_context *ctx,
                      const char *filename,
                      uint32_t offset,
                      uint8_t *buffer,
                      size_t length)
{
    if (!ctx || !ctx->initialized || !filename || !buffer || length == 0)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    /* Find file */
    int file_index = framfs_find_file(ctx, filename);
    if (file_index < 0)
    {
        LOG_WRN("File not found: %s", filename);
        return JUXTA_FRAMFS_ERROR_NOT_FOUND;
    }

    /* Read file entry */
    struct juxta_framfs_entry entry;
    int ret = framfs_read_entry(ctx, file_index, &entry);
    if (ret < 0)
    {
        LOG_ERR("Failed to read file entry: %d", ret);
        return ret;
    }

    /* Validate read bounds */
    if (offset >= entry.length)
    {
        LOG_WRN("Read offset beyond file size: %d >= %d", offset, entry.length);
        return JUXTA_FRAMFS_ERROR_SIZE;
    }

    /* Adjust length if reading beyond end of file */
    size_t available = entry.length - offset;
    if (length > available)
    {
        length = available;
    }

    /* Read data from FRAM */
    uint32_t read_addr = entry.start_addr + offset;
    LOG_INF("📁 FRAMFS READ: file=%s, entry.start_addr=0x%06X, offset=%u, read_addr=0x%06X, length=%zu",
            filename, (unsigned)entry.start_addr, (unsigned)offset, (unsigned)read_addr, length);

    ret = juxta_fram_read(ctx->fram_dev, read_addr, buffer, length);
    if (ret < 0)
    {
        LOG_ERR("Failed to read from FRAM: %d", ret);
        return ret;
    }

    LOG_INF("📁 FRAMFS READ SUCCESS: read %zu bytes from FRAM addr 0x%06X", length, (unsigned)read_addr);

    /* Log first few bytes for debugging */
    if (length >= 12 && offset == 0)
    {
        LOG_INF("📁 FRAMFS READ DATA (first 12 bytes): %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                buffer[0], buffer[1], buffer[2], buffer[3],
                buffer[4], buffer[5], buffer[6], buffer[7],
                buffer[8], buffer[9], buffer[10], buffer[11]);
    }
    return (int)length;
}

int juxta_framfs_get_file_size(struct juxta_framfs_context *ctx,
                               const char *filename)
{
    if (!ctx || !ctx->initialized || !filename)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    /* Find file */
    int file_index = framfs_find_file(ctx, filename);
    if (file_index < 0)
    {
        LOG_WRN("File not found: %s", filename);
        return JUXTA_FRAMFS_ERROR_NOT_FOUND;
    }

    /* Read file entry */
    struct juxta_framfs_entry entry;
    int ret = framfs_read_entry(ctx, file_index, &entry);
    if (ret < 0)
    {
        return ret;
    }

    return (int)entry.length;
}

int juxta_framfs_get_file_info(struct juxta_framfs_context *ctx,
                               const char *filename,
                               struct juxta_framfs_entry *entry)
{
    if (!ctx || !ctx->initialized || !filename || !entry)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    /* Find file */
    int file_index = framfs_find_file(ctx, filename);
    if (file_index < 0)
    {
        LOG_WRN("File not found: %s", filename);
        return JUXTA_FRAMFS_ERROR_NOT_FOUND;
    }

    /* Read file entry */
    return framfs_read_entry(ctx, file_index, entry);
}

/* ========================================================================
 * File Listing Functions
 * ======================================================================== */

int juxta_framfs_list_files(struct juxta_framfs_context *ctx,
                            char filenames[][JUXTA_FRAMFS_FILENAME_LEN],
                            uint16_t max_files)
{
    if (!ctx || !ctx->initialized || !filenames)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    uint16_t count = 0;
    for (uint16_t i = 0; i < ctx->header.file_count && count < max_files; i++)
    {
        struct juxta_framfs_entry entry;
        int ret = framfs_read_entry(ctx, i, &entry);
        if (ret < 0)
        {
            LOG_ERR("Failed to read entry %d: %d", i, ret);
            continue;
        }

        if (entry.flags & JUXTA_FRAMFS_FLAG_VALID)
        {
            strncpy(filenames[count], entry.filename, JUXTA_FRAMFS_FILENAME_LEN - 1);
            filenames[count][JUXTA_FRAMFS_FILENAME_LEN - 1] = '\0';
            count++;
        }
    }

    return count;
}

int juxta_framfs_get_active_filename(struct juxta_framfs_context *ctx,
                                     char *filename)
{
    if (!ctx || !ctx->initialized || !filename)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    if (ctx->active_file_index < 0)
    {
        LOG_WRN("No active file");
        return JUXTA_FRAMFS_ERROR_NO_ACTIVE;
    }

    struct juxta_framfs_entry entry;
    int ret = framfs_read_entry(ctx, ctx->active_file_index, &entry);
    if (ret < 0)
    {
        return ret;
    }

    strncpy(filename, entry.filename, JUXTA_FRAMFS_FILENAME_LEN);
    filename[JUXTA_FRAMFS_FILENAME_LEN - 1] = '\0';

    return JUXTA_FRAMFS_OK;
}

/* ========================================================================
 * Internal Helper Functions
 * ======================================================================== */

static int framfs_read_header(struct juxta_framfs_context *ctx)
{
    return juxta_fram_read(ctx->fram_dev, 0x0000,
                           (uint8_t *)&ctx->header, sizeof(ctx->header));
}

static int framfs_write_header(struct juxta_framfs_context *ctx)
{
    return juxta_fram_write(ctx->fram_dev, 0x0000,
                            (uint8_t *)&ctx->header, sizeof(ctx->header));
}

static int framfs_read_entry(struct juxta_framfs_context *ctx, uint16_t index,
                             struct juxta_framfs_entry *entry)
{
    if (index >= JUXTA_FRAMFS_MAX_FILES)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    uint32_t addr = framfs_get_entry_addr(index);
    return juxta_fram_read(ctx->fram_dev, addr, (uint8_t *)entry, sizeof(*entry));
}

static int framfs_write_entry(struct juxta_framfs_context *ctx, uint16_t index,
                              const struct juxta_framfs_entry *entry)
{
    if (index >= JUXTA_FRAMFS_MAX_FILES)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    uint32_t addr = framfs_get_entry_addr(index);
    return juxta_fram_write(ctx->fram_dev, addr, (uint8_t *)entry, sizeof(*entry));
}

static int framfs_find_file(struct juxta_framfs_context *ctx, const char *filename)
{
    for (uint16_t i = 0; i < ctx->header.file_count; i++)
    {
        struct juxta_framfs_entry entry;
        int ret = framfs_read_entry(ctx, i, &entry);
        if (ret < 0)
        {
            continue;
        }

        if ((entry.flags & JUXTA_FRAMFS_FLAG_VALID) &&
            strncmp(entry.filename, filename, JUXTA_FRAMFS_FILENAME_LEN) == 0)
        {
            return i;
        }
    }
    return -1;
}

static int framfs_find_active_file(struct juxta_framfs_context *ctx)
{
    for (uint16_t i = 0; i < ctx->header.file_count; i++)
    {
        struct juxta_framfs_entry entry;
        int ret = framfs_read_entry(ctx, i, &entry);
        if (ret < 0)
        {
            continue;
        }

        if ((entry.flags & JUXTA_FRAMFS_FLAG_VALID) &&
            (entry.flags & JUXTA_FRAMFS_FLAG_ACTIVE))
        {
            return i;
        }
    }
    return -1;
}

static uint32_t framfs_get_entry_addr(uint16_t index)
{
    return sizeof(struct juxta_framfs_header) +
           (index * sizeof(struct juxta_framfs_entry));
}

static uint32_t framfs_get_data_start_addr(void)
{
    return sizeof(struct juxta_framfs_header) +
           (JUXTA_FRAMFS_MAX_FILES * sizeof(struct juxta_framfs_entry)) +
           sizeof(struct juxta_framfs_mac_header) +
           (JUXTA_FRAMFS_MAX_MAC_ADDRESSES * sizeof(struct juxta_framfs_mac_entry)) +
           sizeof(struct juxta_framfs_user_settings);
}

/* ========================================================================
 * MAC Table Helper Functions
 * ======================================================================== */

static uint32_t framfs_get_mac_header_addr(void)
{
    return sizeof(struct juxta_framfs_header) +
           (JUXTA_FRAMFS_MAX_FILES * sizeof(struct juxta_framfs_entry));
}

static uint32_t framfs_get_mac_entry_addr(uint8_t index)
{
    return framfs_get_mac_header_addr() +
           sizeof(struct juxta_framfs_mac_header) +
           (index * sizeof(struct juxta_framfs_mac_entry));
}

/* ========================================================================
 * User Settings Helper Functions
 * ======================================================================== */

static uint32_t framfs_get_user_settings_addr(void)
{
    return sizeof(struct juxta_framfs_header) +
           (JUXTA_FRAMFS_MAX_FILES * sizeof(struct juxta_framfs_entry)) +
           sizeof(struct juxta_framfs_mac_header) +
           (JUXTA_FRAMFS_MAX_MAC_ADDRESSES * sizeof(struct juxta_framfs_mac_entry));
}

static int framfs_read_user_settings(struct juxta_framfs_context *ctx)
{
    uint32_t addr = framfs_get_user_settings_addr();
    return juxta_fram_read(ctx->fram_dev, addr,
                           (uint8_t *)&ctx->user_settings, sizeof(ctx->user_settings));
}

static int framfs_write_user_settings(struct juxta_framfs_context *ctx)
{
    uint32_t addr = framfs_get_user_settings_addr();
    return juxta_fram_write(ctx->fram_dev, addr,
                            (uint8_t *)&ctx->user_settings, sizeof(ctx->user_settings));
}

static int framfs_read_mac_header(struct juxta_framfs_context *ctx)
{
    uint32_t addr = framfs_get_mac_header_addr();
    return juxta_fram_read(ctx->fram_dev, addr,
                           (uint8_t *)&ctx->mac_header, sizeof(ctx->mac_header));
}

static int framfs_write_mac_header(struct juxta_framfs_context *ctx)
{
    uint32_t addr = framfs_get_mac_header_addr();
    return juxta_fram_write(ctx->fram_dev, addr,
                            (uint8_t *)&ctx->mac_header, sizeof(ctx->mac_header));
}

static int framfs_read_mac_entry(struct juxta_framfs_context *ctx, uint8_t index,
                                 struct juxta_framfs_mac_entry *entry)
{
    if (index >= JUXTA_FRAMFS_MAX_MAC_ADDRESSES)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    uint32_t addr = framfs_get_mac_entry_addr(index);
    return juxta_fram_read(ctx->fram_dev, addr, (uint8_t *)entry, sizeof(*entry));
}

static int framfs_write_mac_entry(struct juxta_framfs_context *ctx, uint8_t index,
                                  const struct juxta_framfs_mac_entry *entry)
{
    if (index >= JUXTA_FRAMFS_MAX_MAC_ADDRESSES)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    uint32_t addr = framfs_get_mac_entry_addr(index);
    return juxta_fram_write(ctx->fram_dev, addr, (uint8_t *)entry, sizeof(*entry));
}

/* ========================================================================
 * MAC Address Table API Functions
 * ======================================================================== */

int juxta_framfs_mac_find_or_add(struct juxta_framfs_context *ctx,
                                 const uint8_t *mac_id,
                                 uint8_t *index)
{
    if (!ctx || !ctx->initialized || !mac_id || !index)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    /* First try to find existing MAC ID */
    int ret = juxta_framfs_mac_find(ctx, mac_id, index);
    if (ret == 0)
    {
        /* Found existing MAC, increment usage */
        ret = juxta_framfs_mac_increment_usage(ctx, *index);
        return ret;
    }

    /* MAC not found, check if table is full */
    if (ctx->mac_header.entry_count >= JUXTA_FRAMFS_MAX_MAC_ADDRESSES)
    {
        LOG_ERR("MAC ID table is full (%d/%d)",
                ctx->mac_header.entry_count, JUXTA_FRAMFS_MAX_MAC_ADDRESSES);
        return JUXTA_FRAMFS_ERROR_MAC_FULL;
    }

    /* Add new MAC ID */
    struct juxta_framfs_mac_entry new_entry;
    memset(&new_entry, 0, sizeof(new_entry));
    memcpy(new_entry.mac_id, mac_id, JUXTA_FRAMFS_MAC_ADDRESS_SIZE);
    new_entry.usage_count = 1;
    new_entry.flags = 0x01; /* Valid entry */

    /* Write to next available slot */
    uint8_t new_index = ctx->mac_header.entry_count;
    ret = framfs_write_mac_entry(ctx, new_index, &new_entry);
    if (ret < 0)
    {
        LOG_ERR("Failed to write MAC entry: %d", ret);
        return ret;
    }

    /* Update header */
    ctx->mac_header.entry_count++;
    ret = framfs_write_mac_header(ctx);
    if (ret < 0)
    {
        LOG_ERR("Failed to update MAC header: %d", ret);
        return ret;
    }

    *index = new_index;
    LOG_DBG("Added MAC ID at index %d", new_index);
    return JUXTA_FRAMFS_OK;
}

int juxta_framfs_mac_find(struct juxta_framfs_context *ctx,
                          const uint8_t *mac_id,
                          uint8_t *index)
{
    if (!ctx || !ctx->initialized || !mac_id || !index)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    /* Search through all valid entries */
    for (uint8_t i = 0; i < ctx->mac_header.entry_count; i++)
    {
        struct juxta_framfs_mac_entry entry;
        int ret = framfs_read_mac_entry(ctx, i, &entry);
        if (ret < 0)
        {
            LOG_ERR("Failed to read MAC entry %d: %d", i, ret);
            continue;
        }

        if ((entry.flags & 0x01) && /* Valid entry */
            memcmp(entry.mac_id, mac_id, JUXTA_FRAMFS_MAC_ADDRESS_SIZE) == 0)
        {
            *index = i;
            return JUXTA_FRAMFS_OK;
        }
    }

    return JUXTA_FRAMFS_ERROR_MAC_NOT_FOUND;
}

int juxta_framfs_mac_get_by_index(struct juxta_framfs_context *ctx,
                                  uint8_t index,
                                  uint8_t *mac_id)
{
    if (!ctx || !ctx->initialized || !mac_id)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    if (index >= ctx->mac_header.entry_count)
    {
        LOG_WRN("MAC index out of range: %d >= %d", index, ctx->mac_header.entry_count);
        return JUXTA_FRAMFS_ERROR;
    }

    struct juxta_framfs_mac_entry entry;
    int ret = framfs_read_mac_entry(ctx, index, &entry);
    if (ret < 0)
    {
        LOG_ERR("Failed to read MAC entry %d: %d", index, ret);
        return ret;
    }

    if (!(entry.flags & 0x01))
    {
        LOG_WRN("MAC entry %d is not valid", index);
        return JUXTA_FRAMFS_ERROR;
    }

    memcpy(mac_id, entry.mac_id, JUXTA_FRAMFS_MAC_ADDRESS_SIZE);
    return JUXTA_FRAMFS_OK;
}

int juxta_framfs_mac_increment_usage(struct juxta_framfs_context *ctx,
                                     uint8_t index)
{
    if (!ctx || !ctx->initialized)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    if (index >= ctx->mac_header.entry_count)
    {
        LOG_WRN("MAC index out of range: %d >= %d", index, ctx->mac_header.entry_count);
        return JUXTA_FRAMFS_ERROR;
    }

    struct juxta_framfs_mac_entry entry;
    int ret = framfs_read_mac_entry(ctx, index, &entry);
    if (ret < 0)
    {
        LOG_ERR("Failed to read MAC entry %d: %d", index, ret);
        return ret;
    }

    if (!(entry.flags & 0x01))
    {
        LOG_WRN("MAC entry %d is not valid", index);
        return JUXTA_FRAMFS_ERROR;
    }

    /* Increment usage count (saturate at 255) */
    if (entry.usage_count < 255)
    {
        entry.usage_count++;
    }

    ret = framfs_write_mac_entry(ctx, index, &entry);
    if (ret < 0)
    {
        LOG_ERR("Failed to write MAC entry %d: %d", index, ret);
        return ret;
    }

    return JUXTA_FRAMFS_OK;
}

int juxta_framfs_mac_get_stats(struct juxta_framfs_context *ctx,
                               uint8_t *entry_count,
                               uint32_t *total_usage)
{
    if (!ctx || !ctx->initialized)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    if (entry_count)
    {
        *entry_count = ctx->mac_header.entry_count;
    }

    if (total_usage)
    {
        *total_usage = 0; /* No longer tracked */
    }

    return JUXTA_FRAMFS_OK;
}

int juxta_framfs_mac_clear(struct juxta_framfs_context *ctx)
{
    if (!ctx || !ctx->fram_dev)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    LOG_INF("Clearing MAC address table");

    /* Initialize MAC header */
    memset(&ctx->mac_header, 0, sizeof(ctx->mac_header));
    ctx->mac_header.magic = JUXTA_FRAMFS_MAC_MAGIC;
    ctx->mac_header.version = JUXTA_FRAMFS_MAC_VERSION;
    ctx->mac_header.entry_count = 0;

    /* Write header to FRAM */
    int ret = framfs_write_mac_header(ctx);
    if (ret < 0)
    {
        LOG_ERR("Failed to write MAC header: %d", ret);
        return ret;
    }

    /* Zero out all MAC entries */
    uint8_t zero_buffer[sizeof(struct juxta_framfs_mac_entry)] = {0};
    for (int i = 0; i < JUXTA_FRAMFS_MAX_MAC_ADDRESSES; i++)
    {
        uint32_t addr = framfs_get_mac_entry_addr(i);
        ret = juxta_fram_write(ctx->fram_dev, addr, zero_buffer, sizeof(zero_buffer));
        if (ret < 0)
        {
            LOG_ERR("Failed to clear MAC entry %d: %d", i, ret);
            return JUXTA_FRAMFS_ERROR;
        }
    }

    LOG_INF("MAC address table cleared successfully");
    return JUXTA_FRAMFS_OK;
}

int juxta_framfs_get_mac_table_data_size(struct juxta_framfs_context *ctx, uint32_t *size)
{
    if (!ctx || !ctx->initialized || !size)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    /* Calculate size: 3 bytes per valid MAC ID (entry_count from header) */
    *size = ctx->mac_header.entry_count * JUXTA_FRAMFS_MAC_ADDRESS_SIZE;
    return JUXTA_FRAMFS_OK;
}

int juxta_framfs_read_mac_table_data(struct juxta_framfs_context *ctx,
                                     uint32_t offset, uint8_t *buffer, size_t length)
{
    if (!ctx || !ctx->initialized || !buffer)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    /* Calculate total data size */
    uint32_t total_size = ctx->mac_header.entry_count * JUXTA_FRAMFS_MAC_ADDRESS_SIZE;

    /* Check bounds */
    if (offset >= total_size)
    {
        return 0; /* End of data */
    }

    /* Calculate how many bytes we can read */
    size_t bytes_to_read = MIN(length, total_size - offset);

    /* Calculate which MAC entries we need to read */
    uint8_t start_entry = offset / JUXTA_FRAMFS_MAC_ADDRESS_SIZE;
    uint8_t end_entry = (offset + bytes_to_read - 1) / JUXTA_FRAMFS_MAC_ADDRESS_SIZE;

    /* Read MAC entries and extract just the 3-byte MAC IDs */
    size_t bytes_read = 0;
    for (uint8_t i = start_entry; i <= end_entry && i < ctx->mac_header.entry_count; i++)
    {
        struct juxta_framfs_mac_entry entry;
        int ret = framfs_read_mac_entry(ctx, i, &entry);
        if (ret != JUXTA_FRAMFS_OK)
        {
            return ret;
        }

        /* Calculate offset within this entry */
        uint32_t entry_offset = offset + bytes_read - (i * JUXTA_FRAMFS_MAC_ADDRESS_SIZE);
        size_t copy_size = MIN(JUXTA_FRAMFS_MAC_ADDRESS_SIZE - entry_offset,
                               bytes_to_read - bytes_read);

        /* Copy just the 3-byte MAC ID */
        memcpy(buffer + bytes_read, entry.mac_id + entry_offset, copy_size);
        bytes_read += copy_size;
    }

    return bytes_read;
}

/* ========================================================================
 * User Settings API Functions
 * ======================================================================== */

/* Advertising and scanning interval functions removed - now session-based only */

/* Operating mode functions removed - operating mode is now session-based only */

int juxta_framfs_get_subject_id(struct juxta_framfs_context *ctx,
                                char *subject_id)
{
    if (!ctx || !ctx->initialized || !subject_id)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    strncpy(subject_id, ctx->user_settings.subject_id, JUXTA_FRAMFS_SUBJECT_ID_LEN);
    subject_id[JUXTA_FRAMFS_SUBJECT_ID_LEN - 1] = '\0';
    return JUXTA_FRAMFS_OK;
}

int juxta_framfs_set_subject_id(struct juxta_framfs_context *ctx,
                                const char *subject_id)
{
    if (!ctx || !ctx->initialized || !subject_id)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    strncpy(ctx->user_settings.subject_id, subject_id, JUXTA_FRAMFS_SUBJECT_ID_LEN - 1);
    ctx->user_settings.subject_id[JUXTA_FRAMFS_SUBJECT_ID_LEN - 1] = '\0';
    return framfs_write_user_settings(ctx);
}

int juxta_framfs_get_upload_path(struct juxta_framfs_context *ctx,
                                 char *upload_path)
{
    if (!ctx || !ctx->initialized || !upload_path)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    strncpy(upload_path, ctx->user_settings.upload_path, JUXTA_FRAMFS_UPLOAD_PATH_LEN);
    upload_path[JUXTA_FRAMFS_UPLOAD_PATH_LEN - 1] = '\0';
    return JUXTA_FRAMFS_OK;
}

int juxta_framfs_set_upload_path(struct juxta_framfs_context *ctx,
                                 const char *upload_path)
{
    if (!ctx || !ctx->initialized || !upload_path)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    strncpy(ctx->user_settings.upload_path, upload_path, JUXTA_FRAMFS_UPLOAD_PATH_LEN - 1);
    ctx->user_settings.upload_path[JUXTA_FRAMFS_UPLOAD_PATH_LEN - 1] = '\0';
    return framfs_write_user_settings(ctx);
}

int juxta_framfs_get_user_settings(struct juxta_framfs_context *ctx,
                                   struct juxta_framfs_user_settings *settings)
{
    if (!ctx || !ctx->initialized || !settings)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    *settings = ctx->user_settings;
    return JUXTA_FRAMFS_OK;
}

int juxta_framfs_set_user_settings(struct juxta_framfs_context *ctx,
                                   const struct juxta_framfs_user_settings *settings)
{
    if (!ctx || !ctx->initialized || !settings)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    ctx->user_settings = *settings;
    return framfs_write_user_settings(ctx);
}

int juxta_framfs_clear_user_settings(struct juxta_framfs_context *ctx)
{
    if (!ctx || !ctx->fram_dev)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    LOG_INF("Clearing user settings");

    /* Initialize user settings with defaults */
    memset(&ctx->user_settings, 0, sizeof(ctx->user_settings));
    ctx->user_settings.magic = JUXTA_FRAMFS_USER_SETTINGS_MAGIC;
    ctx->user_settings.version = JUXTA_FRAMFS_USER_SETTINGS_VERSION;
    ctx->user_settings.reserved1 = 0x00;             /* Reserved (was operating_mode) */
    ctx->user_settings.reserved2 = 0x00;             /* Reserved (was adv_interval) */
    ctx->user_settings.reserved3 = 0x00;             /* Reserved (was scan_interval) */
    strcpy(ctx->user_settings.subject_id, "");       /* Default: empty */
    strcpy(ctx->user_settings.upload_path, "/TEST"); /* Default: /TEST */

    /* Initialize ADC configuration with defaults */
    ctx->user_settings.adc_config.mode = JUXTA_FRAMFS_ADC_MODE_TIMER_BURST;
    ctx->user_settings.adc_config.threshold_mv = 0;          /* 0 = always trigger (timer mode) */
    ctx->user_settings.adc_config.buffer_size = 1000;        /* Default buffer size */
    ctx->user_settings.adc_config.debounce_ms = 5000;        /* 5 second intervals (timer mode) */
    ctx->user_settings.adc_config.output_peaks_only = false; /* Full waveform */

    /* Write to FRAM */
    int ret = framfs_write_user_settings(ctx);
    if (ret < 0)
    {
        LOG_ERR("Failed to write user settings: %d", ret);
        return ret;
    }

    LOG_INF("User settings cleared successfully");
    return JUXTA_FRAMFS_OK;
}

int juxta_framfs_get_adc_config(struct juxta_framfs_context *ctx,
                                struct juxta_framfs_adc_config *config)
{
    if (!ctx || !ctx->initialized || !config)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    *config = ctx->user_settings.adc_config;
    return JUXTA_FRAMFS_OK;
}

int juxta_framfs_set_adc_config(struct juxta_framfs_context *ctx,
                                const struct juxta_framfs_adc_config *config)
{
    if (!ctx || !ctx->initialized || !config)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    /* Validate configuration */
    if (config->mode > JUXTA_FRAMFS_ADC_MODE_THRESHOLD_EVENT)
    {
        LOG_WRN("Invalid ADC mode: %d", config->mode);
        return JUXTA_FRAMFS_ERROR;
    }

    if (config->buffer_size < 1 || config->buffer_size > 1000)
    {
        LOG_WRN("Invalid buffer size: %d (range: 1-1000, limited to prevent duration overflow)", config->buffer_size);
        return JUXTA_FRAMFS_ERROR;
    }

    ctx->user_settings.adc_config = *config;

    LOG_INF("📊 ADC config updated: mode=%d, threshold=%u mV, buffer=%u, debounce=%u ms, peaks_only=%s",
            config->mode, (unsigned)config->threshold_mv, config->buffer_size,
            (unsigned)config->debounce_ms, config->output_peaks_only ? "true" : "false");

    return framfs_write_user_settings(ctx);
}

/* ========================================================================
 * Data Encoding/Decoding Functions
 * ======================================================================== */

int juxta_framfs_encode_device_record(const struct juxta_framfs_device_record *record,
                                      uint8_t *buffer,
                                      size_t buffer_size)
{
    if (!record || !buffer)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    /* Calculate required buffer size */
    size_t required_size = 6 + (2 * record->type); /* minute + type + motion + battery + temp + mac_indices + rssi_values */
    if (buffer_size < required_size)
    {
        LOG_WRN("Buffer too small: %zu < %zu", buffer_size, required_size);
        return JUXTA_FRAMFS_ERROR_SIZE;
    }

    /* Validate device count */
    if (record->type > 128)
    {
        LOG_WRN("Invalid device count: %d", record->type);
        return JUXTA_FRAMFS_ERROR;
    }

    /* Encode fixed fields */
    buffer[0] = (record->minute >> 8) & 0xFF; /* minute high byte */
    buffer[1] = record->minute & 0xFF;        /* minute low byte */
    buffer[2] = record->type;                 /* device count (0-128) */
    buffer[3] = record->motion_count;         /* motion count */
    buffer[4] = record->battery_level;        /* battery level */
    buffer[5] = (uint8_t)record->temperature; /* temperature (signed) */

    /* Encode variable fields */
    size_t offset = 6;
    for (int i = 0; i < record->type; i++)
    {
        buffer[offset + i] = record->mac_indices[i];                /* MAC index */
        buffer[offset + record->type + i] = record->rssi_values[i]; /* RSSI value */
    }

    return (int)required_size;
}

int juxta_framfs_decode_device_record(const uint8_t *buffer,
                                      size_t buffer_size,
                                      struct juxta_framfs_device_record *record)
{
    if (!buffer || !record || buffer_size < 6)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    /* Decode fixed fields */
    record->minute = (buffer[0] << 8) | buffer[1]; /* minute */
    record->type = buffer[2];                      /* device count */
    record->motion_count = buffer[3];              /* motion count */
    record->battery_level = buffer[4];             /* battery level */
    record->temperature = (int8_t)buffer[5];       /* temperature (signed) */

    /* Validate device count */
    if (record->type > 128)
    {
        LOG_WRN("Invalid device count: %d", record->type);
        return JUXTA_FRAMFS_ERROR;
    }

    /* Calculate required buffer size */
    size_t required_size = 6 + (2 * record->type);
    if (buffer_size < required_size)
    {
        LOG_WRN("Buffer too small: %zu < %zu", buffer_size, required_size);
        return JUXTA_FRAMFS_ERROR_SIZE;
    }

    /* Decode variable fields */
    size_t offset = 6;
    for (int i = 0; i < record->type; i++)
    {
        record->mac_indices[i] = buffer[offset + i];                        /* MAC index */
        record->rssi_values[i] = (int8_t)buffer[offset + record->type + i]; /* RSSI value */
    }

    return (int)required_size;
}

int juxta_framfs_encode_simple_record(const struct juxta_framfs_simple_record *record,
                                      uint8_t *buffer)
{
    if (!record || !buffer)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    buffer[0] = (record->minute >> 8) & 0xFF; /* minute high byte */
    buffer[1] = record->minute & 0xFF;        /* minute low byte */
    buffer[2] = record->type;                 /* record type */

    return 3; /* 3 bytes */
}

int juxta_framfs_decode_simple_record(const uint8_t *buffer,
                                      struct juxta_framfs_simple_record *record)
{
    if (!buffer || !record)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    record->minute = (buffer[0] << 8) | buffer[1]; /* minute */
    record->type = buffer[2];                      /* record type */

    return 3; /* 3 bytes */
}

int juxta_framfs_append_device_scan(struct juxta_framfs_context *ctx,
                                    uint16_t minute,
                                    uint8_t motion_count,
                                    uint8_t battery_level,
                                    int8_t temperature,
                                    const uint8_t (*mac_ids)[3],
                                    const int8_t *rssi_values,
                                    uint8_t device_count)
{
    if (!ctx || !ctx->initialized)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    if (device_count > 128)
    {
        LOG_WRN("Invalid device count: %d", device_count);
        return JUXTA_FRAMFS_ERROR;
    }

    /* Prepare device record */
    struct juxta_framfs_device_record record;
    record.minute = minute;
    record.type = device_count;
    record.motion_count = motion_count;
    record.battery_level = battery_level;
    record.temperature = temperature;

    /* Process MAC IDs and get indices (only if devices found) */
    if (device_count > 0 && mac_ids && rssi_values)
    {
        for (int i = 0; i < device_count; i++)
        {
            uint8_t mac_index;
            int ret = juxta_framfs_mac_find_or_add(ctx, mac_ids[i], &mac_index);
            if (ret < 0)
            {
                LOG_ERR("Failed to process MAC ID %d: %d", i, ret);
                return ret;
            }
            record.mac_indices[i] = mac_index;
            record.rssi_values[i] = rssi_values[i];
        }
    }

    /* Encode record */
    uint8_t buffer[6 + (2 * 128)]; /* Maximum size for 128 devices */
    int encoded_size = juxta_framfs_encode_device_record(&record, buffer, sizeof(buffer));
    if (encoded_size < 0)
    {
        LOG_ERR("Failed to encode device record: %d", encoded_size);
        return encoded_size;
    }

    /* Append to active file */
    return juxta_framfs_append(ctx, buffer, encoded_size);
}

int juxta_framfs_append_simple_record(struct juxta_framfs_context *ctx,
                                      uint16_t minute,
                                      uint8_t type)
{
    if (!ctx || !ctx->initialized)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    /* Validate record type */
    if (type != JUXTA_FRAMFS_RECORD_TYPE_NO_ACTIVITY &&
        type != JUXTA_FRAMFS_RECORD_TYPE_BOOT &&
        type != JUXTA_FRAMFS_RECORD_TYPE_CONNECTED &&
        type != JUXTA_FRAMFS_RECORD_TYPE_ERROR)
    {
        LOG_WRN("Invalid simple record type: 0x%02X", type);
        return JUXTA_FRAMFS_ERROR;
    }

    /* Prepare simple record */
    struct juxta_framfs_simple_record record;
    record.minute = minute;
    record.type = type;

    /* Encode record */
    uint8_t buffer[3];
    int ret = juxta_framfs_encode_simple_record(&record, buffer);
    if (ret < 0)
    {
        LOG_ERR("Failed to encode simple record: %d", ret);
        return ret;
    }

    /* Append to active file */
    return juxta_framfs_append(ctx, buffer, 3);
}

/* ========================================================================
 * Primary File System API (Time-Aware)
 * ======================================================================== */

int juxta_framfs_init_with_time(struct juxta_framfs_ctx *ctx,
                                struct juxta_framfs_context *fs_ctx,
                                uint32_t (*get_rtc_time)(void),
                                bool auto_management)
{
    if (!ctx || !fs_ctx || !get_rtc_time)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    if (!fs_ctx->initialized)
    {
        LOG_ERR("File system context not initialized");
        return JUXTA_FRAMFS_ERROR_INIT;
    }

    /* Initialize time context */
    memset(ctx, 0, sizeof(*ctx));
    ctx->fs_ctx = fs_ctx;
    ctx->get_rtc_time = get_rtc_time;
    ctx->auto_file_management = auto_management;

    /* Get current date and initialize filename */
    ctx->current_file_date = get_rtc_time();
    snprintf(ctx->current_filename, sizeof(ctx->current_filename),
             "%06u", ctx->current_file_date);

    LOG_INF("📁 File system initialized with date: %06u, filename: %s",
            ctx->current_file_date, ctx->current_filename);

    LOG_INF("File system initialized with time management for date: %s", ctx->current_filename);
    return JUXTA_FRAMFS_OK;
}

int juxta_framfs_ensure_current_file(struct juxta_framfs_ctx *ctx)
{
    if (!ctx || !ctx->fs_ctx)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    LOG_INF("🔍 ensure_current_file called - checking date");

    /* Get current date from RTC */
    uint32_t current_date = ctx->get_rtc_time();

    LOG_INF("📅 File system date check: current=%06u, stored=%06u",
            current_date, ctx->current_file_date);

    /* Check if we need to create or switch files */
    if (current_date != ctx->current_file_date || ctx->fs_ctx->active_file_index < 0)
    {
        if (current_date != ctx->current_file_date)
        {
            LOG_INF("Date changed from %06u to %06u, switching files",
                    ctx->current_file_date, current_date);
        }
        else
        {
            LOG_INF("No active file, creating new file");
        }

        /* Seal current file if it exists and is active */
        if (ctx->fs_ctx->active_file_index >= 0)
        {
            int ret = juxta_framfs_seal_active(ctx->fs_ctx);
            if (ret < 0)
            {
                LOG_ERR("Failed to seal current file: %d", ret);
                return ret;
            }
        }

        /* Update context with new date */
        ctx->current_file_date = current_date;
        snprintf(ctx->current_filename, sizeof(ctx->current_filename),
                 "%06u", current_date);

        /* Try to create new active file */
        int ret = juxta_framfs_create_active(ctx->fs_ctx,
                                             ctx->current_filename,
                                             JUXTA_FRAMFS_TYPE_SENSOR_LOG);
        if (ret == JUXTA_FRAMFS_ERROR_EXISTS)
        {
            /* File already exists - check if we should reactivate or create fresh */
            int existing_index = framfs_find_file(ctx->fs_ctx, ctx->current_filename);
            if (existing_index >= 0)
            {
                struct juxta_framfs_entry entry;
                ret = framfs_read_entry(ctx->fs_ctx, existing_index, &entry);
                if (ret >= 0)
                {
                    if (entry.flags & JUXTA_FRAMFS_FLAG_ACTIVE)
                    {
                        /* File is already active, use it */
                        ctx->fs_ctx->active_file_index = existing_index;
                        LOG_INF("Using existing active file: %s", ctx->current_filename);
                        return JUXTA_FRAMFS_OK;
                    }
                    else
                    {
                        /* File exists but is not active - reset it to start fresh */
                        LOG_INF("Resetting existing file to start fresh: %s", ctx->current_filename);

                        /* Read current header to get accurate next_data_addr */
                        int header_ret = framfs_read_header(ctx->fs_ctx);
                        if (header_ret < 0)
                        {
                            LOG_ERR("Failed to read header for file reset: %d", header_ret);
                            return header_ret;
                        }

                        entry.start_addr = ctx->fs_ctx->header.next_data_addr;
                        entry.length = 0;
                        entry.flags = JUXTA_FRAMFS_FLAG_VALID | JUXTA_FRAMFS_FLAG_ACTIVE;

                        ret = framfs_write_entry(ctx->fs_ctx, existing_index, &entry);
                        if (ret >= 0)
                        {
                            ctx->fs_ctx->active_file_index = existing_index;
                            LOG_INF("Reset and reactivated file: %s (new addr 0x%06X)",
                                    ctx->current_filename, entry.start_addr);
                            return JUXTA_FRAMFS_OK;
                        }
                    }
                }
            }
        }

        if (ret < 0)
        {
            LOG_ERR("Failed to create or use file: %d", ret);
            return ret;
        }

        LOG_INF("Created new active file: %s", ctx->current_filename);
    }

    return JUXTA_FRAMFS_OK;
}

int juxta_framfs_append_data(struct juxta_framfs_ctx *ctx,
                             const uint8_t *data,
                             size_t length)
{
    if (!ctx || !data || length == 0)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    /* Ensure correct file is active */
    int ret = juxta_framfs_ensure_current_file(ctx);
    if (ret < 0)
    {
        return ret;
    }

    /* Append data to active file */
    return juxta_framfs_append(ctx->fs_ctx, data, length);
}

int juxta_framfs_append_device_scan_data(struct juxta_framfs_ctx *ctx,
                                         uint16_t minute,
                                         uint8_t motion_count,
                                         uint8_t battery_level,
                                         int8_t temperature,
                                         const uint8_t (*mac_ids)[3],
                                         const int8_t *rssi_values,
                                         uint8_t device_count)
{
    if (!ctx)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    /* Ensure correct file is active */
    int ret = juxta_framfs_ensure_current_file(ctx);
    if (ret < 0)
    {
        return ret;
    }

    /* Append device scan to active file */
    return juxta_framfs_append_device_scan(ctx->fs_ctx, minute, motion_count,
                                           battery_level, temperature,
                                           mac_ids, rssi_values, device_count);
}

int juxta_framfs_append_simple_record_data(struct juxta_framfs_ctx *ctx,
                                           uint16_t minute,
                                           uint8_t type)
{
    if (!ctx)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    /* Ensure correct file is active */
    int ret = juxta_framfs_ensure_current_file(ctx);
    if (ret < 0)
    {
        return ret;
    }

    /* Append simple record to active file */
    return juxta_framfs_append_simple_record(ctx->fs_ctx, minute, type);
}

int juxta_framfs_get_current_filename(struct juxta_framfs_ctx *ctx,
                                      char *filename)
{
    if (!ctx || !filename)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    strncpy(filename, ctx->current_filename, JUXTA_FRAMFS_FILENAME_LEN);
    filename[JUXTA_FRAMFS_FILENAME_LEN - 1] = '\0';
    return JUXTA_FRAMFS_OK;
}

int juxta_framfs_advance_to_next_day(struct juxta_framfs_ctx *ctx)
{
    if (!ctx)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    /* Force a date check and file switch */
    uint32_t old_date = ctx->current_file_date;
    ctx->current_file_date = 0; /* Force update */

    int ret = juxta_framfs_ensure_current_file(ctx);
    if (ret < 0)
    {
        return ret;
    }

    if (ctx->current_file_date != old_date)
    {
        LOG_INF("Advanced to next day: %s", ctx->current_filename);
    }

    return JUXTA_FRAMFS_OK;
}

int juxta_framfs_get_memory_usage_percent(struct juxta_framfs_context *ctx,
                                          uint8_t *usage_percent)
{
    if (!ctx || !ctx->initialized || !usage_percent)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    /* Read fresh header from FRAM to get current usage */
    int ret = framfs_read_header(ctx);
    if (ret < 0)
    {
        LOG_ERR("Failed to read header for memory usage calculation: %d", ret);
        return ret;
    }

    /* Calculate usage percentage based on next_data_addr */
    /* next_data_addr points to the next available byte, so it represents used bytes */
    uint32_t used_bytes = ctx->header.next_data_addr;
    uint32_t total_bytes = JUXTA_FRAM_SIZE_BYTES;

    /* Calculate percentage (0-100) */
    uint32_t usage = (used_bytes * 100) / total_bytes;

    /* Cap at 100% */
    if (usage > 100)
    {
        usage = 100;
    }

    *usage_percent = (uint8_t)usage;

    LOG_DBG("📊 Memory usage: %u bytes / %u bytes = %u%%",
            (unsigned)used_bytes, (unsigned)total_bytes, (unsigned)usage);

    return JUXTA_FRAMFS_OK;
}

/* ========================================================================
 * ADC Burst Record Functions
 * ======================================================================== */

/* Note: Encode function removed - encoding is done directly in append function */

int juxta_framfs_decode_adc_burst_record(const uint8_t *buffer,
                                         size_t buffer_size,
                                         struct juxta_framfs_adc_burst_record *record)
{
    if (!buffer || !record || buffer_size < 12)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    /* Decode new 12-byte header format */
    uint32_t unix_timestamp = ((uint32_t)buffer[0] << 24) |
                              ((uint32_t)buffer[1] << 16) |
                              ((uint32_t)buffer[2] << 8) |
                              buffer[3];
    uint32_t microsecond_offset = ((uint32_t)buffer[4] << 24) |
                                  ((uint32_t)buffer[5] << 16) |
                                  ((uint32_t)buffer[6] << 8) |
                                  buffer[7];
    record->data_length = (buffer[8] << 8) | buffer[9];
    record->duration_us = (buffer[10] << 8) | buffer[11];

    /* Convert Unix timestamp + microsecond offset to start_time_us for compatibility */
    record->start_time_us = (unix_timestamp * 1000000) + microsecond_offset;

    /* Validate data length */
    if (record->data_length == 0)
    {
        LOG_WRN("Invalid data length: %d", record->data_length);
        return JUXTA_FRAMFS_ERROR;
    }

    /* Calculate required buffer size */
    size_t required_size = 12 + record->data_length;
    if (buffer_size < required_size)
    {
        LOG_WRN("Buffer too small: %zu < %zu", buffer_size, required_size);
        return JUXTA_FRAMFS_ERROR_SIZE;
    }

    /* Note: Data is available at buffer + 12, but we can't copy to flexible array member */
    /* Caller should access data directly from buffer + 12 */

    return (int)required_size;
}

int juxta_framfs_append_adc_burst_data(struct juxta_framfs_ctx *ctx,
                                       uint32_t unix_timestamp,
                                       uint32_t microsecond_offset,
                                       const uint8_t *samples,
                                       uint16_t sample_count,
                                       uint32_t duration_us)
{
    if (!ctx || !ctx->fs_ctx || !ctx->fs_ctx->initialized)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    if (!samples || sample_count == 0)
    {
        LOG_WRN("Invalid ADC burst parameters");
        return JUXTA_FRAMFS_ERROR;
    }

    /* Ensure correct file is active */
    int ret = juxta_framfs_ensure_current_file(ctx);
    if (ret < 0)
    {
        return ret;
    }

    /* Check active file */
    if (ctx->fs_ctx->active_file_index < 0)
    {
        LOG_WRN("No active file for append operation");
        return JUXTA_FRAMFS_ERROR_NO_ACTIVE;
    }

    /* Read current active file entry */
    struct juxta_framfs_entry entry;
    ret = framfs_read_entry(ctx->fs_ctx, ctx->fs_ctx->active_file_index, &entry);
    if (ret < 0)
    {
        LOG_ERR("Failed to read active file entry: %d", ret);
        return ret;
    }

    /* Verify it's still active */
    if (!(entry.flags & JUXTA_FRAMFS_FLAG_ACTIVE))
    {
        LOG_WRN("File is not active: %s", entry.filename);
        return JUXTA_FRAMFS_ERROR_READ_ONLY;
    }

    /* Calculate total record size and check FRAM bounds */
    uint32_t record_size = JUXTA_FRAMFS_ADC_HEADER_SIZE + sample_count; /* Extended header: 13 bytes */
    uint32_t write_addr = entry.start_addr + entry.length;
    if (write_addr + record_size > JUXTA_FRAM_SIZE_BYTES)
    {
        LOG_WRN("ADC burst would exceed FRAM size");
        return JUXTA_FRAMFS_ERROR_FULL;
    }

    LOG_DBG("📊 FRAMFS: Storing ADC burst - timestamp=%u, μs_offset=%u, samples=%u, duration=%u us",
            (unsigned)unix_timestamp, (unsigned)microsecond_offset,
            (unsigned)sample_count, (unsigned)duration_us);

    /* Prepare header (13 bytes) - New format: Unix timestamp + microsecond offset + sample count + duration + event type */
    uint8_t header[JUXTA_FRAMFS_ADC_HEADER_SIZE];
    header[0] = (unix_timestamp >> 24) & 0xFF;     /* Unix timestamp high byte */
    header[1] = (unix_timestamp >> 16) & 0xFF;     /* Unix timestamp mid-high byte */
    header[2] = (unix_timestamp >> 8) & 0xFF;      /* Unix timestamp mid-low byte */
    header[3] = unix_timestamp & 0xFF;             /* Unix timestamp low byte */
    header[4] = (microsecond_offset >> 24) & 0xFF; /* Microsecond offset high byte */
    header[5] = (microsecond_offset >> 16) & 0xFF; /* Microsecond offset mid-high byte */
    header[6] = (microsecond_offset >> 8) & 0xFF;  /* Microsecond offset mid-low byte */
    header[7] = microsecond_offset & 0xFF;         /* Microsecond offset low byte */
    header[8] = (sample_count >> 8) & 0xFF;        /* Sample count high byte */
    header[9] = sample_count & 0xFF;               /* Sample count low byte */
    /* Cap duration at 65535 µs (65.5ms) for 16-bit storage - overflow indicates timing issue */
    uint32_t clamped_duration = (duration_us > 65535) ? 65535 : duration_us;
    if (duration_us > 65535)
    {
        LOG_WRN("*** CRITICAL *** Duration overflow: %u µs capped to 65535 µs (indicates slow sampling)", (unsigned)duration_us);
    }
    header[10] = (clamped_duration >> 8) & 0xFF;     /* Duration high byte */
    header[11] = clamped_duration & 0xFF;            /* Duration low byte */
    header[12] = JUXTA_FRAMFS_ADC_EVENT_TIMER_BURST; /* Event type */

    /* Write header directly to FRAM */
    LOG_INF("📊 FRAMFS: Writing header to FRAM addr 0x%06X", (unsigned)write_addr);
    ret = juxta_fram_write(ctx->fs_ctx->fram_dev, write_addr, header, JUXTA_FRAMFS_ADC_HEADER_SIZE);
    if (ret < 0)
    {
        LOG_ERR("Failed to write ADC burst header to FRAM: %d", ret);
        return ret;
    }
    LOG_INF("📊 FRAMFS: Header written successfully");

    /* Write samples directly to FRAM (no intermediate buffer) */
    LOG_INF("📊 FRAMFS: Writing %u samples to FRAM addr 0x%06X",
            (unsigned)sample_count, (unsigned)(write_addr + JUXTA_FRAMFS_ADC_HEADER_SIZE));
    ret = juxta_fram_write(ctx->fs_ctx->fram_dev, write_addr + JUXTA_FRAMFS_ADC_HEADER_SIZE, samples, sample_count);
    if (ret < 0)
    {
        LOG_ERR("Failed to write ADC burst samples to FRAM: %d", ret);
        return ret;
    }
    LOG_INF("📊 FRAMFS: Samples written successfully");

    /* Update entry with new length */
    entry.length += record_size;
    ret = framfs_write_entry(ctx->fs_ctx, ctx->fs_ctx->active_file_index, &entry);
    if (ret < 0)
    {
        LOG_ERR("Failed to update file entry: %d", ret);
        return ret;
    }

    /* Update header statistics */
    ctx->fs_ctx->header.total_data_size += record_size;
    ctx->fs_ctx->header.next_data_addr = write_addr + record_size;
    ret = framfs_write_header(ctx->fs_ctx);
    if (ret < 0)
    {
        LOG_ERR("Failed to update header: %d", ret);
        return ret;
    }

    LOG_DBG("Appended ADC burst: %d samples, %d bytes to %s (total: %d bytes)",
            sample_count, record_size, entry.filename, entry.length);

    return JUXTA_FRAMFS_OK;
}

int juxta_framfs_append_adc_event_data(struct juxta_framfs_ctx *ctx,
                                       uint32_t unix_timestamp,
                                       uint32_t microsecond_offset,
                                       uint8_t event_type,
                                       const uint8_t *samples,
                                       uint16_t sample_count,
                                       uint32_t duration_us,
                                       uint8_t peak_positive,
                                       uint8_t peak_negative)
{
    if (!ctx || !ctx->fs_ctx || !ctx->fs_ctx->initialized)
    {
        return JUXTA_FRAMFS_ERROR;
    }

    /* Validate parameters based on event type */
    if (event_type == JUXTA_FRAMFS_ADC_EVENT_SINGLE_EVENT)
    {
        /* Single event mode: no samples, just peaks */
        if (samples != NULL || sample_count != 0)
        {
            LOG_WRN("Single event mode should not have samples");
            return JUXTA_FRAMFS_ERROR;
        }
    }
    else
    {
        /* Timer burst or peri-event: must have samples */
        if (!samples || sample_count == 0)
        {
            LOG_WRN("Timer burst/peri-event mode requires samples");
            return JUXTA_FRAMFS_ERROR;
        }
    }

    /* Ensure correct file is active */
    int ret = juxta_framfs_ensure_current_file(ctx);
    if (ret < 0)
    {
        return ret;
    }

    /* Check active file */
    if (ctx->fs_ctx->active_file_index < 0)
    {
        LOG_WRN("No active file for append operation");
        return JUXTA_FRAMFS_ERROR_NO_ACTIVE;
    }

    /* Read current active file entry */
    struct juxta_framfs_entry entry;
    ret = framfs_read_entry(ctx->fs_ctx, ctx->fs_ctx->active_file_index, &entry);
    if (ret < 0)
    {
        LOG_ERR("Failed to read active file entry: %d", ret);
        return ret;
    }

    /* Verify it's still active */
    if (!(entry.flags & JUXTA_FRAMFS_FLAG_ACTIVE))
    {
        LOG_WRN("File is not active: %s", entry.filename);
        return JUXTA_FRAMFS_ERROR_READ_ONLY;
    }

    /* Calculate record size based on event type */
    uint32_t record_size;
    if (event_type == JUXTA_FRAMFS_ADC_EVENT_SINGLE_EVENT)
    {
        record_size = JUXTA_FRAMFS_ADC_HEADER_SIZE + 3; /* 13-byte header + 2 peaks + 1 reserved */
    }
    else
    {
        record_size = JUXTA_FRAMFS_ADC_HEADER_SIZE + sample_count; /* 13-byte header + samples */
    }

    uint32_t write_addr = entry.start_addr + entry.length;
    if (write_addr + record_size > JUXTA_FRAM_SIZE_BYTES)
    {
        LOG_WRN("ADC event would exceed FRAM size");
        return JUXTA_FRAMFS_ERROR_FULL;
    }

    LOG_DBG("📊 FRAMFS: Storing ADC event - type=%u, timestamp=%u, μs_offset=%u, samples=%u, duration=%u us",
            event_type, (unsigned)unix_timestamp, (unsigned)microsecond_offset,
            (unsigned)sample_count, (unsigned)duration_us);

    /* Prepare header (13 bytes) - Same format for all event types */
    uint8_t header[JUXTA_FRAMFS_ADC_HEADER_SIZE];
    header[0] = (unix_timestamp >> 24) & 0xFF;
    header[1] = (unix_timestamp >> 16) & 0xFF;
    header[2] = (unix_timestamp >> 8) & 0xFF;
    header[3] = unix_timestamp & 0xFF;
    header[4] = (microsecond_offset >> 24) & 0xFF;
    header[5] = (microsecond_offset >> 16) & 0xFF;
    header[6] = (microsecond_offset >> 8) & 0xFF;
    header[7] = microsecond_offset & 0xFF;
    header[8] = (sample_count >> 8) & 0xFF;
    header[9] = sample_count & 0xFF;
    /* Cap duration at 65535 µs (65.5ms) for 16-bit storage - overflow indicates timing issue */
    uint32_t clamped_duration = (duration_us > 65535) ? 65535 : duration_us;
    if (duration_us > 65535)
    {
        LOG_WRN("*** CRITICAL *** Duration overflow: %u µs capped to 65535 µs (indicates slow sampling)", (unsigned)duration_us);
    }
    header[10] = (clamped_duration >> 8) & 0xFF;
    header[11] = clamped_duration & 0xFF;
    header[12] = event_type; /* Event type */

    /* Write header to FRAM */
    ret = juxta_fram_write(ctx->fs_ctx->fram_dev, write_addr, header, JUXTA_FRAMFS_ADC_HEADER_SIZE);
    if (ret < 0)
    {
        LOG_ERR("Failed to write ADC event header to FRAM: %d", ret);
        return ret;
    }

    /* Write event-specific data */
    if (event_type == JUXTA_FRAMFS_ADC_EVENT_SINGLE_EVENT)
    {
        /* Write peaks + reserved (event type now in header) */
        uint8_t event_data[3];
        event_data[0] = peak_positive;
        event_data[1] = peak_negative;
        event_data[2] = 0; /* Reserved */

        ret = juxta_fram_write(ctx->fs_ctx->fram_dev, write_addr + JUXTA_FRAMFS_ADC_HEADER_SIZE, event_data, 3);
        if (ret < 0)
        {
            LOG_ERR("Failed to write ADC event data to FRAM: %d", ret);
            return ret;
        }
    }
    else
    {
        /* Write samples for timer burst or peri-event */
        ret = juxta_fram_write(ctx->fs_ctx->fram_dev, write_addr + JUXTA_FRAMFS_ADC_HEADER_SIZE, samples, sample_count);
        if (ret < 0)
        {
            LOG_ERR("Failed to write ADC samples to FRAM: %d", ret);
            return ret;
        }
    }

    /* Update entry with new length */
    entry.length += record_size;
    ret = framfs_write_entry(ctx->fs_ctx, ctx->fs_ctx->active_file_index, &entry);
    if (ret < 0)
    {
        LOG_ERR("Failed to update file entry: %d", ret);
        return ret;
    }

    /* Update header statistics */
    ctx->fs_ctx->header.total_data_size += record_size;
    ctx->fs_ctx->header.next_data_addr = write_addr + record_size;
    ret = framfs_write_header(ctx->fs_ctx);
    if (ret < 0)
    {
        LOG_ERR("Failed to update header: %d", ret);
        return ret;
    }

    LOG_DBG("Appended ADC event: type=%u, %d bytes to %s (total: %d bytes)",
            event_type, record_size, entry.filename, entry.length);

    return JUXTA_FRAMFS_OK;
}

/* Legacy compatibility functions removed for now - focus on primary API */