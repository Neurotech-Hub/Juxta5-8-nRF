#include "juxta_log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "juxta_time.h"

/* DIAGNOSTIC BUILD: LOG_LEVEL_DBG so the flash_append / append entry-exit
 * brackets fire.  The module has no other LOG_DBG callsites, so raising the
 * level only enables the new bug-hunt instrumentation. */
LOG_MODULE_REGISTER(juxta_log, LOG_LEVEL_DBG);

#define FLASH_NODE DT_ALIAS(spi_mem)

#define JXS_START 0x000000U
#define JXS_SIZE 0x010000U
#define JXV_START 0x010000U
#define JXV_SIZE 0x100000U
#define JXB_START 0x110000U
#define JXB_SIZE 0x2F0000U

#define EOF_MARKER "#EOF\n"

struct log_region
{
	enum juxta_log_type type;
	const char *prefix;
	const char *header;
	uint32_t start;
	uint32_t size;
};

static const struct log_region regions[] = {
	{JUXTA_LOG_JXS, "JXS",
	 "unix,event,device_id,subject_id,experiment,fw_version,scan_interval_s,adv_interval_s,vitals_interval_s,ble_name\n",
	 JXS_START, JXS_SIZE},
	{JUXTA_LOG_JXV, "JXV", "unix,motion,batt_v,temp_c\n", JXV_START, JXV_SIZE},
	{JUXTA_LOG_JXB, "JXB", "unix,observer_id,peer_id,rssi\n", JXB_START, JXB_SIZE},
};

/* YYYYMMDD for which all three log types have been aligned (see touch_all_for_calendar_day). */
static char s_log_calendar_ymd[9];

/* Cached device identifier, populated by juxta_log_init().  ensure_file() uses
 * this to format the JXS `day_start` row on new-file creation without needing
 * to thread device_id through every vitals/BLE-observation append. */
static char s_device_id[JUXTA_DEVICE_ID_LEN];

static const struct log_region *region_for_type(enum juxta_log_type type)
{
	for (size_t i = 0; i < ARRAY_SIZE(regions); i++)
	{
		if (regions[i].type == type)
		{
			return &regions[i];
		}
	}
	return NULL;
}

static int flash_read_u8(const struct juxta_log_context *ctx, uint32_t off, uint8_t *value)
{
	return flash_read(ctx->flash, off, value, 1);
}

static int first_erased_offset(const struct juxta_log_context *ctx, const struct log_region *region,
							   uint32_t *offset)
{
	/* Static: this is called only from the main thread during init or
	 * from a single work-queue context during file rotation. */
	static uint8_t buf[256];
	uint32_t pos = region->start;
	uint32_t end = region->start + region->size;

	while (pos < end)
	{
		size_t len = MIN(sizeof(buf), end - pos);
		int rc = flash_read(ctx->flash, pos, buf, len);

		if (rc != 0)
		{
			return rc;
		}

		for (size_t i = 0; i < len; i++)
		{
			if (buf[i] == 0xFFU)
			{
				*offset = pos + i;
				return 0;
			}
		}

		pos += len;
	}

	*offset = end;
	return 0;
}

uint8_t juxta_log_memory_level_percent(const struct juxta_log_context *ctx)
{
	if (!ctx || !ctx->initialized || !ctx->flash)
	{
		return 0U;
	}

	uint64_t used = 0U;
	uint64_t cap = 0U;

	for (size_t i = 0; i < ARRAY_SIZE(regions); i++)
	{
		uint32_t off;
		int rc = first_erased_offset(ctx, &regions[i], &off);

		if (rc != 0)
		{
			return 0U;
		}
		if (off < regions[i].start)
		{
			continue;
		}
		used += (uint64_t)(off - regions[i].start);
		cap += (uint64_t)regions[i].size;
	}

	if (cap == 0U)
	{
		return 0U;
	}

	uint64_t pct = used * 100ULL / cap;

	return (pct > 100ULL) ? 100U : (uint8_t)pct;
}

