#include "juxta_checkpoint.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(juxta_checkpoint, LOG_LEVEL_INF);

#define FLASH_NODE DT_ALIAS(spi_mem)

/* Ring geometry.  RING_START must match the space carved from the tail of
 * JXB in juxta_log.c (JXB_SIZE shrunk 0x2F0000 -> 0x2E0000); the two
 * modules never overlap and neither touches the other's region. */
#define RING_START 0x3F0000U
#define RING_SIZE 0x010000U
#define SECTOR_SIZE 0x1000U
#define SLOT_SIZE 32U

#define SLOTS_PER_SECTOR (SECTOR_SIZE / SLOT_SIZE) /* 128 */
#define TOTAL_SLOTS (RING_SIZE / SLOT_SIZE)		   /* 2048 */
#define SECTOR_COUNT (RING_SIZE / SECTOR_SIZE)	   /* 16 */

#define SLOT_MAGIC 0x4A58434BU /* 'JXCK' */
#define SLOT_VERSION 1U

/* On-flash record.  CRC32 covers everything before the crc32 field.
 * Erased flash (all 0xFF) naturally fails the magic + CRC check. */
struct checkpoint_slot
{
	uint32_t magic;
	uint32_t seq; /* monotonic across the whole ring; newest wins */
	uint32_t unix_time;
	uint32_t motion_count;
	int32_t batt_mv;
	int8_t temp_c;
	uint8_t version;
	uint8_t flags; /* reserved, 0 */
	uint8_t _pad;
	uint32_t reserved; /* future use, 0 */
	uint32_t crc32;
};

BUILD_ASSERT(sizeof(struct checkpoint_slot) == SLOT_SIZE,
			 "checkpoint slot must be exactly one ring slot");

static const struct device *const s_flash = DEVICE_DT_GET(FLASH_NODE);

/* Serializes head/seq mutations and multi-step flash sequences between the
 * 1 Hz checkpoint work item (sysworkq), the clearMemory work item (also
 * sysworkq) and enter_shelf_mode() on the main thread. */
static K_MUTEX_DEFINE(s_lock);

static bool s_scanned;	/* head/seq/newest are valid */
static bool s_disabled; /* writes no-op (shelf teardown) */
static uint32_t s_head; /* next slot index to write, 0..TOTAL_SLOTS-1 */
static uint32_t s_next_seq;
static struct juxta_checkpoint_record s_newest;
static bool s_newest_valid;

static uint32_t slot_offset(uint32_t slot)
{
	return RING_START + slot * SLOT_SIZE;
}

static uint32_t slot_crc(const struct checkpoint_slot *slot)
{
	return crc32_ieee((const uint8_t *)slot, offsetof(struct checkpoint_slot, crc32));
}

static bool slot_valid(const struct checkpoint_slot *slot)
{
	return slot->magic == SLOT_MAGIC && slot->version == SLOT_VERSION &&
		   slot->unix_time != 0U && slot->crc32 == slot_crc(slot);
}

static int erase_sector_at(uint32_t offset)
{
	int rc = flash_erase(s_flash, offset & ~(SECTOR_SIZE - 1U), SECTOR_SIZE);

	if (rc != 0)
	{
		LOG_ERR("sector erase @0x%06x failed: %d", (unsigned int)offset, rc);
	}
	return rc;
}

/* Scan the whole ring: remember the newest valid record (highest seq) and
 * park the write head on the slot after it.  If the remainder of the head's
 * sector is not erased (stale data from a previous wrap), advance the head
 * to the next sector boundary — the on-entry erase in write() handles the
 * rest.  Must be called with s_lock held. */
static int scan_ring_locked(void)
{
	static uint8_t buf[512]; /* 16 slots per read, 128 reads total */
	struct checkpoint_slot slot;
	uint32_t best_seq = 0U;
	uint32_t best_slot = UINT32_MAX;
	struct juxta_checkpoint_record best = {0};

	if (!device_is_ready(s_flash))
	{
		return -ENODEV;
	}

	for (uint32_t off = 0U; off < RING_SIZE; off += sizeof(buf))
	{
		int rc = flash_read(s_flash, RING_START + off, buf, sizeof(buf));

		if (rc != 0)
		{
			LOG_ERR("scan read @0x%06x failed: %d", (unsigned int)(RING_START + off), rc);
			return rc;
		}

		for (uint32_t i = 0U; i < sizeof(buf); i += SLOT_SIZE)
		{
			memcpy(&slot, &buf[i], sizeof(slot));
			if (slot_valid(&slot) && slot.seq >= best_seq)
			{
				best_seq = slot.seq;
				best_slot = (off + i) / SLOT_SIZE;
				best.unix_time = slot.unix_time;
				best.motion_count = slot.motion_count;
				best.batt_mv = slot.batt_mv;
				best.temp_c = slot.temp_c;
			}
		}
	}

	if (best_slot == UINT32_MAX)
	{
		s_head = 0U;
		s_next_seq = 1U;
		s_newest_valid = false;
		s_scanned = true;
		LOG_INF("ring empty — head at slot 0");
		return 0;
	}

	s_newest = best;
	s_newest_valid = true;
	s_next_seq = best_seq + 1U;
	s_head = (best_slot + 1U) % TOTAL_SLOTS;

	/* Stale-tail check: programming over non-erased bytes would corrupt
	 * every record until the next sector boundary.  If any byte between
	 * the head and its sector end is not 0xFF, skip to the next sector
	 * (loses nothing newer than the oldest records). */
	if ((s_head % SLOTS_PER_SECTOR) != 0U)
	{
		uint32_t sector_end_slot = ((s_head / SLOTS_PER_SECTOR) + 1U) * SLOTS_PER_SECTOR;
		bool tail_erased = true;

		for (uint32_t slot_idx = s_head; slot_idx < sector_end_slot && tail_erased;
			 slot_idx += sizeof(buf) / SLOT_SIZE)
		{
			uint32_t count = MIN(sizeof(buf) / SLOT_SIZE, sector_end_slot - slot_idx);
			int rc = flash_read(s_flash, slot_offset(slot_idx), buf, count * SLOT_SIZE);

			if (rc != 0)
			{
				return rc;
			}
			for (uint32_t b = 0U; b < count * SLOT_SIZE; b++)
			{
				if (buf[b] != 0xFFU)
				{
					tail_erased = false;
					break;
				}
			}
		}

		if (!tail_erased)
		{
			s_head = sector_end_slot % TOTAL_SLOTS;
		}
	}

	s_scanned = true;
	LOG_INF("newest seq=%u unix=%u — head at slot %u", best_seq, best.unix_time, s_head);
	return 0;
}

