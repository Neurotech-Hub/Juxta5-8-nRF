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
#define SETTINGS_KEY_OP_MODE "op_mode"
#define SETTINGS_KEY_BOOT_COUNT "boot_count"
#define SETTINGS_KEY_BREADCRUMB "crumb"
#define LOG_CACHE_MAGIC 0x4A584C43U /* JXLC */
#define LOG_CACHE_VERSION 1U
#define JUXTA_BREADCRUMB_MAGIC 0x4A584243U /* JXBC */
#define JUXTA_BREADCRUMB_VERSION 1U

static struct juxta_settings current_settings;
static struct juxta_log_cache current_log_cache;
static bool loaded_settings;
static bool loaded_log_cache;
/* One-byte persisted operating mode (NVS).  Defaults to SHELF before any
 * setter runs so the cold-boot / first-flash path naturally lands in shelf. */
static uint8_t current_op_mode = (uint8_t)JUXTA_OP_MODE_SHELF;
static char boot_device_id[JUXTA_DEVICE_ID_LEN];
static uint32_t current_boot_count;
static struct juxta_breadcrumb current_breadcrumb;
static bool loaded_breadcrumb;

static void copy_string(char *dst, size_t dst_size, const char *src)
{
	if (dst_size == 0U)
	{
		return;
	}

	if (!src || src[0] == '\0')
	{
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
	copy_string(settings->upload_path, sizeof(settings->upload_path), JUXTA_DEFAULT_UPLOAD_PATH);
	settings->scan_interval_s = JUXTA_DEFAULT_SCAN_INTERVAL_S;
	settings->vitals_interval_s = JUXTA_DEFAULT_VITALS_INTERVAL_S;
	settings->adv_interval_s = JUXTA_DEFAULT_ADV_INTERVAL_S;
	settings->inactivity_multiplier = JUXTA_DEFAULT_INACTIVITY_MULTIPLIER;
	settings->motion_logging = 1U;
}

static int settings_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(key, SETTINGS_KEY_CURRENT) == 0)
	{
		if (len != sizeof(current_settings))
		{
			LOG_WRN("Ignoring settings size %zu expected %zu", len, sizeof(current_settings));
			return -EINVAL;
		}

		int rc = read_cb(cb_arg, &current_settings, sizeof(current_settings));
		if (rc >= 0)
		{
			loaded_settings = true;
			return 0; /* settings_read_cb returns byte count; subsystem expects 0 */
		}
		return rc;
	}

	if (strcmp(key, SETTINGS_KEY_LOG_CACHE) == 0)
	{
		if (len != sizeof(current_log_cache))
		{
			LOG_WRN("Ignoring log cache size %zu expected %zu", len, sizeof(current_log_cache));
			return -EINVAL;
		}

		int rc = read_cb(cb_arg, &current_log_cache, sizeof(current_log_cache));
		if (rc >= 0 && current_log_cache.magic == LOG_CACHE_MAGIC &&
			current_log_cache.version == LOG_CACHE_VERSION &&
			current_log_cache.file_count <= JUXTA_MAX_FILES)
		{
			loaded_log_cache = true;
		}
		return (rc >= 0) ? 0 : rc;
	}

	if (strcmp(key, SETTINGS_KEY_BOOT_COUNT) == 0)
	{
		if (len != sizeof(current_boot_count))
		{
			LOG_WRN("Ignoring boot_count size %zu expected %zu", len,
					sizeof(current_boot_count));
			return -EINVAL;
		}

		uint32_t stored = 0U;
		int rc = read_cb(cb_arg, &stored, sizeof(stored));
		if (rc >= 0)
		{
			/* Keep the larger value: the early-boot increment may already
			 * have bumped the RAM copy before the subtree load runs. */
			if (stored > current_boot_count)
			{
				current_boot_count = stored;
			}
			return 0;
		}
		return rc;
	}

	if (strcmp(key, SETTINGS_KEY_BREADCRUMB) == 0)
	{
		if (len != sizeof(current_breadcrumb))
		{
			LOG_WRN("Ignoring breadcrumb size %zu expected %zu", len,
					sizeof(current_breadcrumb));
			return -EINVAL;
		}

		int rc = read_cb(cb_arg, &current_breadcrumb, sizeof(current_breadcrumb));
		if (rc >= 0 && current_breadcrumb.magic == JUXTA_BREADCRUMB_MAGIC &&
			current_breadcrumb.version == JUXTA_BREADCRUMB_VERSION)
		{
			loaded_breadcrumb = true;
		}
		return (rc >= 0) ? 0 : rc;
	}

	if (strcmp(key, SETTINGS_KEY_OP_MODE) == 0)
	{
		if (len != sizeof(current_op_mode))
		{
			LOG_WRN("Ignoring op_mode size %zu expected %zu", len,
					sizeof(current_op_mode));
			return -EINVAL;
		}

		uint8_t stored = 0U;
		int rc = read_cb(cb_arg, &stored, sizeof(stored));
		if (rc >= 0)
		{
			/* Sanitize: unknown values fall back to SHELF, which is the
			 * safest default when we cannot trust the persisted byte. */
			if (stored == (uint8_t)JUXTA_OP_MODE_PROD ||
				stored == (uint8_t)JUXTA_OP_MODE_DFU ||
				stored == (uint8_t)JUXTA_OP_MODE_SHELF)
			{
				current_op_mode = stored;
			}
			else
			{
				LOG_WRN("Unknown op_mode=%u in NVS, defaulting to SHELF", stored);
				current_op_mode = (uint8_t)JUXTA_OP_MODE_SHELF;
			}
			return 0;
		}
		return rc;
	}

	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(juxta_prod, SETTINGS_SUBTREE, NULL, settings_set, NULL, NULL);

