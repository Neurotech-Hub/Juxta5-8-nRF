#ifndef JUXTA_PROD_H_
#define JUXTA_PROD_H_

#include <stdint.h>

#define JUXTA_PRODUCT_NAME "Juxta5-8"
#define JUXTA_FIRMWARE_VERSION "5.8.0"
#define JUXTA_LOG_SCHEMA "jxta-nor-csv-v4"
#define JUXTA_LOGGING_VERSION 4

#define JUXTA_DEVICE_ID_LEN 10
#define JUXTA_SUBJECT_ID_LEN 32
#define JUXTA_EXPERIMENT_LEN 32
#define JUXTA_UPLOAD_PATH_LEN 32
#define JUXTA_MODE_LEN 16

#define JUXTA_DEFAULT_SCAN_INTERVAL_S 30U
#define JUXTA_DEFAULT_VITALS_INTERVAL_S 60U
#define JUXTA_DEFAULT_ADV_INTERVAL_S 10U
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
/* 3 log types × up to 5 days before clearMemory = 15; keep at 16 for headroom. */
#define JUXTA_MAX_FILES 16
#define JUXTA_TRANSFER_CHUNK_SIZE 512

#endif /* JUXTA_PROD_H_ */
