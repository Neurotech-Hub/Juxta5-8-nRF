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
	uint8_t settings_reserved[JUXTA_MODE_LEN - 3U];
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

#endif /* JUXTA_SETTINGS_H_ */