static void sanitize_settings(struct juxta_settings *settings)
{
	settings->subject_id[sizeof(settings->subject_id) - 1] = '\0';
	settings->experiment[sizeof(settings->experiment) - 1] = '\0';
	settings->upload_path[sizeof(settings->upload_path) - 1] = '\0';

	if (settings->subject_id[0] == '\0')
	{
		copy_string(settings->subject_id, sizeof(settings->subject_id), boot_device_id);
	}
	/* Fixed product policy: always `/` in RAM and on save (Node + NVS). */
	copy_string(settings->upload_path, sizeof(settings->upload_path), JUXTA_UPLOAD_PATH_FIXED);
	/* 0 = disable non-connectable advertising; otherwise 1..JUXTA_MAX_BLE_INTERVAL_S (any integer). */
	if (settings->adv_interval_s > JUXTA_MAX_BLE_INTERVAL_S)
	{
		settings->adv_interval_s = JUXTA_MAX_BLE_INTERVAL_S;
	}
	/* 0 = disable passive scan bursts; otherwise 1..JUXTA_MAX_BLE_INTERVAL_S (any integer). */
	if (settings->scan_interval_s > JUXTA_MAX_BLE_INTERVAL_S)
	{
		settings->scan_interval_s = JUXTA_MAX_BLE_INTERVAL_S;
	}
	if (settings->vitals_interval_s == 0U)
	{
		settings->vitals_interval_s = JUXTA_DEFAULT_VITALS_INTERVAL_S;
	}
	if (settings->inactivity_multiplier == 0U ||
	    settings->inactivity_multiplier > JUXTA_MAX_INACTIVITY_MULTIPLIER)
	{
		if (settings->inactivity_multiplier > JUXTA_MAX_INACTIVITY_MULTIPLIER)
		{
			settings->inactivity_multiplier = JUXTA_MAX_INACTIVITY_MULTIPLIER;
		}
		else
		{
			settings->inactivity_multiplier = JUXTA_DEFAULT_INACTIVITY_MULTIPLIER;
		}
	}
	if (settings->motion_logging > 1U)
	{
		settings->motion_logging = 1U;
	}
	else if (settings->motion_logging == 0U && settings->settings_reserved[0] == 0U)
	{
		/* Legacy NVS: this byte was settings_reserved[0] (always 0). */
		settings->motion_logging = 1U;
	}
}

