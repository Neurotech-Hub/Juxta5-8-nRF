#include "ble_service.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "juxta_prod.h"
#include "juxta_settings.h"
#include "juxta_time.h"

LOG_MODULE_REGISTER(juxta_ble_service, LOG_LEVEL_INF);

#define NODE_RESPONSE_MAX_SIZE 512
#define GATEWAY_COMMAND_MAX_SIZE 512

/* Incoming write: just a filename ("JXS20260508.csv" = 15 chars + null). */
#define FILENAME_WRITE_MAX_SIZE JUXTA_FILE_NAME_LEN

/* Outgoing listing: JUXTA_MAX_FILES entries, each up to ~24 chars ("name|size;").
 * 16 files × 24 chars = 384 chars; 512 gives comfortable headroom. */
#define FILENAME_LISTING_MAX_SIZE 512

static struct juxta_log_context *log_ctx;
static struct bt_conn *current_conn;
static bool production_ready;
static int32_t (*battery_mv_getter)(void);
static uint16_t current_mtu = 23;
static bool connected;
static bool transfer_active;
static bool transfer_eof_pending; /* awaiting ack for standalone "EOF" indication */
static bool indication_pending;
static char node_response[NODE_RESPONSE_MAX_SIZE];
static char gateway_command[GATEWAY_COMMAND_MAX_SIZE];
static char filename_write[FILENAME_WRITE_MAX_SIZE];	 /* received from iOS */
static char filename_listing[FILENAME_LISTING_MAX_SIZE]; /* sent to iOS */
static uint8_t transfer_chunk[JUXTA_TRANSFER_CHUNK_SIZE];
static struct juxta_file_entry transfer_entry;
static uint32_t transfer_offset;

static const struct bt_gatt_attr *filename_char_attr;
static const struct bt_gatt_attr *file_transfer_char_attr;
static struct bt_gatt_indicate_params filename_ind_params;
static struct bt_gatt_indicate_params file_transfer_ind_params;

static void continue_file_transfer(void);

void juxta_ble_set_battery_mv_source(int32_t (*getter)(void))
{
	battery_mv_getter = getter;
}

void juxta_ble_set_production_ready(void)
{
	production_ready = true;
}

/* Li-Po voltage to percent: linear 3000–4200 mV range.
 * Output is clamped to 0–100 so noise above full-charge voltage
 * or calibration drift never wraps the uint8 or returns >100. */
static uint8_t batt_mv_to_percent(int32_t mv)
{
	if (mv <= 3000)
	{
		return 0U;
	}
	int32_t pct = (mv - 3000) * 100 / 1200;

	return (pct >= 100) ? 100U : (uint8_t)pct;
}

int juxta_ble_get_device_id(char *device_id)
{
	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(addrs);

	if (!device_id)
	{
		return -EINVAL;
	}

	bt_id_get(addrs, &count);
	if (count == 0U)
	{
		(void)snprintf(device_id, JUXTA_DEVICE_ID_LEN, "JX_000000");
		return 0;
	}

	(void)snprintf(device_id, JUXTA_DEVICE_ID_LEN, "JX_%02X%02X%02X", addrs[0].a.val[2],
				   addrs[0].a.val[1], addrs[0].a.val[0]);
	return 0;
}

static int extract_string(const char *json, const char *key, char *out, size_t out_size)
{
	char pattern[40];
	const char *p;
	const char *start;
	const char *end;

	(void)snprintf(pattern, sizeof(pattern), "\"%s\":", key);
	p = strstr(json, pattern);
	if (!p)
	{
		return -ENOENT;
	}

	start = strchr(p + strlen(pattern), '"');
	if (!start)
	{
		return -EINVAL;
	}
	start++;
	end = strchr(start, '"');
	if (!end)
	{
		return -EINVAL;
	}

	size_t len = MIN((size_t)(end - start), out_size - 1U);
	memcpy(out, start, len);
	out[len] = '\0';
	return 0;
}

static int extract_u32(const char *json, const char *key, uint32_t *value)
{
	char pattern[40];
	const char *p;

	(void)snprintf(pattern, sizeof(pattern), "\"%s\":", key);
	p = strstr(json, pattern);
	if (!p)
	{
		return -ENOENT;
	}

	if (sscanf(p + strlen(pattern), "%u", value) != 1)
	{
		return -EINVAL;
	}
	return 0;
}

