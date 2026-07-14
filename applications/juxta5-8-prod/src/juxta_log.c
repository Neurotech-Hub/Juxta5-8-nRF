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

/* Production: LOG_LEVEL_INF.  The per-chunk DBG instrumentation in
 * flash_append produced ~one log line per 256 B of NOR write, which under
 * CONFIG_LOG_MODE_IMMEDIATE + the previous BLOCK_IF_FIFO_FULL RTT mode
 * could stall the BT RX / system workqueue thread for hundreds of ms per
 * burst (and indefinitely with no host attached).  Raise temporarily to
 * LOG_LEVEL_DBG only when actively chasing a NOR-append issue. */
LOG_MODULE_REGISTER(juxta_log, LOG_LEVEL_INF);

#define FLASH_NODE DT_ALIAS(spi_mem)

/* The last 64 KB of the 4 MB NOR (0x3F0000–0x3FFFFF) is the production
 * checkpoint ring (juxta_checkpoint.c) — carved from the tail of JXB in
 * fw 5.8.4.  Keep JXB_SIZE in sync with RING_START/RING_SIZE there. */
#define JXS_START 0x000000U
#define JXS_SIZE 0x010000U
#define JXV_START 0x010000U
#define JXV_SIZE 0x100000U
#define JXB_START 0x110000U
#define JXB_SIZE 0x2E0000U

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

/* Long-op tick hook (see juxta_log_set_long_op_tick in juxta_log.h). */
static void (*s_long_op_tick)(void);

void juxta_log_set_long_op_tick(void (*tick)(void))
{
	s_long_op_tick = tick;
}

/* Single recursive-style mutex guarding every public juxta_log_* entry
 * point.  Zephyr's k_mutex is already recursive (counts re-locks by the
 * owning thread), so juxta_log_init -> juxta_log_format -> erase_region
 * paths self-nest safely.  Production is non-connectable between sync sessions,
 * so BT RX does not touch the log concurrently with the system workqueue;
 * the mutex still guards against any future path where two threads could reach
 * a juxta_log_* call and tear shared static buffers (chunk[], row[], header[],
 * find_eof_or_erased buf[]) or ctx->files[]/file_count mutations. */
K_MUTEX_DEFINE(s_log_mutex);

#define LOG_LOCK() (void)k_mutex_lock(&s_log_mutex, K_FOREVER)
#define LOG_UNLOCK() (void)k_mutex_unlock(&s_log_mutex)

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

/* Index into `regions[]` (and into ctx->region_full[]) for a log type.
 * Returns -1 for an unknown type; callers that have already validated
 * the type via region_for_type() can treat that case as defensive. */
static int region_index_for_type(enum juxta_log_type type)
{
	for (size_t i = 0; i < ARRAY_SIZE(regions); i++)
	{
		if (regions[i].type == type)
		{
			return (int)i;
		}
	}
	return -1;
}

/* One-shot WARN when the file-slot table fills up.  Without this guard the
 * ensure_file() short-circuit below would be silent forever.  Cleared by
 * juxta_log_format() so a clearMemory rearms the warning. */
static bool s_file_count_cap_warned;

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

/* Worst-case per-region utilisation.  The previous implementation summed
 * `used`/`cap` across all three regions, but because JXB occupies ~75 % of
 * the chip a fully-saturated JXV or JXS only nudged the aggregate by a few
 * points — the gateway never saw a meaningful "full" warning before data
 * started being dropped.  Returning the max means a single region hitting
 * 100 % surfaces immediately in the Node JSON, matching the user-visible
 * "memory full" semantics expected by the iOS app. */