static int ensure_scanned_locked(void)
{
	if (s_scanned)
	{
		return 0;
	}
	return scan_ring_locked();
}

int juxta_checkpoint_init(void)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	int rc = ensure_scanned_locked();

	k_mutex_unlock(&s_lock);
	return rc;
}

int juxta_checkpoint_load(struct juxta_checkpoint_record *out)
{
	if (!out)
	{
		return -EINVAL;
	}

	k_mutex_lock(&s_lock, K_FOREVER);
	int rc = ensure_scanned_locked();

	if (rc == 0)
	{
		if (s_newest_valid)
		{
			*out = s_newest;
		}
		else
		{
			rc = -ENOENT;
		}
	}
	k_mutex_unlock(&s_lock);
	return rc;
}

int juxta_checkpoint_write(const struct juxta_checkpoint_record *rec)
{
	struct checkpoint_slot slot;
	int rc;

	if (!rec || rec->unix_time == 0U)
	{
		return -EINVAL;
	}

	k_mutex_lock(&s_lock, K_FOREVER);

	if (s_disabled)
	{
		k_mutex_unlock(&s_lock);
		return 0;
	}

	rc = ensure_scanned_locked();
	if (rc != 0)
	{
		k_mutex_unlock(&s_lock);
		return rc;
	}

	/* On-entry erase: the sector the head is entering holds only the
	 * oldest ~2 minutes of records.  ~45 ms once every 128 writes on the
	 * system workqueue — well under the 2 s WDT feed cadence. */
	if ((s_head % SLOTS_PER_SECTOR) == 0U)
	{
		rc = erase_sector_at(slot_offset(s_head));
		if (rc != 0)
		{
			k_mutex_unlock(&s_lock);
			return rc;
		}
	}

	memset(&slot, 0, sizeof(slot));
	slot.magic = SLOT_MAGIC;
	slot.seq = s_next_seq;
	slot.unix_time = rec->unix_time;
	slot.motion_count = rec->motion_count;
	slot.batt_mv = rec->batt_mv;
	slot.temp_c = rec->temp_c;
	slot.version = SLOT_VERSION;
	slot.crc32 = slot_crc(&slot);

	rc = flash_write(s_flash, slot_offset(s_head), &slot, sizeof(slot));
	if (rc != 0)
	{
		LOG_ERR("slot write @%u failed: %d", s_head, rc);
		/* Skip the (possibly half-programmed) slot so the next write
		 * cannot land on dirty bytes. */
		s_head = (s_head + 1U) % TOTAL_SLOTS;
		k_mutex_unlock(&s_lock);
		return rc;
	}

	s_newest.unix_time = rec->unix_time;
	s_newest.motion_count = rec->motion_count;
	s_newest.batt_mv = rec->batt_mv;
	s_newest.temp_c = rec->temp_c;
	s_newest_valid = true;
	s_next_seq++;
	s_head = (s_head + 1U) % TOTAL_SLOTS;

	k_mutex_unlock(&s_lock);
	return 0;
}

/* Erase every non-blank sector of the ring and restart at slot 0.  The
 * already-erased check keeps repeat resets (activation after a shelf-entry
 * erase) nearly free.  Must be called with s_lock held. */
static int erase_ring_locked(void)
{
	static uint8_t buf[512];

	if (!device_is_ready(s_flash))
	{
		return -ENODEV;
	}

	for (uint32_t sector = 0U; sector < SECTOR_COUNT; sector++)
	{
		uint32_t base = RING_START + sector * SECTOR_SIZE;
		bool blank = true;

		for (uint32_t off = 0U; off < SECTOR_SIZE && blank; off += sizeof(buf))
		{
			int rc = flash_read(s_flash, base + off, buf, sizeof(buf));

			if (rc != 0)
			{
				return rc;
			}
			for (uint32_t b = 0U; b < sizeof(buf); b++)
			{
				if (buf[b] != 0xFFU)
				{
					blank = false;
					break;
				}
			}
		}

		if (!blank)
		{
			int rc = erase_sector_at(base);

			if (rc != 0)
			{
				return rc;
			}
		}
	}

	s_head = 0U;
	s_next_seq = 1U;
	s_newest_valid = false;
	s_scanned = true;
	return 0;
}

int juxta_checkpoint_reset(void)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	int rc = erase_ring_locked();

	if (rc == 0)
	{
		s_disabled = false;
		LOG_INF("ring reset — checkpointing enabled");
	}
	k_mutex_unlock(&s_lock);
	return rc;
}

void juxta_checkpoint_disable(void)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	s_disabled = true;
	k_mutex_unlock(&s_lock);
}

int juxta_checkpoint_erase(void)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	int rc = erase_ring_locked();

	k_mutex_unlock(&s_lock);
	return rc;
}