static bool extract_bool_true(const char *json, const char *key)
{
	char pattern[48];
	const char *p;

	(void)snprintf(pattern, sizeof(pattern), "\"%s\":", key);
	p = strstr(json, pattern);
	return p && (strstr(p, "true") == p + strlen(pattern) ||
				 strstr(p, " true") == p + strlen(pattern));
}

static int generate_node_response(char *buffer, size_t buffer_size)
{
	char device_id[JUXTA_DEVICE_ID_LEN];
	const struct juxta_settings *settings = juxta_settings_get();
	uint8_t battery_level = 0U;
	uint8_t memory_level = 0U;

	(void)juxta_ble_get_device_id(device_id);

	if (battery_mv_getter)
	{
		battery_level = batt_mv_to_percent(battery_mv_getter());
	}
	if (log_ctx)
	{
		memory_level = juxta_log_memory_level_percent(log_ctx);
	}

	int written = snprintf(buffer, buffer_size,
						   "{\"firmwareVersion\":\"%s\","
						   "\"batteryLevel\":%u,"
						   "\"memoryLevel\":%u,"
						   "\"deviceId\":\"%s\","
						   "\"subjectId\":\"%s\","
						   "\"experiment\":\"%s\","
						   "\"advInterval\":%u,"
						   "\"scanInterval\":%u,"
						   "\"inactivityMultiplier\":%u}",
						   JUXTA_FIRMWARE_VERSION, battery_level, memory_level, device_id,
						   settings->subject_id, settings->experiment, settings->adv_interval_s,
						   settings->scan_interval_s,
						   (unsigned int)settings->inactivity_multiplier);

	if (written < 0 || written >= (int)buffer_size)
	{
		return -ENOSPC;
	}

	return written;
}

static ssize_t read_node_char(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
							  uint16_t len, uint16_t offset)
{
	ARG_UNUSED(attr);

	int response_len = generate_node_response(node_response, sizeof(node_response));
	if (response_len < 0)
	{
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}

	return bt_gatt_attr_read(conn, attr, buf, len, offset, node_response, (uint16_t)response_len);
}

static int send_indication(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *data,
						   uint16_t len, bt_gatt_indicate_func_t func)
{
	struct bt_gatt_indicate_params *params;

	if (!conn || !attr || !data || len == 0U)
	{
		return -EINVAL;
	}
	if (indication_pending)
	{
		return -EBUSY;
	}

	params = (attr == filename_char_attr) ? &filename_ind_params : &file_transfer_ind_params;
	memset(params, 0, sizeof(*params));
	params->attr = attr;
	params->data = data;
	params->len = len;
	params->func = func;

	int rc = bt_gatt_indicate(conn, params);
	if (rc == 0)
	{
		indication_pending = true;
	}
	return rc;
}

static void filename_indication_confirmed(struct bt_conn *conn,
										  struct bt_gatt_indicate_params *params, uint8_t err)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(params);

	indication_pending = false;
	if (err != 0U)
	{
		LOG_WRN("filename indication failed: 0x%02x", err);
	}
}

static void file_transfer_indication_confirmed(struct bt_conn *conn,
											   struct bt_gatt_indicate_params *params, uint8_t err)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(params);

	indication_pending = false;
	if (err != 0U)
	{
		LOG_WRN("file transfer indication failed: 0x%02x", err);
		transfer_eof_pending = false;
		transfer_active = false;
		return;
	}

	if (transfer_eof_pending)
	{
		transfer_eof_pending = false;
		transfer_active = false;
		LOG_INF("transfer complete");
		return;
	}

	continue_file_transfer();
}

static int send_file_listing(struct bt_conn *conn)
{
	int len = juxta_log_list_files(log_ctx, filename_listing, sizeof(filename_listing));

	if (len < 0)
	{
		LOG_ERR("file listing failed: %d (buffer too small?)", len);
		return len;
	}

	LOG_INF("file listing (%d bytes): %s", len, filename_listing);

	if (conn && filename_char_attr)
	{
		return send_indication(conn, filename_char_attr, filename_listing, (uint16_t)len,
							   filename_indication_confirmed);
	}

	return 0;
}

static int start_transfer(const char *name)
{
	int rc = juxta_log_find_file(log_ctx, name, &transfer_entry);

	if (rc != 0)
	{
		return rc;
	}

	transfer_offset = 0;
	transfer_eof_pending = false;
	transfer_active = true;
	LOG_INF("transfer start %s payload=%u", transfer_entry.path,
			juxta_log_transfer_payload_bytes(&transfer_entry));
	return 0;
}