uint8_t juxta_log_memory_level_percent(const struct juxta_log_context *ctx)
{
	if (!ctx || !ctx->initialized || !ctx->flash)
	{
		return 0U;
	}

	LOG_LOCK();

	uint8_t worst = 0U;

	for (size_t i = 0; i < ARRAY_SIZE(regions); i++)
	{
		/* A latched region is, by definition, full from the gateway's
		 * perspective even if the very last bytes happen to be 0xFF. */
		if (ctx->region_full[i])
		{
			worst = 100U;
			continue;
		}

		uint32_t off;
		int rc = first_erased_offset(ctx, &regions[i], &off);

		if (rc != 0)
		{
			continue;
		}
		if (off < regions[i].start || regions[i].size == 0U)
		{
			continue;
		}

		uint64_t used = (uint64_t)(off - regions[i].start);
		uint64_t pct = used * 100ULL / (uint64_t)regions[i].size;
		uint8_t p = (pct > 100ULL) ? 100U : (uint8_t)pct;

		if (p > worst)
		{
			worst = p;
		}
	}

	LOG_UNLOCK();
	return worst;
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

		int rc = flash_write(ctx->flash, off + done, chunk, write_len);
		if (rc != 0)
		{
			return rc;
		}

		done += payload;
	}

	return 0;
}

/* Bounds-checked wrapper around flash_append.  Returns -ENOSPC (without
 * touching the flash) when:
 *   • the region's full-latch is already set, OR
 *   • the write would cross the region's end boundary.
 * In the boundary case the latch is set as a side effect so subsequent
 * appends to the same region short-circuit without re-running the bounds
 * arithmetic.  This is the single chokepoint that enforces the "stop
 * writing when full" invariant for every NOR-touching path: append_row,
 * append_jxs_day_start_row, the #EOF close in ensure_file, and the header
 * write in ensure_file. */