static int flash_append(struct juxta_log_context *ctx, uint32_t off, const void *data, size_t len)
{
	const struct flash_parameters *params = flash_get_parameters(ctx->flash);
	size_t wbs = params ? params->write_block_size : 1U;
	const uint8_t *src = data;
	/* Static: avoids 256-byte stack allocation on every append call. */
	static uint8_t chunk[256];
	size_t done = 0;

	if (wbs == 0U)
	{
		wbs = 1U;
	}

	LOG_DBG("flash_append: enter off=0x%06x len=%u wbs=%u", off, (unsigned)len, (unsigned)wbs);

	while (done < len)
	{
		size_t payload = MIN(len - done, sizeof(chunk));
		size_t write_len = ROUND_UP(payload, wbs);

		if (write_len > sizeof(chunk))
		{
			payload = (sizeof(chunk) / wbs) * wbs;
			write_len = payload;
		}

		memset(chunk, 0xFF, sizeof(chunk));
		memcpy(chunk, src + done, payload);

		LOG_DBG("flash_append: write enter off=0x%06x wlen=%u (chunk %u/%u)",
				off + (uint32_t)done, (unsigned)write_len, (unsigned)done, (unsigned)len);
		int rc = flash_write(ctx->flash, off + done, chunk, write_len);
		LOG_DBG("flash_append: write exit rc=%d", rc);
		if (rc != 0)
		{
			LOG_DBG("flash_append: exit rc=%d (early, after %u of %u)",
					rc, (unsigned)done, (unsigned)len);
			return rc;
		}

		done += payload;
	}

	LOG_DBG("flash_append: exit rc=0 (wrote %u)", (unsigned)len);
	return 0;
}

static int add_file_entry(struct juxta_log_context *ctx, const char *path, uint32_t offset,
						  uint32_t length, enum juxta_log_type type, bool active)
{
	if (ctx->file_count >= JUXTA_MAX_FILES)
	{
		return -ENOSPC;
	}

	struct juxta_file_entry *entry = &ctx->files[ctx->file_count++];
	memset(entry, 0, sizeof(*entry));
	(void)snprintf(entry->path, sizeof(entry->path), "%s", path);
	entry->offset = offset;
	entry->length = length;
	entry->type = type;
	entry->active = active;

	switch (type)
	{
	case JUXTA_LOG_JXS:
		ctx->active_jxs = ctx->file_count - 1U;
		break;
	case JUXTA_LOG_JXV:
		ctx->active_jxv = ctx->file_count - 1U;
		break;
	case JUXTA_LOG_JXB:
		ctx->active_jxb = ctx->file_count - 1U;
		break;
	default:
		break;
	}

	return 0;
}

static void cache_from_context(const struct juxta_log_context *ctx, struct juxta_log_cache *cache)
{
	memset(cache, 0, sizeof(*cache));
	cache->file_count = ctx->file_count;
	for (uint16_t i = 0; i < ctx->file_count && i < JUXTA_MAX_FILES; i++)
	{
		(void)strncpy(cache->files[i].path, ctx->files[i].path,
					  sizeof(cache->files[i].path) - 1U);
		cache->files[i].path[sizeof(cache->files[i].path) - 1U] = '\0';
		cache->files[i].offset = ctx->files[i].offset;
		cache->files[i].length = ctx->files[i].length;
		cache->files[i].log_type = (uint8_t)ctx->files[i].type;
		cache->files[i].active = ctx->files[i].active;
	}
}

static void context_from_cache(struct juxta_log_context *ctx, const struct juxta_log_cache *cache)
{
	ctx->file_count = 0;
	ctx->active_jxs = UINT32_MAX;
	ctx->active_jxv = UINT32_MAX;
	ctx->active_jxb = UINT32_MAX;

	for (uint16_t i = 0; i < cache->file_count && i < JUXTA_MAX_FILES; i++)
	{
		(void)add_file_entry(ctx, cache->files[i].path, cache->files[i].offset,
							 cache->files[i].length,
							 (enum juxta_log_type)cache->files[i].log_type,
							 cache->files[i].active);
	}
}

static uint32_t *active_slot_for_type(struct juxta_log_context *ctx, enum juxta_log_type type)
{
	switch (type)
	{
	case JUXTA_LOG_JXS:
		return &ctx->active_jxs;
	case JUXTA_LOG_JXV:
		return &ctx->active_jxv;
	case JUXTA_LOG_JXB:
		return &ctx->active_jxb;
	default:
		return NULL;
	}
}

