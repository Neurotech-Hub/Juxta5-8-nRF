/*
 * Juxta5-8 BLE range test: minimal advertiser (non-connectable JX_XXXXXX) or scanner
 * (~1 s cadence, LED = peer seen). No GATT, battery, magnet, or sleep path.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/net_buf.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(juxta5_8_ble_range, LOG_LEVEL_INF);

#define DEVICE_ID_LEN 10U
#define SCAN_TIMEOUT_10MS 50U /* 500 ms passive window */
#define SCAN_CYCLE_MS     1000U

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/** Complete local name (JX_ + last 3 bytes of public identity, MSB..LSB). */
static char adv_name[DEVICE_ID_LEN];

#if IS_ENABLED(CONFIG_JUXTA_BLE_RANGE_ROLE_SCANNER)
static atomic_t peer_seen_in_window = ATOMIC_INIT(0);
#endif

/* Advertising data: flags + complete local name (no scan response, no GATT). */
static struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_NAME_COMPLETE, adv_name, 0),
};

static int fill_device_id(char *out, size_t out_sz)
{
	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(addrs);

	if (!out || out_sz < DEVICE_ID_LEN) {
		return -EINVAL;
	}

	bt_id_get(addrs, &count);
	if (count == 0U) {
		(void)snprintf(out, out_sz, "JX_000000");
		return 0;
	}

	(void)snprintf(out, out_sz, "JX_%02X%02X%02X", addrs[0].a.val[2], addrs[0].a.val[1],
		       addrs[0].a.val[0]);
	return 0;
}

#if !IS_ENABLED(CONFIG_JUXTA_BLE_RANGE_ROLE_SCANNER)
static int advertising_start(void)
{
	ad[1].data_len = (uint8_t)strlen(adv_name);

	int err = bt_le_adv_start(BT_LE_ADV_NCONN_IDENTITY, ad, ARRAY_SIZE(ad), NULL, 0);

	if (err != 0) {
		LOG_ERR("Advertising failed (%d)", err);
	} else {
		LOG_INF("Non-connectable advertising as \"%s\"", adv_name);
	}

	return err;
}
#endif

#if IS_ENABLED(CONFIG_JUXTA_BLE_RANGE_ROLE_SCANNER)
static bool name_matches_target(const char *dev_name)
{
	const char *want = CONFIG_JUXTA_RANGE_PEER_NAME;
	size_t want_len = strlen(want);

	if (want_len > 0U) {
		return strcmp(dev_name, want) == 0;
	}

	if (strlen(dev_name) != 9U || strncmp(dev_name, "JX_", 3) != 0) {
		return false;
	}
	for (size_t i = 3U; i < 9U; i++) {
		if (!isxdigit((unsigned char)dev_name[i])) {
			return false;
		}
	}
	return strcmp(dev_name, adv_name) != 0;
}

static void range_scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
			  struct net_buf_simple *ad_buf)
{
	char dev_name[DEVICE_ID_LEN];
	struct net_buf_simple_state state;

	ARG_UNUSED(addr);
	ARG_UNUSED(rssi);
	ARG_UNUSED(adv_type);

	if (!ad_buf || ad_buf->len == 0) {
		return;
	}

	memset(dev_name, 0, sizeof(dev_name));
	net_buf_simple_save(ad_buf, &state);
	while (ad_buf->len > 1) {
		uint8_t flen = net_buf_simple_pull_u8(ad_buf);

		if (flen == 0 || flen > ad_buf->len) {
			break;
		}
		uint8_t ftype = net_buf_simple_pull_u8(ad_buf);

		flen--;
		if (flen > ad_buf->len) {
			break;
		}
		if ((ftype == BT_DATA_NAME_COMPLETE || ftype == BT_DATA_NAME_SHORTENED) &&
		    flen < sizeof(dev_name)) {
			memcpy(dev_name, ad_buf->data, flen);
			dev_name[flen] = '\0';
		}
		net_buf_simple_pull(ad_buf, flen);
	}
	net_buf_simple_restore(ad_buf, &state);

	if (name_matches_target(dev_name)) {
		atomic_set(&peer_seen_in_window, 1);
	}
}

static void scanner_main_loop(void)
{
	struct bt_le_scan_param scan_param = {
		.type     = BT_LE_SCAN_TYPE_PASSIVE,
		.options  = BT_LE_SCAN_OPT_FILTER_DUPLICATE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window   = BT_GAP_SCAN_FAST_WINDOW,
		.timeout  = SCAN_TIMEOUT_10MS,
	};

	for (;;) {
		int err;

		atomic_set(&peer_seen_in_window, 0);

		err = bt_le_scan_start(&scan_param, range_scan_cb);
		if (err != 0 && err != -EALREADY) {
			LOG_ERR("Scan start failed: %d", err);
		} else {
			k_sleep(K_MSEC(10U * SCAN_TIMEOUT_10MS + 80U));
			(void)bt_le_scan_stop();
		}

		(void)gpio_pin_set_dt(&led, atomic_get(&peer_seen_in_window) ? 1 : 0);

		if (atomic_get(&peer_seen_in_window) != 0) {
			LOG_INF("Range: peer seen in window");
		} else {
			LOG_INF("Range: no peer in window");
		}

		uint32_t elapsed = 10U * SCAN_TIMEOUT_10MS + 80U;

		if (elapsed < SCAN_CYCLE_MS) {
			k_sleep(K_MSEC(SCAN_CYCLE_MS - elapsed));
		}
	}
}
#endif /* CONFIG_JUXTA_BLE_RANGE_ROLE_SCANNER */

int main(void)
{
	int err;

	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("LED GPIO not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (err != 0) {
		LOG_ERR("gpio_pin_configure_dt(led) failed: %d", err);
		return err;
	}

	err = bt_enable(NULL);
	if (err != 0) {
		LOG_ERR("Bluetooth init failed (%d)", err);
		return err;
	}

	(void)fill_device_id(adv_name, sizeof(adv_name));
	err = bt_set_name(adv_name);
	if (err != 0) {
		LOG_WRN("bt_set_name failed (%d), continuing", err);
	}

	LOG_INF("Role: %s, identity name \"%s\"",
#if IS_ENABLED(CONFIG_JUXTA_BLE_RANGE_ROLE_SCANNER)
		"SCANNER",
#else
		"ADVERTISER",
#endif
		adv_name);

#if IS_ENABLED(CONFIG_JUXTA_BLE_RANGE_ROLE_SCANNER)
	if (strlen(CONFIG_JUXTA_RANGE_PEER_NAME) > 0U) {
		LOG_INF("Scanner filter: exact name \"%s\"", CONFIG_JUXTA_RANGE_PEER_NAME);
	} else {
		LOG_INF("Scanner filter: any JX_XXXXXX except \"%s\"", adv_name);
	}
	scanner_main_loop();
	return 0;
#else
	err = advertising_start();
	if (err != 0) {
		return err;
	}

	for (;;) {
		k_sleep(K_SECONDS(60));
	}
#endif
}
