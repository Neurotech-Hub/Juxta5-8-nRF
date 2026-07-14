#ifndef JUXTA_SETTINGS_H_
#define JUXTA_SETTINGS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "juxta_prod.h"

struct juxta_settings
{
	char subject_id[JUXTA_SUBJECT_ID_LEN];
	char experiment[JUXTA_EXPERIMENT_LEN];
	/* Same 16-byte footprint as legacy `settings_reserved[JUXTA_MODE_LEN]`. */
	uint16_t adv_interval_s;   /* 0–JUXTA_MAX_BLE_INTERVAL_S; 0 = no periodic non-conn adv */
	uint8_t inactivity_multiplier; /* 1–JUXTA_MAX_INACTIVITY_MULTIPLIER; scan interval only */
	uint8_t motion_logging;    /* 1 = count/log motion (default); 0 = ignore IRQ, JXV motion=0 */
	uint8_t settings_reserved[JUXTA_MODE_LEN - 4U];
	char upload_path[JUXTA_UPLOAD_PATH_LEN];
	uint16_t scan_interval_s; /* 0–JUXTA_MAX_BLE_INTERVAL_S; 0 = no periodic passive scan */
	uint16_t vitals_interval_s;
};

struct juxta_log_cache_file
{
	char path[JUXTA_CACHE_NAME_LEN]; /* "JXS20260507.csv" = 15 chars + null */
	uint32_t offset;
	uint32_t length;
	uint8_t log_type;
	uint8_t active; /* uint8_t avoids implicit bool padding */
};

struct juxta_log_cache
{
	uint32_t magic;
	uint16_t version;
	uint16_t file_count;
	struct juxta_log_cache_file files[JUXTA_MAX_FILES];
};

int juxta_settings_init(const char *device_id);
const struct juxta_settings *juxta_settings_get(void);
int juxta_settings_update(const struct juxta_settings *settings);
void juxta_settings_defaults(struct juxta_settings *settings, const char *device_id);

int juxta_settings_load_log_cache(struct juxta_log_cache *cache);
int juxta_settings_save_log_cache(const struct juxta_log_cache *cache);
int juxta_settings_clear_log_cache(void);

/* Persistent operating mode (NVS-backed, separate key from `current` so
 * gateway field updates do not rewrite this byte and vice-versa).  Default
 * value on a unit that has never written op_mode is SHELF, preserving the
 * cold-boot-to-shelf invariant when retained RAM is also empty. */
enum juxta_op_mode juxta_settings_get_op_mode(void);
int juxta_settings_set_op_mode(enum juxta_op_mode mode);

/* Direct single-key op_mode read, safe before juxta_settings_init() (same
 * pattern as the boot counter).  The Step 3 POR-resume decision needs the
 * persisted mode long before the full Step 7 subtree load.  Returns SHELF
 * on any error or unknown stored value. */
enum juxta_op_mode juxta_settings_read_op_mode_early(void);

/* Forensic breadcrumb: written at every shelf entry, consumed (emitted as a
 * JXS row, then cleared) at the next boot that reaches a valid clock.  Kept
 * in its own NVS key so it survives independently of the settings blob. */
struct juxta_breadcrumb
{
	uint32_t magic;		/* JUXTA_BREADCRUMB_MAGIC */
	uint8_t version;	/* JUXTA_BREADCRUMB_VERSION */
	uint8_t reason;		/* enum juxta_shelf_reason */
	uint8_t pof_hit;	/* 1 = POFCON fired during the life that shelved */
	uint8_t _pad;
	uint32_t resetreas; /* RESETREAS of the life that shelved */
	uint32_t boot_count;
	uint32_t unix_time; /* 0 if the clock was never set that life */
	int32_t batt_mv;	/* last battery sample, 0 if none */
};

/* Monotonic boot counter (NVS).  Incremented exactly once per boot, before
 * any shelf-entry branch can run, so even zero-trace lives are counted.
 * Safe to call before juxta_settings_init(): initializes the settings
 * subsystem itself and loads only its own key. */
int juxta_settings_boot_count_increment(void);
uint32_t juxta_settings_boot_count(void);

/* Save is safe before juxta_settings_init() (fresh-boot / battery-gate shelf
 * entries run before Step 7).  Load returns -ENOENT when no valid breadcrumb
 * is stored. */
int juxta_settings_save_breadcrumb(const struct juxta_breadcrumb *crumb);
int juxta_settings_load_breadcrumb(struct juxta_breadcrumb *out);
int juxta_settings_clear_breadcrumb(void);

#endif /* JUXTA_SETTINGS_H_ */