static int read_line(struct juxta_log_context *ctx, uint32_t off, uint32_t end, char *line,
					 size_t line_size, uint32_t *next_off)
{
	size_t used = 0;

	while (off < end && used + 1U < line_size)
	{
		uint8_t ch;
		int rc = flash_read_u8(ctx, off++, &ch);

		if (rc != 0)
		{
			return rc;
		}
		if (ch == 0xFFU)
		{
			return -ENOENT;
		}
		if (ch == '\n')
		{
			line[used] = '\0';
			*next_off = off;
			return 0;
		}
		line[used++] = (char)ch;
	}

	return -ENOSPC;
}

static int find_eof_or_erased(struct juxta_log_context *ctx, uint32_t off, uint32_t end,
							  uint32_t *file_end, bool *closed)
{
	const char marker[] = EOF_MARKER;
	size_t matched = 0;

	while (off < end)
	{
		uint8_t ch;
		int rc = flash_read_u8(ctx, off, &ch);

		if (rc != 0)
		{
			return rc;
		}
		if (ch == 0xFFU)
		{
			*file_end = off;
			*closed = false;
			return 0;
		}

		if (ch == (uint8_t)marker[matched])
		{
			matched++;
			if (matched == strlen(marker))
			{
				*file_end = off + 1U;
				*closed = true;
				return 0;
			}
		}
		else
		{
			matched = (ch == (uint8_t)marker[0]) ? 1U : 0U;
		}

		off++;
	}

	*file_end = end;
	*closed = false;
	return 0;
}

static int recover_region(struct juxta_log_context *ctx, const struct log_region *region)
{
	uint32_t pos = region->start;
	uint32_t end = region->start + region->size;

	while (pos < end)
	{
		uint8_t first;
		char filename[JUXTA_FILE_NAME_LEN];
		uint32_t after_name;
		uint32_t file_end;
		bool closed;
		int rc = flash_read_u8(ctx, pos, &first);

		if (rc != 0)
		{
			return rc;
		}
		if (first == 0xFFU)
		{
			return 0;
		}

		rc = read_line(ctx, pos, end, filename, sizeof(filename), &after_name);
		if (rc != 0)
		{
			LOG_WRN("Failed to read filename at 0x%06x: %d", pos, rc);
			return rc;
		}
		if (strncmp(filename, region->prefix, 3) != 0)
		{
			/* Non-CSV data at this position — region is unformatted or
			 * contains leftover data (e.g. from a destructive flash test).
			 * Signal to the caller that the region needs erasing. */
			LOG_WRN("Region %s at 0x%06x has unrecognised data — needs format",
					region->prefix, pos);
			return -ENODATA;
		}

		rc = find_eof_or_erased(ctx, after_name, end, &file_end, &closed);
		if (rc != 0)
		{
			return rc;
		}

		rc = add_file_entry(ctx, filename, pos, file_end - pos, region->type, !closed);
		if (rc != 0)
		{
			return rc;
		}

		if (!closed)
		{
			return 0;
		}
		pos = file_end;
	}

	return 0;
}

int juxta_log_recover_files(struct juxta_log_context *ctx)
{
	if (!ctx || !ctx->flash)
	{
		return -EINVAL;
	}

	ctx->file_count = 0;
	ctx->active_jxs = UINT32_MAX;
	ctx->active_jxv = UINT32_MAX;
	ctx->active_jxb = UINT32_MAX;

	for (size_t i = 0; i < ARRAY_SIZE(regions); i++)
	{
		int rc = recover_region(ctx, &regions[i]);

		if (rc != 0)
		{
			return rc;
		}
	}

	struct juxta_log_cache cache;
	cache_from_context(ctx, &cache);
	(void)juxta_settings_save_log_cache(&cache);
	LOG_INF("Recovered %u NOR CSV pseudo-files", ctx->file_count);
	return 0;
}

/* On new JXS pseudo-file creation, emit a single `day_start` row carrying the
 * current NVS snapshot via the JXS schema columns (subject_id, experiment,
 * intervals, ble_name).  Keeps each day interpretable from JXS alone without
 * pulling history from prior days, and preserves the strict CSV contract
 * (single header, all data rows) that the previous JXV `#device_settings`
 * comment lines broke. */