static int guarded_flash_append(struct juxta_log_context *ctx,
				const struct log_region *region, int region_idx,
				uint32_t off, const void *data, size_t len)
{
	if (!region || region_idx < 0 || region_idx >= (int)ARRAY_SIZE(regions))
	{
		return -EINVAL;
	}
	if (ctx->region_full[region_idx])
	{
		return -ENOSPC;
	}

	uint32_t region_end = region->start + region->size;

	if (off < region->start || off > region_end || (uint64_t)off + (uint64_t)len > (uint64_t)region_end)
	{
		if (!ctx->region_full[region_idx])
		{
			LOG_WRN("Region %s full: would write %u bytes at 0x%06x (end 0x%06x); latching",
				region->prefix, (unsigned)len, off, region_end);
		}
		ctx->region_full[region_idx] = true;
		return -ENOSPC;
	}

	return flash_append(ctx, off, data, len);
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
	/* Chunked scan.  The previous implementation issued one flash_read() per
	 * byte, which on nRF52840 SPI NOR is ~40-60 µs per call (CS toggle +
	 * cmd + 24-bit address + 1 byte data).  For a single day of vitals
	 * (~36 KB per active file × 3 files) that cost ~5 s of boot time.
	 *
	 * Reading in 256-byte pages cuts the SPI overhead by ~256× because the
	 * address and command phases happen once per chunk instead of once per
	 * byte.  The inner per-byte loop is byte-for-byte the same logic as
	 * before, with one important invariant preserved: `matched` carries
	 * the partial-EOF-marker match count across chunk boundaries.  That
	 * is the only correctness-critical state when a marker straddles a
	 * 256-byte boundary; everything else is local to one chunk.
	 *
	 * Static buffer (same pattern as first_erased_offset): keeps the 256 B
	 * off thread stacks.  Safe because find_eof_or_erased is only called
	 * from the main thread during boot init (recover_region and the
	 * juxta_log_init cache-reconcile pass).  Both are single-threaded,
	 * sequential, and complete before app timers / work queues start. */
	static uint8_t buf[256];
	const char marker[] = EOF_MARKER;
	const size_t marker_len = strlen(marker);
	size_t matched = 0;

	while (off < end)
	{
		size_t len = MIN(sizeof(buf), end - off);
		int rc = flash_read(ctx->flash, off, buf, len);

		if (rc != 0)
		{
			return rc;
		}

		for (size_t i = 0; i < len; i++)
		{
			uint8_t ch = buf[i];

			if (ch == 0xFFU)
			{
				/* Erased byte = end of written region.  No byte of
				 * EOF_MARKER ("#EOF\n") is 0xFF, so a partial match
				 * cannot extend through 0xFF — safe to bail here
				 * regardless of `matched`. */
				*file_end = off + i;
				*closed = false;
				return 0;
			}

			if (ch == (uint8_t)marker[matched])
			{
				matched++;
				if (matched == marker_len)
				{
					*file_end = off + i + 1U;
					*closed = true;
					return 0;
				}
			}
			else
			{
				/* Mismatch.  If the current byte happens to be the
				 * marker's first character we still have a 1-byte
				 * partial match — same fallback the single-byte
				 * implementation used. */
				matched = (ch == (uint8_t)marker[0]) ? 1U : 0U;
			}
		}

		off += len;
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

	LOG_LOCK();

	ctx->file_count = 0;
	ctx->active_jxs = UINT32_MAX;
	ctx->active_jxv = UINT32_MAX;
	ctx->active_jxb = UINT32_MAX;

	int rc = 0;

	for (size_t i = 0; i < ARRAY_SIZE(regions); i++)
	{
		rc = recover_region(ctx, &regions[i]);

		if (rc != 0)
		{
			LOG_UNLOCK();
			return rc;
		}
	}

	struct juxta_log_cache cache;
	cache_from_context(ctx, &cache);
	(void)juxta_settings_save_log_cache(&cache);
	LOG_INF("Recovered %u NOR CSV pseudo-files", ctx->file_count);
	LOG_UNLOCK();
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
	const struct log_region *region;
	int region_idx;

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

	region = region_for_type(JUXTA_LOG_JXS);
	region_idx = region_index_for_type(JUXTA_LOG_JXS);
	if (!region || region_idx < 0)
	{
		return -EINVAL;
	}
	/* Silently no-op when the JXS region has already been latched full.
	 * The caller (ensure_file) already wrote the header successfully; the
	 * missing day_start row is a minor data loss but is not corruption. */
	if (ctx->region_full[region_idx])
	{
		return 0;
	}

	len = snprintf(row, sizeof(row), "%u,day_start,%s,%s,%s,%s,%u,%u,%u,%s\n",
		       unix_time, s_device_id, settings->subject_id, settings->experiment,
		       JUXTA_FIRMWARE_VERSION, settings->scan_interval_s,
		       settings->adv_interval_s, settings->vitals_interval_s, s_device_id);
	if (len < 0 || len >= (int)sizeof(row))
	{
		return -ENOSPC;
	}

	rc = guarded_flash_append(ctx, region, region_idx,
				  entry->offset + entry->length, row, (size_t)len);
	if (rc == -ENOSPC)
	{
		/* Latch set by guarded_flash_append; treat the missing day_start
		 * row as a soft drop so ensure_file does not unwind the new file. */
		return 0;
	}
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
	int region_idx = region_index_for_type(type);
	static char date[9];
	static char filename[JUXTA_FILE_NAME_LEN];
	static char prefix[4];

	if (!region || !active_slot || !settings || region_idx < 0)
	{
		return -EINVAL;
	}

	/* Clock not yet set — keep using the existing active file if there is one,
	 * or skip creation entirely.  Rows with unix_time == 0 are meaningless. */
	if (unix_time == 0U)
	{
		return 0;
	}

	/* Region full latch: do not touch this region at all (no close, no
	 * header write, no rotation).  Returning 0 lets touch_all_for_calendar_day
	 * continue rotating the other (non-full) regions normally. */
	if (ctx->region_full[region_idx])
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

	/* ----------------------------------------------------------------
	 * Preconditions checked BEFORE any NOR write.  This is the fix for
	 * the "orphan header" leak: with the old order, a successful header
	 * flash_append could be followed by an -ENOSPC from add_file_entry,
	 * leaving an untracked header on NOR that the per-vitals-tick retry
	 * loop would rewrite forever.  Now we either commit the full rotation
	 * (close + header + add_file_entry [+ day_start row]) or we do
	 * nothing.
	 * -------------------------------------------------------------- */
	if (ctx->file_count >= JUXTA_MAX_FILES)
	{
		if (!s_file_count_cap_warned)
		{
			s_file_count_cap_warned = true;
			LOG_WRN("ensure_file: file slot cap reached (%u/%u); further "
				"rotations disabled until clearMemory",
				(unsigned)ctx->file_count, JUXTA_MAX_FILES);
		}
		return 0;
	}

	uint32_t off;
	int rc = first_erased_offset(ctx, region, &off);
	if (rc != 0)
	{
		return rc;
	}
	if (off >= region->start + region->size)
	{
		/* The region's erased tail is gone — latch full and no-op.  Callers
		 * keep working on other regions; row writes targeted at this
		 * region will be dropped by append_row's latch check. */
		ctx->region_full[region_idx] = true;
		LOG_WRN("Region %s exhausted (off=0x%06x end=0x%06x); latching",
			region->prefix, off, region->start + region->size);
		return 0;
	}

	static char header[160];
	int header_len = snprintf(header, sizeof(header), "%s\n%s", filename, region->header);
	if (header_len < 0 || header_len >= (int)sizeof(header))
	{
		return -ENOSPC;
	}

	/* Bounds-check the new file's header + an estimated worst-case
	 * day_start row (~256 B) before committing.  This keeps the rotation
	 * "atomic": if there is not enough physical room left we never close
	 * the old file, never write a half-rotated state.  Both writes still
	 * go through guarded_flash_append below for defence in depth. */
	const uint32_t needed_bytes = (uint32_t)header_len +
		((type == JUXTA_LOG_JXS) ? 256U : 0U);
	if ((uint64_t)off + (uint64_t)needed_bytes > (uint64_t)(region->start + region->size))
	{
		ctx->region_full[region_idx] = true;
		LOG_WRN("Region %s would overflow on rotation (need %u at 0x%06x, end 0x%06x); latching",
			region->prefix, (unsigned)needed_bytes, off, region->start + region->size);
		return 0;
	}

	/* ----------------------------------------------------------------
	 * Commit the rotation: close old (if any), write new header,
	 * register the new entry, optionally append the JXS day_start row,
	 * persist the cache.
	 * -------------------------------------------------------------- */
	if (*active_slot != UINT32_MAX && *active_slot < ctx->file_count &&
		ctx->files[*active_slot].active)
	{
		struct juxta_file_entry *old = &ctx->files[*active_slot];
		rc = guarded_flash_append(ctx, region, region_idx,
					  old->offset + old->length,
					  EOF_MARKER, strlen(EOF_MARKER));
		if (rc == -ENOSPC)
		{
			/* Latched by the guard.  Old file is left active (no EOF);
			 * juxta_log_transfer_payload_bytes() handles active files
			 * correctly without the EOF subtraction. */
			return 0;
		}
		if (rc != 0)
		{
			return rc;
		}
		old->length += strlen(EOF_MARKER);
		old->active = false;
	}

	rc = guarded_flash_append(ctx, region, region_idx, off, header, (size_t)header_len);
	if (rc == -ENOSPC)
	{
		/* Pre-check above should have caught this — defensive only. */
		return 0;
	}
	if (rc != 0)
	{
		return rc;
	}

	rc = add_file_entry(ctx, filename, off, (uint32_t)header_len, type, true);
	if (rc != 0)
	{
		/* Cannot happen after the file_count pre-check above; treated as
		 * a real I/O error and surfaced to the caller. */
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
	const struct log_region *region = region_for_type(type);
	int region_idx = region_index_for_type(type);
	struct juxta_file_entry *entry;
	size_t row_len;
	int rc;

	if (!slot || *slot == UINT32_MAX || *slot >= ctx->file_count)
	{
		return -ENOENT;
	}
	if (!region || region_idx < 0)
	{
		return -EINVAL;
	}

	/* Region full latch: silently drop the row.  Returning 0 keeps the
	 * caller's logging quiet and lets the device continue running with
	 * the other (non-full) regions; the "memory full" status is surfaced
	 * via juxta_log_memory_level_percent() instead of per-call rc spam. */
	if (ctx->region_full[region_idx])
	{
		return 0;
	}

	entry = &ctx->files[*slot];
	row_len = strlen(row);
	rc = guarded_flash_append(ctx, region, region_idx,
				  entry->offset + entry->length, row, row_len);
	if (rc == -ENOSPC)
	{
		/* Latch set by the guard; treat the drop as a silent stop. */
		return 0;
	}
	if (rc != 0)
	{
		return rc;
	}

	entry->length += (uint32_t)row_len;
	/* Do NOT save the NVS log cache here.  The NVS sectors are 4 KB each
	 * and the cache struct is ~1.5 KB at JUXTA_MAX_FILES=48, so a sector
	 * fills after ~2 writes.  At one JXB row every 20 s that means an
	 * erase cycle every ~40 s — the nRF52840 internal flash (10 k cycles)
	 * would fail in days.  Instead the cache is saved only in
	 * ensure_file() on file creation/rotation (~once per calendar day per
	 * type).  juxta_log_init() reconciles stale cached lengths by scanning
	 * each active file with find_eof_or_erased() on every boot. */
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

	LOG_LOCK();

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
		rc = -ENODEV;
		goto out;
	}

	uint32_t now = juxta_time_now();
	bool need_format = false;

	rc = juxta_settings_load_log_cache(&cache);
	if (rc == 0)
	{
		context_from_cache(ctx, &cache);
		LOG_INF("Loaded %u cached NOR CSV pseudo-files", ctx->file_count);

		/* The NVS cache stores the file length only at file
		 * creation/rotation — never on individual row appends (NVS wear:
		 * see comment in append_row).  After a reboot on the same calendar
		 * day the cached length points back to the header, and append_row
		 * would write at that stale offset (into already-written NOR bytes
		 * that are silently ignored).  Scan each active file with
		 * find_eof_or_erased to reconcile entry->length with every row
		 * that was written in previous sessions.  The scan is fast (~30 ms
		 * per 36 KB file) thanks to the 256-byte chunked read inside
		 * find_eof_or_erased — see the comment there. */
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
		goto out;
	}

	ctx->initialized = true;

	/* Skip file creation and the boot event when the clock is not yet set
	 * (unix_time == 0).  ensure_file() will create properly-dated files on
	 * the first append that carries a valid timestamp.  The boot event is
	 * logged by the caller (main.c) after datetime sync completes. */
	if (now == 0U)
	{
		LOG_INF("Clock not set — deferring NOR file creation until time sync");
		rc = 0;
		goto out;
	}

	rc = ensure_file(ctx, JUXTA_LOG_JXS, settings, now);
	if (rc != 0)
	{
		goto out;
	}
	rc = ensure_file(ctx, JUXTA_LOG_JXV, settings, now);
	if (rc != 0)
	{
		goto out;
	}
	rc = ensure_file(ctx, JUXTA_LOG_JXB, settings, now);
	if (rc != 0)
	{
		goto out;
	}

	juxta_time_date_string(now, s_log_calendar_ymd);

	rc = juxta_log_append_event(ctx, settings, device_id, "boot", now);

out:
	LOG_UNLOCK();
	return rc;
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

		/* Per-sector watchdog feed + yield.  Each 4 KB sector erase is
		 * ~45 ms on this SPI NOR and the JXB region alone is ~3 MB
		 * (~750 sectors → ~34 s of pure erase time).  Because the
		 * periodic WDT feed runs on the same system workqueue as the
		 * clearMemory handler, it cannot fire while we are looping
		 * here; we must feed inline.  k_yield() also lets any other
		 * higher-priority thread (BT RX servicing a disconnect, ISR
		 * deferred work) make progress between sector erases. */
		if (s_long_op_tick != NULL)
		{
			s_long_op_tick();
		}
		k_yield();
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

	LOG_LOCK();

	int rc = 0;

	for (size_t i = 0; i < ARRAY_SIZE(regions); i++)
	{
		rc = erase_region(ctx, &regions[i]);

		if (rc != 0)
		{
			LOG_ERR("Failed to erase region %s: %d", regions[i].prefix, rc);
			LOG_UNLOCK();
			return rc;
		}
	}

	(void)juxta_settings_clear_log_cache();
	ctx->file_count = 0;
	ctx->active_jxs = UINT32_MAX;
	ctx->active_jxv = UINT32_MAX;
	ctx->active_jxb = UINT32_MAX;
	memset(ctx->region_full, 0, sizeof(ctx->region_full));
	s_log_calendar_ymd[0] = '\0';
	s_file_count_cap_warned = false;

	LOG_INF("NOR CSV regions erased — file creation deferred to next data write");
	LOG_UNLOCK();
	return 0;
}

int juxta_log_append_event(struct juxta_log_context *ctx, const struct juxta_settings *settings,
						   const char *device_id, const char *event, uint32_t unix_time)
{
	static char row[256];
	int rc;

	if (!ctx || !settings || !device_id || !event)
	{
		return -EINVAL;
	}

	LOG_LOCK();

	rc = touch_all_for_calendar_day(ctx, settings, unix_time);
	if (rc != 0)
	{
		goto out;
	}

	rc = ensure_file(ctx, JUXTA_LOG_JXS, settings, unix_time);
	if (rc != 0)
	{
		goto out;
	}

	int len = snprintf(row, sizeof(row), "%u,%s,%s,%s,%s,%s,%u,%u,%u,%s\n", unix_time,
					   event, device_id, settings->subject_id, settings->experiment,
					   JUXTA_FIRMWARE_VERSION, settings->scan_interval_s,
					   settings->adv_interval_s, settings->vitals_interval_s, device_id);
	if (len < 0 || len >= (int)sizeof(row))
	{
		rc = -ENOSPC;
		goto out;
	}

	LOG_INF("JXS[%u] %s", unix_time, event);
	rc = append_row(ctx, JUXTA_LOG_JXS, row);

out:
	LOG_UNLOCK();
	return rc;
}

int juxta_log_append_vitals(struct juxta_log_context *ctx, uint32_t unix_time, uint16_t motion,
							int32_t batt_mv, int8_t temp_c)
{
	static char row[96];
	int volts = batt_mv / 1000;
	int centivolts = (batt_mv % 1000) / 10;
	int len;
	int rc;

	if (!ctx)
	{
		return -EINVAL;
	}

	LOG_LOCK();

	rc = touch_all_for_calendar_day(ctx, juxta_settings_get(), unix_time);
	if (rc != 0)
	{
		goto out;
	}

	rc = ensure_file(ctx, JUXTA_LOG_JXV, juxta_settings_get(), unix_time);
	if (rc != 0)
	{
		goto out;
	}

	len = snprintf(row, sizeof(row), "%u,%u,%d.%02d,%d\n", unix_time, motion, volts,
				   centivolts, temp_c);
	if (len < 0 || len >= (int)sizeof(row))
	{
		rc = -ENOSPC;
		goto out;
	}

	LOG_INF("JXV[%u] motion=%u batt=%d.%02dV temp=%d", unix_time, motion, volts,
			centivolts, temp_c);
	rc = append_row(ctx, JUXTA_LOG_JXV, row);

out:
	LOG_UNLOCK();
	return rc;
}

int juxta_log_append_ble_observation(struct juxta_log_context *ctx, uint32_t unix_time,
									 const char *observer_id, const char *peer_id, int8_t rssi)
{
	static char row[96];
	int rc;

	if (!ctx || !observer_id || !peer_id)
	{
		return -EINVAL;
	}

	LOG_LOCK();

	rc = touch_all_for_calendar_day(ctx, juxta_settings_get(), unix_time);
	if (rc != 0)
	{
		goto out;
	}

	rc = ensure_file(ctx, JUXTA_LOG_JXB, juxta_settings_get(), unix_time);
	if (rc != 0)
	{
		goto out;
	}

	int len = snprintf(row, sizeof(row), "%u,%s,%s,%d\n", unix_time, observer_id, peer_id,
					   rssi);
	if (len < 0 || len >= (int)sizeof(row))
	{
		rc = -ENOSPC;
		goto out;
	}

	LOG_INF("JXB[%u] %s rssi=%d", unix_time, peer_id, rssi);
	rc = append_row(ctx, JUXTA_LOG_JXB, row);

out:
	LOG_UNLOCK();
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

	LOG_LOCK();

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
			LOG_UNLOCK();
			return -ENOSPC;
		}
		written += (size_t)len;
	}

	/* Append terminal "EOF" token. */
	if (written + 3U >= buffer_size)
	{
		LOG_UNLOCK();
		return -ENOSPC;
	}
	memcpy(buffer + written, "EOF", 4U); /* includes null terminator */
	written += 3U;

	LOG_UNLOCK();
	return (int)written;
}