static void continue_file_transfer(void)
{
	size_t bytes_read = 0;
	size_t max_chunk = MIN(sizeof(transfer_chunk), current_mtu > 3U ? current_mtu - 3U : 20U);
	int rc;

	if (!transfer_active || !current_conn || !file_transfer_char_attr)
	{
		return;
	}

	rc = juxta_log_read_file_for_transfer(log_ctx, &transfer_entry, transfer_offset,
										  transfer_chunk, max_chunk, &bytes_read);
	if (rc != 0)
	{
		LOG_ERR("transfer read failed: %d", rc);
		transfer_eof_pending = false;
		transfer_active = false;
		return;
	}

	if (bytes_read == 0U)
	{
		uint32_t payload_len = juxta_log_transfer_payload_bytes(&transfer_entry);

		if (transfer_offset != payload_len)
		{
			LOG_ERR("transfer: stale offset=%u payload_len=%u", transfer_offset,
					payload_len);
			transfer_eof_pending = false;
			transfer_active = false;
			return;
		}

		memcpy(transfer_chunk, "EOF", 3);
		transfer_eof_pending = true;
		rc = send_indication(current_conn, file_transfer_char_attr, transfer_chunk, 3,
							 file_transfer_indication_confirmed);
		if (rc != 0)
		{
			LOG_WRN("EOF indication busy/failed: %d", rc);
			transfer_eof_pending = false;
			transfer_active = false;
		}
		return;
	}

	transfer_offset += bytes_read;
	rc = send_indication(current_conn, file_transfer_char_attr, transfer_chunk, (uint16_t)bytes_read,
						 file_transfer_indication_confirmed);
	if (rc != 0)
	{
		LOG_WRN("transfer indication busy/failed: %d", rc);
		transfer_eof_pending = false;
		transfer_active = false;
	}
}

static int apply_gateway_command(const char *json)
{
	struct juxta_settings next = *juxta_settings_get();
	char device_id[JUXTA_DEVICE_ID_LEN];
	uint32_t value;
	bool changed = false;

	(void)juxta_ble_get_device_id(device_id);

	if (extract_u32(json, "timestamp", &value) == 0)
	{
		/* Always set the clock — needed for file naming and row timestamps. */
		juxta_time_set(value);
		LOG_INF("gateway timestamp=%u", value);
		/* Log only in production; during the sync phase no files exist yet. */
		if (production_ready)
		{
			(void)juxta_log_append_event(log_ctx, &next, device_id, "time_set", value);
		}
		juxta_ble_datetime_synchronized();
	}

	if (extract_bool_true(json, "sendFilenames"))
	{
		(void)send_file_listing(current_conn);
	}

	if (extract_bool_true(json, "clearMemory"))
	{
		/* Always honored — explicit destructive action, not gated by production_ready.
		 * Erase only; file creation is deferred to the next data write (ProductionInit
		 * "boot" event or first vitals/scan row), consistent with the init path. */
		LOG_INF("clearMemory: erasing all NOR CSV regions");
		(void)juxta_log_format(log_ctx);
	}

	if (extract_bool_true(json, "reset"))
	{
		/* Disconnect BLE and enter shelf mode (System OFF in production,
		 * soft reboot in debug).  Implemented in main.c. */
		LOG_INF("reset: entering shelf mode on request");
		juxta_ble_reset_requested(); /* does not return */
	}

	if (extract_u32(json, "scanInterval", &value) == 0 && value <= UINT16_MAX)
	{
		if (value > JUXTA_MAX_BLE_INTERVAL_S)
		{
			LOG_WRN("gateway scanInterval=%u clamped to %u", value, JUXTA_MAX_BLE_INTERVAL_S);
			value = JUXTA_MAX_BLE_INTERVAL_S;
		}
		next.scan_interval_s = (uint16_t)value;
		changed = true;
	}
	if (extract_u32(json, "advInterval", &value) == 0 && value <= UINT16_MAX)
	{
		if (value > JUXTA_MAX_BLE_INTERVAL_S)
		{
			LOG_WRN("gateway advInterval=%u clamped to %u", value, JUXTA_MAX_BLE_INTERVAL_S);
			value = JUXTA_MAX_BLE_INTERVAL_S;
		}
		next.adv_interval_s = (uint16_t)value;
		changed = true;
	}

	if (extract_u32(json, "inactivityMultiplier", &value) == 0)
	{
		if (value < JUXTA_DEFAULT_INACTIVITY_MULTIPLIER)
		{
			LOG_WRN("gateway inactivityMultiplier=%u clamped to %u", value,
				JUXTA_DEFAULT_INACTIVITY_MULTIPLIER);
			value = JUXTA_DEFAULT_INACTIVITY_MULTIPLIER;
		}
		else if (value > JUXTA_MAX_INACTIVITY_MULTIPLIER)
		{
			LOG_WRN("gateway inactivityMultiplier=%u clamped to %u", value,
				JUXTA_MAX_INACTIVITY_MULTIPLIER);
			value = JUXTA_MAX_INACTIVITY_MULTIPLIER;
		}
		next.inactivity_multiplier = (uint8_t)value;
		changed = true;
	}
	if (extract_u32(json, "vitalsInterval", &value) == 0 && value > 0U &&
		value <= UINT16_MAX)
	{
		next.vitals_interval_s = (uint16_t)value;
		changed = true;
	}
	if (extract_string(json, "subjectId", next.subject_id, sizeof(next.subject_id)) == 0)
	{
		changed = true;
	}
	if (extract_string(json, "experiment", next.experiment, sizeof(next.experiment)) == 0)
	{
		changed = true;
	}

	if (changed)
	{
		int rc = juxta_settings_update(&next);

		if (rc != 0)
		{
			return rc;
		}
		const struct juxta_settings *a = juxta_settings_get();

		LOG_INF("gateway settings saved: scan=%u adv=%u vitals=%u inactivity_multiplier=%u "
				"subject=\"%s\" experiment=\"%s\"",
				a->scan_interval_s, a->adv_interval_s, a->vitals_interval_s,
				(unsigned int)a->inactivity_multiplier, a->subject_id, a->experiment);
		if (production_ready)
		{
			(void)juxta_log_append_event(log_ctx, a, device_id, "settings_changed",
						     juxta_time_now());
		}
		juxta_ble_timing_update_trigger();
	}

	return 0;
}