static int append_jxs_day_start_row(struct juxta_log_context *ctx,
				    const struct juxta_settings *settings,
				    uint32_t unix_time)
{
	struct juxta_file_entry *entry;
	static char row[256];
	int len;
	int rc;

	if (!ctx || !settings || ctx->active_jxs == UINT32_MAX ||
	    ctx->active_jxs >= ctx->file_count)
	{
		return -EINVAL;
	}

	/* Defensive: if the caller forgot to populate device_id at init, skip
	 * the row rather than emit a malformed CSV. */
	if (s_device_id[0] == '\0')
	{
		return 0;
	}

	entry = &ctx->files[ctx->active_jxs];
	if (entry->type != JUXTA_LOG_JXS)
	{
		return -EINVAL;
	}

	len = snprintf(row, sizeof(row), "%u,day_start,%s,%s,%s,%s,%u,%u,%u,%s\n",
		       unix_time, s_device_id, settings->subject_id, settings->experiment,
		       JUXTA_FIRMWARE_VERSION, settings->scan_interval_s,
		       settings->adv_interval_s, settings->vitals_interval_s, s_device_id);
	if (len < 0 || len >= (int)sizeof(row))
	{
		return -ENOSPC;
	}

	rc = flash_append(ctx, entry->offset + entry->length, row, (size_t)len);
	if (rc != 0)
	{
		return rc;
	}

	entry->length += (uint32_t)len;
	return 0;
}

static int ensure_file(struct juxta_log_context *ctx, enum juxta_log_type type,
					   const struct juxta_settings *settings, uint32_t unix_time)
{
	const struct log_region *region = region_for_type(type);
	uint32_t *active_slot = active_slot_for_type(ctx, type);
	static char date[9];
	static char filename[JUXTA_FILE_NAME_LEN];
	static char prefix[4];

	if (!region || !active_slot || !settings)
	{
		return -EINVAL;
	}

	/* Clock not yet set — keep using the existing active file if there is one,
	 * or skip creation entirely.  Rows with unix_time == 0 are meaningless. */
	if (unix_time == 0U)
	{
		return 0;
	}

	juxta_time_date_string(unix_time, date);
	(void)snprintf(prefix, sizeof(prefix), "%s", region->prefix);
	(void)snprintf(filename, sizeof(filename), "%s%s.csv", prefix, date);

	if (*active_slot != UINT32_MAX && *active_slot < ctx->file_count &&
		strcmp(ctx->files[*active_slot].path, filename) == 0)
	{
		return 0;
	}

	if (*active_slot != UINT32_MAX && *active_slot < ctx->file_count &&
		ctx->files[*active_slot].active)
	{
		struct juxta_file_entry *old = &ctx->files[*active_slot];
		int rc = flash_append(ctx, old->offset + old->length, EOF_MARKER,
							  strlen(EOF_MARKER));

		if (rc != 0)
		{
			return rc;
		}
		old->length += strlen(EOF_MARKER);
		old->active = false;
	}

	uint32_t off;
	int rc = first_erased_offset(ctx, region, &off);
	if (rc != 0)
	{
		return rc;
	}
	if (off >= region->start + region->size)
	{
		return -ENOSPC;
	}

	static char header[160];
	int header_len = snprintf(header, sizeof(header), "%s\n%s", filename, region->header);
	if (header_len < 0 || header_len >= (int)sizeof(header))
	{
		return -ENOSPC;
	}

	rc = flash_append(ctx, off, header, (size_t)header_len);
	if (rc != 0)
	{
		return rc;
	}

	rc = add_file_entry(ctx, filename, off, (uint32_t)header_len, type, true);
	if (rc != 0)
	{
		return rc;
	}

	if (type == JUXTA_LOG_JXS)
	{
		rc = append_jxs_day_start_row(ctx, settings, unix_time);
		if (rc != 0)
		{
			return rc;
		}
	}

	struct juxta_log_cache cache;
	cache_from_context(ctx, &cache);
	(void)juxta_settings_save_log_cache(&cache);
	LOG_INF("Created %s at 0x%06x", filename, off);
	return 0;
}

/* On calendar day change, open JXS/JXV/JXB for the new day (header only is OK)
 * so the gateway always sees three dated files per day. */