int juxta_log_find_file(struct juxta_log_context *ctx, const char *path,
						struct juxta_file_entry *entry)
{
	if (!ctx || !path || !entry)
	{
		return -EINVAL;
	}

	LOG_LOCK();

	for (uint16_t i = 0; i < ctx->file_count; i++)
	{
		if (strcmp(ctx->files[i].path, path) == 0)
		{
			*entry = ctx->files[i];
			LOG_UNLOCK();
			return 0;
		}
	}

	LOG_UNLOCK();
	return -ENOENT;
}

int juxta_log_read_file(struct juxta_log_context *ctx, const struct juxta_file_entry *entry,
						uint32_t file_offset, uint8_t *buffer, size_t buffer_size,
						size_t *bytes_read)
{
	uint32_t visible_len;
	int rc = 0;

	if (!ctx || !entry || !buffer || !bytes_read)
	{
		return -EINVAL;
	}

	LOG_LOCK();

	visible_len = entry->length + (entry->active ? (uint32_t)strlen(EOF_MARKER) : 0U);
	if (file_offset >= visible_len)
	{
		*bytes_read = 0;
		LOG_UNLOCK();
		return 0;
	}

	size_t to_read = MIN(buffer_size, visible_len - file_offset);
	if (file_offset < entry->length)
	{
		size_t physical = MIN(to_read, entry->length - file_offset);
		rc = flash_read(ctx->flash, entry->offset + file_offset, buffer, physical);

		if (rc != 0)
		{
			LOG_UNLOCK();
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
	LOG_UNLOCK();
	return 0;
}

int juxta_log_read_file_for_transfer(struct juxta_log_context *ctx,
									 const struct juxta_file_entry *entry, uint32_t file_offset,
									 uint8_t *buffer, size_t buffer_size, size_t *bytes_read)
{
	uint32_t payload;
	uint32_t flash_abs;
	int rc;

	if (!ctx || !entry || !buffer || !bytes_read)
	{
		return -EINVAL;
	}

	LOG_LOCK();

	payload = juxta_log_transfer_payload_bytes(entry);
	if (file_offset >= payload)
	{
		*bytes_read = 0;
		LOG_UNLOCK();
		return 0;
	}

	size_t to_read = MIN(buffer_size, (size_t)(payload - file_offset));
	uint32_t skip = (uint32_t)strlen(entry->path) + 1U;

	flash_abs = entry->offset + skip + file_offset;

	rc = flash_read(ctx->flash, flash_abs, buffer, to_read);

	if (rc != 0)
	{
		LOG_UNLOCK();
		return rc;
	}
	*bytes_read = to_read;
	LOG_UNLOCK();
	return 0;
}