static ssize_t write_gateway_char(struct bt_conn *conn, const struct bt_gatt_attr *attr,
								  const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset + len >= sizeof(gateway_command))
	{
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	memcpy(gateway_command + offset, buf, len);
	gateway_command[offset + len] = '\0';

	int rc = apply_gateway_command(gateway_command);
	if (rc != 0)
	{
		LOG_ERR("gateway command failed: %d", rc);
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}

	return len;
}

static ssize_t read_filename_char(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
								  uint16_t len, uint16_t offset)
{
	ARG_UNUSED(attr);

	int listing_len = juxta_log_list_files(log_ctx, filename_listing, sizeof(filename_listing));
	if (listing_len < 0)
	{
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}

	return bt_gatt_attr_read(conn, attr, buf, len, offset, filename_listing,
							 (uint16_t)listing_len);
}

static ssize_t write_filename_char(struct bt_conn *conn, const struct bt_gatt_attr *attr,
								   const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0U || len >= sizeof(filename_write))
	{
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	memcpy(filename_write, buf, len);
	filename_write[len] = '\0';

	if (strcmp(filename_write, "LIST") == 0 || strcmp(filename_write, "FILENAMES") == 0)
	{
		int rc = send_file_listing(conn);
		return rc == 0 ? len : BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}

	int rc = start_transfer(filename_write);
	if (rc != 0)
	{
		LOG_WRN("file not found for transfer: %s", filename_write);
		return BT_GATT_ERR(BT_ATT_ERR_ATTRIBUTE_NOT_FOUND);
	}

	continue_file_transfer();
	return len;
}

static ssize_t read_file_transfer_char(struct bt_conn *conn, const struct bt_gatt_attr *attr,
									   void *buf, uint16_t len, uint16_t offset)
{
	ARG_UNUSED(attr);

	if (!transfer_active)
	{
		return 0;
	}

	size_t bytes_read = 0;
	int rc = juxta_log_read_file_for_transfer(log_ctx, &transfer_entry, offset, buf, len,
											  &bytes_read);
	if (rc != 0)
	{
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}
	return (ssize_t)bytes_read;
}

static void file_transfer_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	LOG_INF("file transfer indications %s", value == BT_GATT_CCC_INDICATE ? "enabled" : "off");
}

static void filename_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	LOG_INF("filename indications %s", value == BT_GATT_CCC_INDICATE ? "enabled" : "off");
}