static int touch_all_for_calendar_day(struct juxta_log_context *ctx,
				      const struct juxta_settings *settings,
				      uint32_t unix_time)
{
	char today[9];
	int rc;

	if (!ctx || !ctx->initialized || !settings || unix_time == 0U)
	{
		return 0;
	}

	juxta_time_date_string(unix_time, today);
	if (s_log_calendar_ymd[0] != '\0' && strcmp(s_log_calendar_ymd, today) == 0)
	{
		return 0;
	}

	rc = ensure_file(ctx, JUXTA_LOG_JXS, settings, unix_time);
	if (rc != 0)
	{
		return rc;
	}
	rc = ensure_file(ctx, JUXTA_LOG_JXV, settings, unix_time);
	if (rc != 0)
	{
		return rc;
	}
	rc = ensure_file(ctx, JUXTA_LOG_JXB, settings, unix_time);
	if (rc != 0)
	{
		return rc;
	}

	juxta_time_date_string(unix_time, s_log_calendar_ymd);
	LOG_INF("Daily log files for %s (JXS/JXV/JXB)", s_log_calendar_ymd);
	return 0;
}

static int append_row(struct juxta_log_context *ctx, enum juxta_log_type type, const char *row)
{
	uint32_t *slot = active_slot_for_type(ctx, type);
	struct juxta_file_entry *entry;
	int rc;

	if (!slot || *slot == UINT32_MAX || *slot >= ctx->file_count)
	{
		return -ENOENT;
	}

	entry = &ctx->files[*slot];
	rc = flash_append(ctx, entry->offset + entry->length, row, strlen(row));
	if (rc != 0)
	{
		return rc;
	}

	entry->length += strlen(row);
	/* Do NOT save the NVS log cache here.  The NVS sectors are 4 KB each
	 * and the cache struct is ~520 bytes, so a sector fills after ~7 writes.
	 * At one JXB row every 20 s that means an erase cycle every ~2.5 min —
	 * the nRF52840 internal flash (10 k cycles) would fail in ~17 days.
	 * Instead the cache is saved only in ensure_file() on file
	 * creation/rotation (~once per calendar day per type).  juxta_log_init()
	 * reconciles stale cached lengths by scanning each active file with
	 * find_eof_or_erased() on every boot. */
	return 0;
}

