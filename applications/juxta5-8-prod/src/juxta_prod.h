#ifndef JUXTA_PROD_H_
#define JUXTA_PROD_H_

#include <stdint.h>

#define JUXTA_PRODUCT_NAME "Juxta5-8"
#define JUXTA_FIRMWARE_VERSION "5.8.2"
#define JUXTA_LOG_SCHEMA "jxta-nor-csv-v5"
#define JUXTA_LOGGING_VERSION 5

#define JUXTA_DEVICE_ID_LEN 10
#define JUXTA_SUBJECT_ID_LEN 32
#define JUXTA_EXPERIMENT_LEN 32
#define JUXTA_UPLOAD_PATH_LEN 32
#define JUXTA_MODE_LEN 16

#define JUXTA_DEFAULT_SCAN_INTERVAL_S 30U
#define JUXTA_DEFAULT_VITALS_INTERVAL_S 60U
#define JUXTA_DEFAULT_ADV_INTERVAL_S 5U
/* Non-connectable adv cadence and passive scan burst cadence (seconds), NVS + Gateway.
 * Whole-second integers 0 through JUXTA_MAX_BLE_INTERVAL_S inclusive (0 = that modality off).
 * No stepping; values above the max are clamped on save. */
#define JUXTA_MAX_BLE_INTERVAL_S 120U
/* After a vitals window with zero LIS2DH12 motion, passive scan cadence uses
 * scan_interval_s × inactivity_multiplier (capped at JUXTA_MAX_BLE_INTERVAL_S). */
#define JUXTA_DEFAULT_INACTIVITY_MULTIPLIER 1U
#define JUXTA_MAX_INACTIVITY_MULTIPLIER 10U
/* Node `upload_path` and NVS field (single hard-coded path until app-driven paths return). */
#define JUXTA_UPLOAD_PATH_FIXED "/"
#define JUXTA_DEFAULT_UPLOAD_PATH JUXTA_UPLOAD_PATH_FIXED

/* Runtime file-path buffer (BLE listing, find, transfer).
 * Filenames are "JXByyyymmdd.csv" = 15 chars + null. Use 20 for headroom so
 * read_line() can read the full name and still find the trailing '\n'. */
#define JUXTA_FILE_NAME_LEN 20
#define JUXTA_FILE_PATH_LEN 64
/* NVS-backed cache uses a tighter name field to keep the struct small.
 * Filenames are at most "JXS20260507.csv" = 15 chars + null. */
#define JUXTA_CACHE_NAME_LEN 20
/* 3 log types × up to 16 days = 48; sized for 7–12 day deployments with
 * margin.  The NVS-backed juxta_log_cache (8 B header + 32 B per file) is
 * ~1.5 KB at 48 entries — well under a 4 KB internal-flash sector and well
 * under the per-entry write limit of the SETTINGS_NVS backend. */
#define JUXTA_MAX_FILES 48
#define JUXTA_TRANSFER_CHUNK_SIZE 512

/* Persisted operating mode (NVS-backed; see juxta_settings_get_op_mode /
 * juxta_settings_set_op_mode).  The recovery-on-silent-reset boot branch in
 * main.c only attempts to resume production when this is PROD, so DFU- and
 * shelf-mode units never accidentally fall into the recovery path. */
enum juxta_op_mode
{
	JUXTA_OP_MODE_SHELF = 0, /* default — power-on, shelf, after magnet/gateway-driven exit */
	JUXTA_OP_MODE_PROD = 1,	 /* production init completed; recovery target */
	JUXTA_OP_MODE_DFU = 2,	 /* DFU mode entered */
};

#endif /* JUXTA_PROD_H_ */
