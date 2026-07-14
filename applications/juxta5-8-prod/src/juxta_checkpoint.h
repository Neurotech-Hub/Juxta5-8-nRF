/*
 * Juxta5-8 production checkpoint ring ("draft vitals")
 *
 * A 64 KB ring buffer at the tail of the external NOR (carved from JXB)
 * that persists a small clock + vitals snapshot once per second while
 * production runs.  Unlike the retained-RAM RTC snapshot in juxta_time.c
 * (which only survives soft resets), this ring survives a true power-on
 * reset — it is what lets a mid-session POR resume production instead of
 * silently shelving (fw 5.8.4 "POR production resume").
 *
 * Geometry: 16 × 4 KB sectors, 32 B records → 2048 slots (~34 min of
 * history at 1 Hz).  Each sector is erased as the write head enters it,
 * so the newest ~15 sectors of records are always intact.  Endurance:
 * ~42 sector erases/day → years at typical 100k NOR P/E cycles.  A torn
 * record from a dying supply simply fails CRC; the previous record wins.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef JUXTA_CHECKPOINT_H_
#define JUXTA_CHECKPOINT_H_

#include <stdint.h>

/* RAM-facing view of one checkpoint.  The on-flash slot adds magic, a
 * monotonic sequence number, a version byte and a CRC32 (32 B total). */
struct juxta_checkpoint_record
{
	uint32_t unix_time;	   /* clock at capture (never 0 for stored records) */
	uint32_t motion_count; /* LIS2DH12 events since the last JXV vitals row */
	int32_t batt_mv;	   /* last battery sample */
	int8_t temp_c;		   /* last LIS2DH12 temperature (0 until first vitals) */
};

/* Scan the ring for the newest valid record and position the write head
 * after it.  Idempotent; safe to call before juxta_settings_init() (touches
 * only external NOR).  Returns 0 on success (including an empty ring),
 * -ENODEV when the flash device is not ready. */
int juxta_checkpoint_init(void);

/* Copy the newest valid record found by the scan into *out.
 * Auto-scans when needed.  Returns 0 on success, -ENOENT when the ring
 * holds no valid record. */
int juxta_checkpoint_load(struct juxta_checkpoint_record *out);

/* Append one record at the write head (erasing the sector on first entry).
 * No-ops with 0 after juxta_checkpoint_disable().  Auto-scans when needed.
 * Called from the system workqueue once per second in production. */
int juxta_checkpoint_write(const struct juxta_checkpoint_record *rec);

/* Erase the whole ring and restart at slot 0 with writes enabled.  Called
 * at every fresh production activation (before op_mode := PROD) and from
 * the clearMemory path so a stale draft can never leak across sessions. */
int juxta_checkpoint_reset(void);

/* Stop accepting writes (shelf entry teardown).  The 1 Hz timer may still
 * fire once more; the queued write becomes a no-op. */
void juxta_checkpoint_disable(void);

/* Erase the ring storage without changing the disabled state.  Used by
 * enter_shelf_mode() after juxta_checkpoint_disable(). */
int juxta_checkpoint_erase(void);

#endif /* JUXTA_CHECKPOINT_H_ */