int juxta_log_init(struct juxta_log_context *ctx, const struct juxta_settings *settings,
				   const char *device_id)
{
	struct juxta_log_cache cache;
	int rc;

	if (!ctx || !settings)
	{
		return -EINVAL;
	}

	/* Cache device_id so ensure_file() can format the JXS `day_start` row
	 * on new-file creation without a new parameter on the vitals/BLE-observation
	 * append paths.  Tolerate a NULL device_id (defensive — the field is
	 * blanked and the day_start row is skipped if it ever happens). */
	s_device_id[0] = '\0';
	if (device_id != NULL)
	{
		(void)snprintf(s_device_id, sizeof(s_device_id), "%s", device_id);
	}

	memset(ctx, 0, sizeof(*ctx));
	ctx->active_jxs = UINT32_MAX;
	ctx->active_jxv = UINT32_MAX;
	ctx->active_jxb = UINT32_MAX;
	ctx->flash = DEVICE_DT_GET(FLASH_NODE);

	if (!device_is_ready(ctx->flash))
	{
		LOG_ERR("SPI NOR is not ready");
		return -ENODEV;
	}

	uint32_t now = juxta_time_now();
	bool need_format = false;

	rc = juxta_settings_load_log_cache(&cache);
	if (rc == 0)
	{
		context_from_cache(ctx, &cache);
		LOG_INF("Loaded %u cached NOR CSV pseudo-files", ctx->file_count);

		/* The NVS cache stores the file length only at file
		 * creation/rotation — never on individual row appends.  After a
		 * reboot on the same calendar day the cached length points back
		 * to the header, and append_row would write at that stale offset
		 * (into already-written NOR bytes that are silently ignored).
		 * Scan each active file with find_eof_or_erased so entry->length
		 * reflects every row that was written in previous sessions. */
		for (uint16_t i = 0; i < ctx->file_count; i++)
		{
			struct juxta_file_entry *e = &ctx->files[i];

			if (!e->active)
			{
				continue;
			}
			const struct log_region *rgn = region_for_type(e->type);

			if (!rgn)
			{
				continue;
			}
			uint32_t actual_end;
			bool closed;
			int scan_rc = find_eof_or_erased(ctx, e->offset,
											 rgn->start + rgn->size,
											 &actual_end, &closed);

			if (scan_rc == 0)
			{
				uint32_t actual_len = actual_end - e->offset;

				if (actual_len != e->length)
				{
					LOG_INF("Reconciled %s length %u → %u bytes",
							e->path, e->length, actual_len);
					e->length = actual_len;
				}
			}
		}
	}
	else
	{
		rc = juxta_log_recover_files(ctx);
		if (rc != 0)
		{
			/* -ENODATA: unformatted / stale data. Other errors: I/O problem.
			 * In both cases we must erase before writing CSV headers. */
			LOG_WRN("NOR recovery failed (%d) — erasing all regions for fresh format",
					rc);
			need_format = true;
		}
		else if (ctx->file_count == 0)
		{
			/* Fully erased NOR — no files found, no error. */
			LOG_INF("NOR is blank — creating initial CSV files");
		}
	}

	if (need_format)
	{
		/* Erase all regions; file creation is deferred until time is valid
		 * and production init writes the first data (same as the normal path). */
		ctx->initialized = true;
		rc = juxta_log_format(ctx);
		return rc;
	}

	ctx->initialized = true;

	/* Skip file creation and the boot event when the clock is not yet set
	 * (unix_time == 0).  ensure_file() will create properly-dated files on
	 * the first append that carries a valid timestamp.  The boot event is
	 * logged by the caller (main.c) after datetime sync completes. */
	if (now == 0U)
	{
		LOG_INF("Clock not set — deferring NOR file creation until time sync");
		return 0;
	}

	rc = ensure_file(ctx, JUXTA_LOG_JXS, settings, now);
	if (rc != 0)
	{
		return rc;
	}
	rc = ensure_file(ctx, JUXTA_LOG_JXV, settings, now);
	if (rc != 0)
	{
		return rc;
	}
	rc = ensure_file(ctx, JUXTA_LOG_JXB, settings, now);
	if (rc != 0)
	{
		return rc;
	}

	juxta_time_date_string(now, s_log_calendar_ymd);

	return juxta_log_append_event(ctx, settings, device_id, "boot", now);
}

static int erase_region(struct juxta_log_context *ctx, const struct log_region *region)
{
	struct flash_pages_info page;
	uint32_t pos = region->start;
	uint32_t end = region->start + region->size;

	while (pos < end)
	{
		int rc = flash_get_page_info_by_offs(ctx->flash, pos, &page);

		if (rc != 0)
		{
			return rc;
		}

		rc = flash_erase(ctx->flash, page.start_offset, page.size);
		if (rc != 0)
		{
			return rc;
		}
		pos = page.start_offset + page.size;
	}

	return 0;
}

int juxta_log_format(struct juxta_log_context *ctx)
{
	/* Erase all NOR CSV regions and reset in-memory state only.
	 * File creation and provenance event logging are deferred to the caller
	 * so that clearMemory during the sync phase does not bypass the
	 * ProductionInit file-creation gate. */
	if (!ctx || !ctx->flash)
	{
		return -EINVAL;
	}

	for (size_t i = 0; i < ARRAY_SIZE(regions); i++)
	{
		int rc = erase_region(ctx, &regions[i]);

		if (rc != 0)
		{
			LOG_ERR("Failed to erase region %s: %d", regions[i].prefix, rc);
			return rc;
		}
	}

	(void)juxta_settings_clear_log_cache();
	ctx->file_count = 0;
	ctx->active_jxs = UINT32_MAX;
	ctx->active_jxv = UINT32_MAX;
	ctx->active_jxb = UINT32_MAX;
	s_log_calendar_ymd[0] = '\0';

	LOG_INF("NOR CSV regions erased — file creation deferred to next data write");
	return 0;
}