int juxta_settings_init(const char *device_id)
{
	int rc;

	copy_string(boot_device_id, sizeof(boot_device_id),
				(device_id && device_id[0] != '\0') ? device_id : "JX_000000");
	juxta_settings_defaults(&current_settings, boot_device_id);

	rc = settings_subsys_init();
	if (rc != 0 && rc != -EALREADY)
	{
		LOG_ERR("settings_subsys_init failed: %d", rc);
		return rc;
	}

	rc = settings_load_subtree(SETTINGS_SUBTREE);
	if (rc != 0)
	{
		LOG_WRN("settings_load_subtree failed: %d; using defaults", rc);
	}

	if (!loaded_settings)
	{
		LOG_INF("No saved settings; storing defaults");
		rc = juxta_settings_update(&current_settings);
		if (rc != 0)
		{
			return rc;
		}
	}
	else
	{
		sanitize_settings(&current_settings);
	}

	LOG_INF("settings subject=%s experiment=%s scan=%us adv=%us vitals=%us inactivity_multiplier=%u motion_logging=%u",
			current_settings.subject_id, current_settings.experiment,
			current_settings.scan_interval_s, current_settings.adv_interval_s,
			current_settings.vitals_interval_s,
			(unsigned int)current_settings.inactivity_multiplier,
			(unsigned int)current_settings.motion_logging);
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

	if (!settings)
	{
		return -EINVAL;
	}

	next = *settings;
	sanitize_settings(&next);

	rc = settings_save_one(SETTINGS_SUBTREE "/" SETTINGS_KEY_CURRENT, &next, sizeof(next));
	if (rc != 0)
	{
		LOG_ERR("settings_save_one current failed: %d", rc);
		return rc;
	}

	current_settings = next;
	loaded_settings = true;
	return 0;
}

