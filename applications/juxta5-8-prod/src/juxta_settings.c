#include "juxta_settings.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(juxta_settings, LOG_LEVEL_INF);

#define SETTINGS_SUBTREE "juxta_prod"
#define SETTINGS_KEY_CURRENT "current"
#define SETTINGS_KEY_LOG_CACHE "log_cache"
#define LOG_CACHE_MAGIC 0x4A584C43U /* JXLC */
#define LOG_CACHE_VERSION 1U

static struct juxta_settings current_settings;
static struct juxta_log_cache current_log_cache;
static bool loaded_settings;
static bool loaded_log_cache;
static char boot_device_id[JUXTA_DEVICE_ID_LEN];

static void copy_string(char *dst, size_t dst_size, const char *src)
{
	if (dst_size == 0U) {
		return;
	}

	if (!src || src[0] == '\0') {
		dst[0] = '\0';
		return;
	}

	(void)snprintf(dst, dst_size, "%s", src);
}

void juxta_settings_defaults(struct juxta_settings *settings, const char *device_id)
{
	memset(settings, 0, sizeof(*settings));
	copy_string(settings->subject_id, sizeof(settings->subject_id),
		    (device_id && device_id[0] != '\0') ? device_id : "JX_000000");
	copy_string(settings->experiment, sizeof(settings->experiment), JUXTA_DEFAULT_EXPERIMENT);
	copy_string(settings->mode, sizeof(settings->mode), JUXTA_DEFAULT_MODE);
	copy_string(settings->upload_path, sizeof(settings->upload_path), JUXTA_DEFAULT_UPLOAD_PATH);
	settings->scan_interval_s = JUXTA_DEFAULT_SCAN_INTERVAL_S;
	settings->vitals_interval_s = JUXTA_DEFAULT_VITALS_INTERVAL_S;
}

static int settings_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(key, SETTINGS_KEY_CURRENT) == 0) {
		if (len != sizeof(current_settings)) {
			LOG_WRN("Ignoring settings size %zu expected %zu", len, sizeof(current_settings));
			return -EINVAL;
		}

		int rc = read_cb(cb_arg, &current_settings, sizeof(current_settings));
		if (rc >= 0) {
			loaded_settings = true;
			return 0; /* settings_read_cb returns byte count; subsystem expects 0 */
		}
		return rc;
	}

	if (strcmp(key, SETTINGS_KEY_LOG_CACHE) == 0) {
		if (len != sizeof(current_log_cache)) {
			LOG_WRN("Ignoring log cache size %zu expected %zu", len, sizeof(current_log_cache));
			return -EINVAL;
		}

		int rc = read_cb(cb_arg, &current_log_cache, sizeof(current_log_cache));
		if (rc >= 0 && current_log_cache.magic == LOG_CACHE_MAGIC &&
		    current_log_cache.version == LOG_CACHE_VERSION &&
		    current_log_cache.file_count <= JUXTA_MAX_FILES) {
			loaded_log_cache = true;
		}
		return (rc >= 0) ? 0 : rc;
	}

	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(juxta_prod, SETTINGS_SUBTREE, NULL, settings_set, NULL, NULL);

static void sanitize_settings(struct juxta_settings *settings)
{
	settings->subject_id[sizeof(settings->subject_id) - 1] = '\0';
	settings->experiment[sizeof(settings->experiment) - 1] = '\0';
	settings->mode[sizeof(settings->mode) - 1] = '\0';
	settings->upload_path[sizeof(settings->upload_path) - 1] = '\0';

	if (settings->subject_id[0] == '\0') {
		copy_string(settings->subject_id, sizeof(settings->subject_id), boot_device_id);
	}
	if (settings->experiment[0] == '\0') {
		copy_string(settings->experiment, sizeof(settings->experiment), JUXTA_DEFAULT_EXPERIMENT);
	}
	if (settings->mode[0] == '\0') {
		copy_string(settings->mode, sizeof(settings->mode), JUXTA_DEFAULT_MODE);
	}
	if (settings->upload_path[0] == '\0') {
		copy_string(settings->upload_path, sizeof(settings->upload_path), JUXTA_DEFAULT_UPLOAD_PATH);
	}
	if (settings->scan_interval_s == 0U) {
		settings->scan_interval_s = JUXTA_DEFAULT_SCAN_INTERVAL_S;
	}
	if (settings->vitals_interval_s == 0U) {
		settings->vitals_interval_s = JUXTA_DEFAULT_VITALS_INTERVAL_S;
	}
}

int juxta_settings_init(const char *device_id)
{
	int rc;

	copy_string(boot_device_id, sizeof(boot_device_id),
		    (device_id && device_id[0] != '\0') ? device_id : "JX_000000");
	juxta_settings_defaults(&current_settings, boot_device_id);

	rc = settings_subsys_init();
	if (rc != 0 && rc != -EALREADY) {
		LOG_ERR("settings_subsys_init failed: %d", rc);
		return rc;
	}

	rc = settings_load_subtree(SETTINGS_SUBTREE);
	if (rc != 0) {
		LOG_WRN("settings_load_subtree failed: %d; using defaults", rc);
	}

	if (!loaded_settings) {
		LOG_INF("No saved settings; storing defaults");
		rc = juxta_settings_update(&current_settings);
		if (rc != 0) {
			return rc;
		}
	} else {
		sanitize_settings(&current_settings);
	}

	LOG_INF("settings subject=%s experiment=%s mode=%s scan=%us vitals=%us",
		current_settings.subject_id, current_settings.experiment, current_settings.mode,
		current_settings.scan_interval_s, current_settings.vitals_interval_s);
	return 0;
}

const struct juxta_settings *juxta_settings_get(void)
{
	return &current_settings;
}

int juxta_settings_update(const struct juxta_settings *settings)
{
	struct juxta_settings next;
	int rc;

	if (!settings) {
		return -EINVAL;
	}

	next = *settings;
	sanitize_settings(&next);

	rc = settings_save_one(SETTINGS_SUBTREE "/" SETTINGS_KEY_CURRENT, &next, sizeof(next));
	if (rc != 0) {
		LOG_ERR("settings_save_one current failed: %d", rc);
		return rc;
	}

	current_settings = next;
	loaded_settings = true;
	return 0;
}

int juxta_settings_load_log_cache(struct juxta_log_cache *cache)
{
	if (!cache) {
		return -EINVAL;
	}
	if (!loaded_log_cache) {
		return -ENOENT;
	}

	*cache = current_log_cache;
	return 0;
}

int juxta_settings_save_log_cache(const struct juxta_log_cache *cache)
{
	/* Static: keeps the 520-byte struct off thread stacks. This function is
	 * only called from ensure_file() (file creation) which runs infrequently
	 * and not concurrently with itself. */
	static struct juxta_log_cache next;
	int rc;

	if (!cache || cache->file_count > JUXTA_MAX_FILES) {
		return -EINVAL;
	}

	next = *cache;
	next.magic = LOG_CACHE_MAGIC;
	next.version = LOG_CACHE_VERSION;

	rc = settings_save_one(SETTINGS_SUBTREE "/" SETTINGS_KEY_LOG_CACHE, &next, sizeof(next));
	if (rc != 0) {
		LOG_WRN("settings_save_one log cache failed: %d", rc);
		return rc;
	}

	current_log_cache = next;
	loaded_log_cache = true;
	return 0;
}

int juxta_settings_clear_log_cache(void)
{
	memset(&current_log_cache, 0, sizeof(current_log_cache));
	loaded_log_cache = false;
	return settings_delete(SETTINGS_SUBTREE "/" SETTINGS_KEY_LOG_CACHE);
}