int juxta_log_append_event(struct juxta_log_context *ctx, const struct juxta_settings *settings,
						   const char *device_id, const char *event, uint32_t unix_time)
{
	static char row[256];

	if (!ctx || !settings || !device_id || !event)
	{
		return -EINVAL;
	}

	LOG_DBG("append_event: enter t=%u event=%s", unix_time, event);

	int rc = touch_all_for_calendar_day(ctx, settings, unix_time);
	if (rc != 0)
	{
		LOG_DBG("append_event: exit rc=%d (touch_all)", rc);
		return rc;
	}

	rc = ensure_file(ctx, JUXTA_LOG_JXS, settings, unix_time);
	if (rc != 0)
	{
		LOG_DBG("append_event: exit rc=%d (ensure_file)", rc);
		return rc;
	}

	int len = snprintf(row, sizeof(row), "%u,%s,%s,%s,%s,%s,%u,%u,%u,%s\n", unix_time,
					   event, device_id, settings->subject_id, settings->experiment,
					   JUXTA_FIRMWARE_VERSION, settings->scan_interval_s,
					   settings->adv_interval_s, settings->vitals_interval_s, device_id);
	if (len < 0 || len >= (int)sizeof(row))
	{
		LOG_DBG("append_event: exit rc=-ENOSPC (snprintf)");
		return -ENOSPC;
	}

	LOG_INF("JXS[%u] %s", unix_time, event);
	rc = append_row(ctx, JUXTA_LOG_JXS, row);
	LOG_DBG("append_event: exit rc=%d", rc);
	return rc;
}

int juxta_log_append_vitals(struct juxta_log_context *ctx, uint32_t unix_time, uint16_t motion,
							int32_t batt_mv, int8_t temp_c)
{
	static char row[96];
	int volts = batt_mv / 1000;
	int centivolts = (batt_mv % 1000) / 10;
	int len;

	if (!ctx)
	{
		return -EINVAL;
	}

	LOG_DBG("append_vitals: enter t=%u motion=%u", unix_time, motion);

	int rc = touch_all_for_calendar_day(ctx, juxta_settings_get(), unix_time);
	if (rc != 0)
	{
		LOG_DBG("append_vitals: exit rc=%d (touch_all)", rc);
		return rc;
	}

	rc = ensure_file(ctx, JUXTA_LOG_JXV, juxta_settings_get(), unix_time);
	if (rc != 0)
	{
		LOG_DBG("append_vitals: exit rc=%d (ensure_file)", rc);
		return rc;
	}

	len = snprintf(row, sizeof(row), "%u,%u,%d.%02d,%d\n", unix_time, motion, volts,
				   centivolts, temp_c);
	if (len < 0 || len >= (int)sizeof(row))
	{
		LOG_DBG("append_vitals: exit rc=-ENOSPC (snprintf)");
		return -ENOSPC;
	}

	LOG_INF("JXV[%u] motion=%u batt=%d.%02dV temp=%d", unix_time, motion, volts,
			centivolts, temp_c);
	rc = append_row(ctx, JUXTA_LOG_JXV, row);
	LOG_DBG("append_vitals: exit rc=%d", rc);
	return rc;
}

int juxta_log_append_ble_observation(struct juxta_log_context *ctx, uint32_t unix_time,
									 const char *observer_id, const char *peer_id, int8_t rssi)
{
	static char row[96];

	if (!ctx || !observer_id || !peer_id)
	{
		return -EINVAL;
	}

	LOG_DBG("append_ble: enter t=%u peer=%s", unix_time, peer_id);

	int rc = touch_all_for_calendar_day(ctx, juxta_settings_get(), unix_time);
	if (rc != 0)
	{
		LOG_DBG("append_ble: exit rc=%d (touch_all)", rc);
		return rc;
	}

	rc = ensure_file(ctx, JUXTA_LOG_JXB, juxta_settings_get(), unix_time);
	if (rc != 0)
	{
		LOG_DBG("append_ble: exit rc=%d (ensure_file)", rc);
		return rc;
	}

	int len = snprintf(row, sizeof(row), "%u,%s,%s,%d\n", unix_time, observer_id, peer_id,
					   rssi);
	if (len < 0 || len >= (int)sizeof(row))
	{
		LOG_DBG("append_ble: exit rc=-ENOSPC (snprintf)");
		return -ENOSPC;
	}

	LOG_INF("JXB[%u] %s rssi=%d", unix_time, peer_id, rssi);
	rc = append_row(ctx, JUXTA_LOG_JXB, row);
	LOG_DBG("append_ble: exit rc=%d", rc);
	return rc;
}