BT_GATT_SERVICE_DEFINE(juxta_hublink_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_JUXTA_HUBLINK_SERVICE),
					   BT_GATT_CHARACTERISTIC(BT_UUID_JUXTA_NODE_CHAR, BT_GATT_CHRC_READ,
											  BT_GATT_PERM_READ, read_node_char, NULL, NULL),
					   BT_GATT_CHARACTERISTIC(BT_UUID_JUXTA_GATEWAY_CHAR,
											  BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
											  BT_GATT_PERM_WRITE, NULL, write_gateway_char, NULL),
					   BT_GATT_CHARACTERISTIC(BT_UUID_JUXTA_FILENAME_CHAR,
											  BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE |
												  BT_GATT_CHRC_INDICATE,
											  BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
											  read_filename_char, write_filename_char, NULL),
					   BT_GATT_CCC(filename_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
					   BT_GATT_CHARACTERISTIC(BT_UUID_JUXTA_FILE_TRANSFER_CHAR,
											  BT_GATT_CHRC_READ | BT_GATT_CHRC_INDICATE,
											  BT_GATT_PERM_READ, read_file_transfer_char, NULL,
											  NULL),
					   BT_GATT_CCC(file_transfer_ccc_changed,
								   BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

int juxta_ble_service_init(struct juxta_log_context *ctx)
{
	log_ctx = ctx;
	filename_char_attr = bt_gatt_find_by_uuid(juxta_hublink_svc.attrs,
											  juxta_hublink_svc.attr_count,
											  BT_UUID_JUXTA_FILENAME_CHAR);
	file_transfer_char_attr = bt_gatt_find_by_uuid(juxta_hublink_svc.attrs,
												   juxta_hublink_svc.attr_count,
												   BT_UUID_JUXTA_FILE_TRANSFER_CHAR);
	if (!filename_char_attr || !file_transfer_char_attr)
	{
		return -ENOENT;
	}

	LOG_INF("Hublink service ready");
	return 0;
}

static void mtu_exchange_cb(struct bt_conn *conn, uint8_t err,
							struct bt_gatt_exchange_params *params)
{
	ARG_UNUSED(params);

	if (err != 0U)
	{
		LOG_WRN("MTU exchange failed: %u", err);
	}
	else
	{
		current_mtu = bt_gatt_get_mtu(conn);
		LOG_INF("MTU exchanged: %u bytes (chunk size: %u)",
				current_mtu, (uint16_t)(current_mtu > 3U ? current_mtu - 3U : 20U));
	}
}

static struct bt_gatt_exchange_params mtu_exchange_params = {
	.func = mtu_exchange_cb,
};

/* Preferred connection parameters:
 *   interval 30-50 ms  — good balance of latency and power
 *   slave latency 0    — peripheral responds every event (needed for indications)
 *   supervision 4000ms — generous timeout; prevents spurious 0x08 disconnects
 *                        during long NOR flash erase operations (~3.5 s observed) */
/* BT_LE_CONN_PARAM expands to a compound literal and cannot be used as a
 * static initializer — initialize the struct fields directly instead. */
static const struct bt_le_conn_param hublink_conn_params = {
	.interval_min = 24U, /* 24 * 1.25 ms = 30 ms */
	.interval_max = 40U, /* 40 * 1.25 ms = 50 ms */
	.latency = 0U,		 /* no slave latency */
	.timeout = 400U,	 /* 400 * 10 ms = 4000 ms supervision timeout */
};

void juxta_ble_connection_established(struct bt_conn *conn)
{
	current_conn = bt_conn_ref(conn);
	current_mtu = bt_gatt_get_mtu(conn);
	connected = true;
	LOG_INF("Hublink connected mtu=%u", current_mtu);

	/* Initiate MTU exchange — with CONFIG_BT_L2CAP_TX_MTU=247 we can push
	 * up to 244-byte indication payloads, drastically reducing packet count
	 * during file transfers and lowering supervision-timeout exposure. */
	(void)bt_gatt_exchange_mtu(conn, &mtu_exchange_params);

	/* Request longer supervision timeout and moderate connection interval.
	 * The central (iOS) may accept or ignore this; we log the result via
	 * the standard on_disconnected reason code. */
	int err = bt_conn_le_param_update(conn, &hublink_conn_params);
	if (err != 0)
	{
		LOG_WRN("Connection param update request failed: %d", err);
	}
}

void juxta_ble_connection_terminated(void)
{
	if (current_conn)
	{
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
	connected = false;
	transfer_eof_pending = false;
	transfer_active = false;
	indication_pending = false;
}

int juxta_ble_get_status(uint16_t *mtu, bool *is_connected, bool *is_transfer_active)
{
	if (mtu)
	{
		*mtu = current_mtu;
	}
	if (is_connected)
	{
		*is_connected = connected;
	}
	if (is_transfer_active)
	{
		*is_transfer_active = transfer_active;
	}
	return 0;
}