int juxta_settings_load_log_cache(struct juxta_log_cache *cache)
{
	if (!cache)
	{
		return -EINVAL;
	}
	if (!loaded_log_cache)
	{
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

	if (!cache || cache->file_count > JUXTA_MAX_FILES)
	{
		return -EINVAL;
	}

	next = *cache;
	next.magic = LOG_CACHE_MAGIC;
	next.version = LOG_CACHE_VERSION;

	rc = settings_save_one(SETTINGS_SUBTREE "/" SETTINGS_KEY_LOG_CACHE, &next, sizeof(next));
	if (rc != 0)
	{
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

enum juxta_op_mode juxta_settings_get_op_mode(void)
{
	return (enum juxta_op_mode)current_op_mode;
}

int juxta_settings_set_op_mode(enum juxta_op_mode mode)
{
	uint8_t next = (uint8_t)mode;

	/* Sanitize at the boundary so a bad argument cannot poison NVS. */
	if (mode != JUXTA_OP_MODE_SHELF && mode != JUXTA_OP_MODE_PROD &&
		mode != JUXTA_OP_MODE_DFU)
	{
		LOG_ERR("Refusing to save unknown op_mode=%u", next);
		return -EINVAL;
	}

	/* Skip the write when value is unchanged: minimises NVS wear over the
	 * many shelf-PROD-shelf cycles a unit goes through during testing. */
	if (next == current_op_mode)
	{
		return 0;
	}

	int rc = settings_save_one(SETTINGS_SUBTREE "/" SETTINGS_KEY_OP_MODE, &next,
							   sizeof(next));
	if (rc != 0)
	{
		LOG_ERR("settings_save_one op_mode failed: %d", rc);
		return rc;
	}

	current_op_mode = next;
	LOG_INF("op_mode := %u", next);
	return 0;
}

/* ---------------------------------------------------------------------------
 * Boot counter + forensic breadcrumb
 *
 * Both must be usable *before* juxta_settings_init(): the fresh-boot and
 * Step 3b battery-gate shelf entries run at the very top of main(), long
 * before Step 7.  settings_subsys_init() is idempotent (-EALREADY on repeat
 * calls) so each entry point below simply ensures it has run.
 * -------------------------------------------------------------------------*/
static int ensure_settings_subsys(void)
{
	int rc = settings_subsys_init();

	if (rc != 0 && rc != -EALREADY)
	{
		LOG_ERR("settings_subsys_init failed: %d", rc);
		return rc;
	}
	return 0;
}

/* Direct single-key loader used before the full subtree load has happened. */
static int boot_count_direct_cb(const char *key, size_t len, settings_read_cb read_cb,
								void *cb_arg, void *param)
{
	uint32_t *out = param;

	ARG_UNUSED(key);
	if (len == sizeof(*out))
	{
		(void)read_cb(cb_arg, out, sizeof(*out));
	}
	return 0;
}

static int op_mode_direct_cb(const char *key, size_t len, settings_read_cb read_cb,
							 void *cb_arg, void *param)
{
	uint8_t *out = param;

	ARG_UNUSED(key);
	if (len == sizeof(*out))
	{
		(void)read_cb(cb_arg, out, sizeof(*out));
	}
	return 0;
}

enum juxta_op_mode juxta_settings_read_op_mode_early(void)
{
	if (ensure_settings_subsys() != 0)
	{
		return JUXTA_OP_MODE_SHELF;
	}

	uint8_t stored = (uint8_t)JUXTA_OP_MODE_SHELF;

	(void)settings_load_subtree_direct(SETTINGS_SUBTREE "/" SETTINGS_KEY_OP_MODE,
									   op_mode_direct_cb, &stored);

	if (stored != (uint8_t)JUXTA_OP_MODE_PROD && stored != (uint8_t)JUXTA_OP_MODE_DFU &&
		stored != (uint8_t)JUXTA_OP_MODE_SHELF)
	{
		return JUXTA_OP_MODE_SHELF;
	}
	return (enum juxta_op_mode)stored;
}

int juxta_settings_boot_count_increment(void)
{
	int rc = ensure_settings_subsys();

	if (rc != 0)
	{
		return rc;
	}

	uint32_t stored = 0U;

	(void)settings_load_subtree_direct(SETTINGS_SUBTREE "/" SETTINGS_KEY_BOOT_COUNT,
									   boot_count_direct_cb, &stored);
	if (stored > current_boot_count)
	{
		current_boot_count = stored;
	}

	current_boot_count++;
	rc = settings_save_one(SETTINGS_SUBTREE "/" SETTINGS_KEY_BOOT_COUNT,
						   &current_boot_count, sizeof(current_boot_count));
	if (rc != 0)
	{
		LOG_ERR("settings_save_one boot_count failed: %d", rc);
		return rc;
	}

	LOG_INF("boot_count := %u", current_boot_count);
	return 0;
}

uint32_t juxta_settings_boot_count(void)
{
	return current_boot_count;
}

int juxta_settings_save_breadcrumb(const struct juxta_breadcrumb *crumb)
{
	struct juxta_breadcrumb next;
	int rc;

	if (!crumb)
	{
		return -EINVAL;
	}

	rc = ensure_settings_subsys();
	if (rc != 0)
	{
		return rc;
	}

	next = *crumb;
	next.magic = JUXTA_BREADCRUMB_MAGIC;
	next.version = JUXTA_BREADCRUMB_VERSION;

	rc = settings_save_one(SETTINGS_SUBTREE "/" SETTINGS_KEY_BREADCRUMB, &next,
						   sizeof(next));
	if (rc != 0)
	{
		LOG_ERR("settings_save_one breadcrumb failed: %d", rc);
		return rc;
	}

	current_breadcrumb = next;
	loaded_breadcrumb = true;
	return 0;
}

int juxta_settings_load_breadcrumb(struct juxta_breadcrumb *out)
{
	if (!out)
	{
		return -EINVAL;
	}
	if (!loaded_breadcrumb)
	{
		return -ENOENT;
	}

	*out = current_breadcrumb;
	return 0;
}

int juxta_settings_clear_breadcrumb(void)
{
	memset(&current_breadcrumb, 0, sizeof(current_breadcrumb));
	loaded_breadcrumb = false;
	return settings_delete(SETTINGS_SUBTREE "/" SETTINGS_KEY_BREADCRUMB);
}