/* Bytes streamed over BLE (after filename line; closed files omit trailing #EOF). */
uint32_t juxta_log_transfer_payload_bytes(const struct juxta_file_entry *entry)
{
	uint32_t skip = (uint32_t)strlen(entry->path) + 1U;

	if (entry->length <= skip)
	{
		return 0U;
	}
	uint32_t payload = entry->length - skip;

	if (!entry->active && payload >= (uint32_t)strlen(EOF_MARKER))
	{
		payload -= (uint32_t)strlen(EOF_MARKER);
	}
	return payload;
}

int juxta_log_list_files(struct juxta_log_context *ctx, char *buffer, size_t buffer_size)
{
	size_t written = 0;

	if (!ctx || !buffer || buffer_size == 0U)
	{
		return -EINVAL;
	}

	/* Format: "name|size;name|size;EOF"
	 * Each entry carries a trailing semicolon; "EOF" is appended with no
	 * leading semicolon.  This matches the legacy juxta-ble wire format
	 * that the iOS companion app uses as a termination sentinel. */
	buffer[0] = '\0';
	for (uint16_t i = 0; i < ctx->file_count; i++)
	{
		const struct juxta_file_entry *entry = &ctx->files[i];
		uint32_t visible_len = juxta_log_transfer_payload_bytes(entry);
		int len = snprintf(buffer + written, buffer_size - written, "%s|%u;",
						   entry->path, visible_len);

		if (len < 0 || (size_t)len >= buffer_size - written)
		{
			return -ENOSPC;
		}
		written += (size_t)len;
	}

	/* Append terminal "EOF" token. */
	if (written + 3U >= buffer_size)
	{
		return -ENOSPC;
	}
	memcpy(buffer + written, "EOF", 4U); /* includes null terminator */
	written += 3U;

	return (int)written;
}

int juxta_log_find_file(struct juxta_log_context *ctx, const char *path,
						struct juxta_file_entry *entry)
{
	if (!ctx || !path || !entry)
	{
		return -EINVAL;
	}

	for (uint16_t i = 0; i < ctx->file_count; i++)
	{
		if (strcmp(ctx->files[i].path, path) == 0)
		{
			*entry = ctx->files[i];
			return 0;
		}
	}

	return -ENOENT;
}

int juxta_log_read_file(struct juxta_log_context *ctx, const struct juxta_file_entry *entry,
						uint32_t file_offset, uint8_t *buffer, size_t buffer_size,
						size_t *bytes_read)
{
	uint32_t visible_len;

	if (!ctx || !entry || !buffer || !bytes_read)
	{
		return -EINVAL;
	}

	visible_len = entry->length + (entry->active ? (uint32_t)strlen(EOF_MARKER) : 0U);
	if (file_offset >= visible_len)
	{
		*bytes_read = 0;
		return 0;
	}

	size_t to_read = MIN(buffer_size, visible_len - file_offset);
	if (file_offset < entry->length)
	{
		size_t physical = MIN(to_read, entry->length - file_offset);
		int rc = flash_read(ctx->flash, entry->offset + file_offset, buffer, physical);

		if (rc != 0)
		{
			return rc;
		}

		if (physical < to_read)
		{
			memcpy(buffer + physical, EOF_MARKER, to_read - physical);
		}
	}
	else
	{
		uint32_t eof_offset = file_offset - entry->length;

		memcpy(buffer, EOF_MARKER + eof_offset, to_read);
	}

	*bytes_read = to_read;
	return 0;
}

int juxta_log_read_file_for_transfer(struct juxta_log_context *ctx,
									 const struct juxta_file_entry *entry, uint32_t file_offset,
									 uint8_t *buffer, size_t buffer_size, size_t *bytes_read)
{
	uint32_t payload;
	uint32_t flash_abs;

	if (!ctx || !entry || !buffer || !bytes_read)
	{
		return -EINVAL;
	}

	payload = juxta_log_transfer_payload_bytes(entry);
	if (file_offset >= payload)
	{
		*bytes_read = 0;
		return 0;
	}

	size_t to_read = MIN(buffer_size, (size_t)(payload - file_offset));
	uint32_t skip = (uint32_t)strlen(entry->path) + 1U;

	flash_abs = entry->offset + skip + file_offset;

	int rc = flash_read(ctx->flash, flash_abs, buffer, to_read);

	if (rc != 0)
	{
		return rc;
	}
	*bytes_read = to_read;
	return 0;
}
