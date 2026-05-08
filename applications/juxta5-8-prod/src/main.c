/*
 * Juxta5-8 Production Firmware
 *
 * Boot sequence:
 *   Fresh power-on (RESETREAS == 0) → System OFF (shelf mode), no LED.
 *   System OFF wake (RESETREAS[OFF]) → LED ON while magnet held:
 *     < 7 s  → slow blink (50 ms ON / 450 ms OFF) → connectable adv + datetime sync
 *              → must receive timestamp before entering production
 *     ≥ 7 s  → 3× blink, then fast blink (50/50 ms) → DFU mode stub
 *              → 5 s debounce, then magnet swipe returns to shelf
 *   Other reset (watchdog / pin / lockup) → straight to production, no LED gate.
 *
 * Connected state: LED solid ON.
 * Sync success + disconnect: 5× blink, LED off, then production init.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <nrf.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include "ble_service.h"
#include "juxta_log.h"
#include "juxta_prod.h"
#include "juxta_settings.h"
#include "juxta_time.h"
#include "lis2dh12_zephyr.h"

LOG_MODULE_REGISTER(juxta5_8_prod, LOG_LEVEL_INF);

BUILD_ASSERT(JUXTA_DEVICE_ID_LEN >= 10, "JUXTA_DEVICE_ID_LEN too small");

/* ---------------------------------------------------------------------------
 * Board peripherals
 * -------------------------------------------------------------------------*/
#define LED_NODE       DT_ALIAS(led0)
#define MAG_NODE       DT_ALIAS(magnet_sensor)
#define ACCEL_INT_NODE DT_ALIAS(accel_int)

static const struct gpio_dt_spec led       = GPIO_DT_SPEC_GET(LED_NODE, gpios);
static const struct gpio_dt_spec magnet    = GPIO_DT_SPEC_GET(MAG_NODE, gpios);
static const struct gpio_dt_spec accel_int = GPIO_DT_SPEC_GET(ACCEL_INT_NODE, gpios);

/* Battery ADC — VBATT = VADC * 7.96 (calibrated from hardware measurements).
 * Nominal factor for 1.5 MΩ / 220 kΩ divider is 7.82, but that under-reads
 * ~2% on this board (4.2 V → 94%, 3.5 V → 36%).  7.96 gives ≈100% / ≈41%. */
#define FUEL_DIV_NUM 796L
#define FUEL_DIV_DEN 100L
static const struct adc_dt_spec fuel = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));

/* ---------------------------------------------------------------------------
 * LED mode state machine
 *
 * Uses a kernel timer + work item so LED patterns work from any context
 * including the main thread blocking loops.
 * -------------------------------------------------------------------------*/
typedef enum {
	LED_MODE_OFF = 0,
	LED_MODE_ON,         /* solid ON — connected */
	LED_MODE_SLOW_BLINK, /* 50 ms ON / 450 ms OFF — gateway adv waiting */
	LED_MODE_FAST_BLINK, /* 50 ms ON / 50 ms OFF  — DFU waiting */
} led_mode_t;

static led_mode_t led_mode;
static bool led_phase;
static struct k_work led_work;
static struct k_timer led_timer;

static void led_timer_cb(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_work_submit(&led_work);
}

static void led_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	switch (led_mode) {
	case LED_MODE_OFF:
		gpio_pin_set_dt(&led, 0);
		break;
	case LED_MODE_ON:
		gpio_pin_set_dt(&led, 1);
		break;
	case LED_MODE_SLOW_BLINK:
		if (led_phase) {
			gpio_pin_set_dt(&led, 1);
			led_phase = false;
			k_timer_start(&led_timer, K_MSEC(50), K_NO_WAIT);
		} else {
			gpio_pin_set_dt(&led, 0);
			led_phase = true;
			k_timer_start(&led_timer, K_MSEC(450), K_NO_WAIT);
		}
		break;
	case LED_MODE_FAST_BLINK:
		led_phase = !led_phase;
		gpio_pin_set_dt(&led, led_phase ? 1 : 0);
		k_timer_start(&led_timer, K_MSEC(50), K_NO_WAIT);
		break;
	}
}

static void set_led_mode(led_mode_t mode)
{
	k_timer_stop(&led_timer);
	led_mode = mode;
	led_phase = true;
	k_work_submit(&led_work);
}

/* Blocking blink sequence; stops any running LED mode first. */
static void led_blink(uint8_t count, uint32_t on_ms, uint32_t off_ms)
{
	k_timer_stop(&led_timer);
	gpio_pin_set_dt(&led, 0);
	for (uint8_t i = 0; i < count; i++) {
		gpio_pin_set_dt(&led, 1);
		k_sleep(K_MSEC(on_ms));
		gpio_pin_set_dt(&led, 0);
		if (i < count - 1U) {
			k_sleep(K_MSEC(off_ms));
		}
	}
}

/* ---------------------------------------------------------------------------
 * Watchdog
 * -------------------------------------------------------------------------*/
static const struct device *wdt;
static int wdt_channel_id = -1;
static struct k_timer wdt_feed_timer;

static void wdt_feed_timer_cb(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	if (wdt && wdt_channel_id >= 0) {
		(void)wdt_feed(wdt, wdt_channel_id);
	}
}

static int init_watchdog(void)
{
	const struct wdt_timeout_cfg cfg = {
		.window = {.min = 0U, .max = 30000U},
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};

	wdt = DEVICE_DT_GET(DT_NODELABEL(wdt0));
	if (!device_is_ready(wdt)) {
		LOG_ERR("Watchdog not ready");
		return -ENODEV;
	}

	wdt_channel_id = wdt_install_timeout(wdt, &cfg);
	if (wdt_channel_id < 0) {
		return wdt_channel_id;
	}

	int rc = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (rc != 0) {
		return rc;
	}

	k_timer_init(&wdt_feed_timer, wdt_feed_timer_cb, NULL);
	k_timer_start(&wdt_feed_timer, K_MSEC(10000), K_MSEC(10000));
	LOG_INF("Watchdog: 30 s window");
	return 0;
}

/* ---------------------------------------------------------------------------
 * Battery
 * -------------------------------------------------------------------------*/
static int32_t g_batt_mv;

static int32_t batt_mv_get(void)
{
	return g_batt_mv;
}

static int sample_battery_mv(void)
{
	uint16_t raw;
	struct adc_sequence seq = {.buffer = &raw, .buffer_size = sizeof(raw)};
	int32_t val_mv;
	int err;

	err = adc_sequence_init_dt(&fuel, &seq);
	if (err != 0) {
		return err;
	}
	err = adc_read_dt(&fuel, &seq);
	if (err != 0) {
		return err;
	}
	val_mv = (int32_t)raw;
	err = adc_raw_to_millivolts_dt(&fuel, &val_mv);
	if (err != 0) {
		return err;
	}
	g_batt_mv = (val_mv * FUEL_DIV_NUM) / FUEL_DIV_DEN;
	return 0;
}

/* ---------------------------------------------------------------------------
 * Shelf mode (System OFF in production; soft reboot in debug)
 *
 * When the J-Link debugger is active sys_poweroff() does not reliably enter
 * System OFF. We substitute sys_reboot() so that the debug simulation loop
 * in main() restarts and can poll for the next magnet application.
 *
 * In production (no debugger): 1× short blink confirms the path was reached,
 * then MAG_INT is armed as a GPIO_INT_LEVEL_ACTIVE wake source and the nRF52840
 * enters System OFF. Cold boot on magnet application, RESETREAS[OFF] set.
 * -------------------------------------------------------------------------*/
static void enter_shelf_mode(void)
{
	bool debug = (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) != 0U;

	k_sleep(K_MSEC(30)); /* flush RTT */

	if (debug) {
		LOG_INF("[DBG] Shelf mode: soft reboot to restart simulation loop");
		k_sleep(K_MSEC(10));
		sys_reboot(SYS_REBOOT_COLD); /* triggers debugger path on restart */
		/* Does not return */
	}

	LOG_INF("Shelf mode: entering System OFF — wake on magnet (P0.25 LOW)");

	/* 1× short blink confirms this path was reached (visible on hardware). */
	gpio_pin_set_dt(&led, 1);
	k_sleep(K_MSEC(50));
	gpio_pin_set_dt(&led, 0);
	k_sleep(K_MSEC(20));

	gpio_pin_configure_dt(&magnet, GPIO_INPUT);
	int err = gpio_pin_interrupt_configure_dt(&magnet, GPIO_INT_LEVEL_ACTIVE);
	if (err != 0) {
		LOG_ERR("MAG wake sense cfg failed (%d) — not entering System OFF", err);
		for (;;) {
			k_sleep(K_SECONDS(1));
		}
	}

	LOG_INF("Shelf mode: MAG sense armed — calling sys_poweroff()");
	k_sleep(K_MSEC(10));
	sys_poweroff();
	/* Does not return */
}

/* ---------------------------------------------------------------------------
 * Boot magnet hold measurement
 *
 * LED is turned ON before calling this so the user sees confirmation that
 * the magnet wake was detected. LED stays ON for the full hold duration.
 * -------------------------------------------------------------------------*/
#define DFU_HOLD_THRESHOLD_MS 7000U

static int64_t measure_magnet_hold(void)
{
	int64_t t0 = k_uptime_get();

	/* gpio_pin_get_dt returns non-zero while magnet is active (present) */
	while (gpio_pin_get_dt(&magnet) != 0) {
		k_sleep(K_MSEC(50));
		if (k_uptime_get() - t0 >= (int64_t)DFU_HOLD_THRESHOLD_MS) {
			break;
		}
	}

	return k_uptime_get() - t0;
}

/* ---------------------------------------------------------------------------
 * DFU mode (MCUboot SMP BLE — stub)
 *
 * Blinks 3× on entry, then fast-blinks until connected.
 * After a 5 s debounce, a magnet swipe returns to shelf mode.
 * -------------------------------------------------------------------------*/
static void enter_dfu_mode(void)
{
	/* 3 entry blinks */
	led_blink(3U, 200U, 200U);

	/* Fast blink: DFU waiting for connection */
	set_led_mode(LED_MODE_FAST_BLINK);

	LOG_WRN("DFU mode: MCUboot SMP BLE not yet implemented");

	/* 5 s debounce — magnet was just released after long hold */
	k_sleep(K_SECONDS(5));

	/* Monitor for another magnet swipe to return to shelf */
	LOG_INF("DFU: debounce done — magnet swipe will return to shelf");
	for (;;) {
		if (gpio_pin_get_dt(&magnet) != 0) {
			LOG_INF("DFU: magnet re-applied — returning to shelf");
			set_led_mode(LED_MODE_OFF);
			k_sleep(K_MSEC(20));
			enter_shelf_mode();
		}
		k_sleep(K_MSEC(100));
	}
}

/* ---------------------------------------------------------------------------
 * Motion / LIS2DH12
 * -------------------------------------------------------------------------*/
static struct lis2dh12_dev lis2dh12;
static struct gpio_callback accel_int_cb;
static volatile uint32_t motion_events;

static void accel_int_callback(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	motion_events++;
	/* LOG_DBG is ISR-safe with Zephyr's deferred logger and won't appear
	 * at the default INFO level — change log level to DEBUG to see per-event. */
	LOG_DBG("Motion interrupt #%u", motion_events);
}

static int init_accel(void)
{
	int rc = lis2dh12_zephyr_init(&lis2dh12);
	if (rc != 0) {
		return rc;
	}
	rc = lis2dh12_zephyr_config_motion(&lis2dh12, 10U, 0U);
	if (rc != 0) {
		return rc;
	}
	gpio_init_callback(&accel_int_cb, accel_int_callback, BIT(accel_int.pin));
	rc = gpio_add_callback(accel_int.port, &accel_int_cb);
	rc |= gpio_pin_interrupt_configure_dt(&accel_int, GPIO_INT_EDGE_TO_ACTIVE);
	return rc;
}

/* ---------------------------------------------------------------------------
 * Device ID
 * -------------------------------------------------------------------------*/
static char device_id[JUXTA_DEVICE_ID_LEN];
static char adv_name[JUXTA_DEVICE_ID_LEN];

static void derive_device_id(void)
{
	(void)juxta_ble_get_device_id(device_id);
	(void)snprintf(adv_name, sizeof(adv_name), "%s", device_id);
	LOG_INF("Device ID: %s", device_id);
}

/* ---------------------------------------------------------------------------
 * Global application state
 * -------------------------------------------------------------------------*/
static struct juxta_log_context log_ctx;
static volatile bool datetime_synchronized;
static volatile bool ble_connected;
static bool hardware_ready;

/* ---------------------------------------------------------------------------
 * Vitals (once per vitals_interval_s)
 * -------------------------------------------------------------------------*/
static struct k_work vitals_work;
static struct k_timer vitals_timer;

static void vitals_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!hardware_ready) {
		return;
	}

	uint32_t now = juxta_time_now();
	int8_t temp_c = 0;

	(void)sample_battery_mv();
	(void)lis2dh12_zephyr_read_temp_c(&lis2dh12, &temp_c);

	uint8_t motion = (uint8_t)MIN(motion_events, 255U);
	motion_events = 0;

	(void)juxta_log_append_vitals(&log_ctx, now, motion, g_batt_mv, temp_c);
}

static void vitals_timer_cb(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_work_submit(&vitals_work);
}

/* ---------------------------------------------------------------------------
 * BLE scan callback (ISR context — post to queue)
 * -------------------------------------------------------------------------*/
#define MAX_JUXTA_SCAN_ENTRIES 64
typedef struct {
	char peer_id[JUXTA_DEVICE_ID_LEN];
	int8_t rssi;
} scan_entry_t;
static scan_entry_t scan_table[MAX_JUXTA_SCAN_ENTRIES];
static uint8_t scan_count;

#define SCAN_EVENT_QUEUE_SIZE 16
typedef struct {
	char peer_id[JUXTA_DEVICE_ID_LEN];
	int8_t rssi;
} scan_event_t;
K_MSGQ_DEFINE(scan_event_q, sizeof(scan_event_t), SCAN_EVENT_QUEUE_SIZE, 4);

static bool do_gateway_adv;

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
		    struct net_buf_simple *ad)
{
	ARG_UNUSED(adv_type);
	ARG_UNUSED(addr);

	if (!ad || ad->len == 0) {
		return;
	}

	char dev_name[JUXTA_DEVICE_ID_LEN] = {0};
	struct net_buf_simple_state state;

	net_buf_simple_save(ad, &state);
	while (ad->len > 1) {
		uint8_t flen = net_buf_simple_pull_u8(ad);

		if (flen == 0 || flen > ad->len) {
			break;
		}
		uint8_t ftype = net_buf_simple_pull_u8(ad);

		flen--;
		if (flen > ad->len) {
			break;
		}
		if ((ftype == BT_DATA_NAME_COMPLETE || ftype == BT_DATA_NAME_SHORTENED) &&
		    flen < sizeof(dev_name)) {
			memcpy(dev_name, ad->data, flen);
			dev_name[flen] = '\0';
		}
		net_buf_simple_pull(ad, flen);
	}
	net_buf_simple_restore(ad, &state);

	if (strncmp(dev_name, "JX_", 3) == 0 && strlen(dev_name) == 9) {
		scan_event_t evt;

		memset(&evt, 0, sizeof(evt));
		(void)snprintf(evt.peer_id, sizeof(evt.peer_id), "%s", dev_name);
		evt.rssi = rssi;
		(void)k_msgq_put(&scan_event_q, &evt, K_NO_WAIT);
	} else if (strncmp(dev_name, "JXGA_", 5) == 0) {
		do_gateway_adv = true;
	}
}

/* ---------------------------------------------------------------------------
 * BLE connection callbacks
 * -------------------------------------------------------------------------*/
static void on_connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	if (err != 0U) {
		LOG_ERR("Connect failed %s err=0x%02x", addr, err);
		return;
	}

	LOG_INF("Connected %s", addr);
	ble_connected = true;
	(void)sample_battery_mv(); /* refresh for Node characteristic read */
	set_led_mode(LED_MODE_ON); /* solid LED while connected */
	juxta_ble_connection_established(conn);

	if (hardware_ready) {
		(void)juxta_log_append_event(&log_ctx, juxta_settings_get(), device_id,
					     "user_connected", juxta_time_now());
	}
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Disconnected %s reason=0x%02x", addr, reason);
	ble_connected = false;
	juxta_ble_connection_terminated();

	if (hardware_ready) {
		/* Production: LED off, log event, resume state machine */
		set_led_mode(LED_MODE_OFF);
		(void)juxta_log_append_event(&log_ctx, juxta_settings_get(), device_id,
					     "user_disconnected", juxta_time_now());
	}
	/* During sync phase: LED transitions are handled by wait_for_datetime_sync() */
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
};

/* ---------------------------------------------------------------------------
 * Advertising / scanning helpers
 * -------------------------------------------------------------------------*/
static struct bt_data adv_data[2];
static struct bt_data sd_data[1];

static int start_connectable_adv(void)
{
	adv_data[0] = (struct bt_data)BT_DATA_BYTES(BT_DATA_FLAGS,
						     BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR);
	adv_data[1] = (struct bt_data)BT_DATA_BYTES(BT_DATA_UUID128_ALL,
						     JUXTA_HUBLINK_SERVICE_UUID);
	sd_data[0]  = (struct bt_data)BT_DATA(BT_DATA_NAME_COMPLETE, adv_name, strlen(adv_name));

	int rc = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, adv_data, ARRAY_SIZE(adv_data),
				 sd_data, ARRAY_SIZE(sd_data));

	if (rc != 0) {
		LOG_ERR("Connectable adv start failed: %d", rc);
	} else {
		LOG_INF("Connectable advertising started as %s", adv_name);
	}
	return rc;
}

static int start_nonconn_adv(void)
{
	struct bt_le_adv_param adv_param = {
		.id = BT_ID_DEFAULT,
		.options = 0,
		.interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
		.interval_max = BT_GAP_ADV_FAST_INT_MAX_2,
		.peer = NULL,
	};
	struct bt_data nc_adv[] = {
		BT_DATA(BT_DATA_NAME_COMPLETE, adv_name, strlen(adv_name)),
	};
	int rc = bt_le_adv_start(&adv_param, nc_adv, ARRAY_SIZE(nc_adv), NULL, 0);

	if (rc != 0) {
		LOG_ERR("Non-conn adv start failed: %d", rc);
	} else {
		LOG_INF("Advertising as %s", adv_name);
	}
	return rc;
}

static int start_scanning(void)
{
	struct bt_le_scan_param scan_param = {
		.type    = BT_LE_SCAN_TYPE_PASSIVE,
		.options = BT_LE_SCAN_OPT_FILTER_DUPLICATE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window   = BT_GAP_SCAN_FAST_WINDOW,
		.timeout  = 0,
	};

	(void)bt_le_adv_stop();
	k_sleep(K_MSEC(100));

	int rc = bt_le_scan_start(&scan_param, scan_cb);

	if (rc != 0) {
		LOG_ERR("Scan start failed: %d", rc);
	} else {
		LOG_INF("Scanning started");
	}
	return rc;
}

/* ---------------------------------------------------------------------------
 * Datetime sync gate
 *
 * Runs before production init on a shelf-wake boot. Loops indefinitely
 * (no timeout) until iOS sends a valid timestamp. LED behavior:
 *   - Waiting for connection: slow blink (50 ms ON / 450 ms OFF)
 *   - Connected:              solid ON (set by on_connected callback)
 *   - Disconnected without timestamp: resume slow blink, restart advertising
 *   - Timestamp received + disconnected: 5× blink then LED off
 * -------------------------------------------------------------------------*/
static void wait_for_datetime_sync(void)
{
	LOG_INF("Datetime sync: starting connectable advertising");
	set_led_mode(LED_MODE_SLOW_BLINK);
	(void)start_connectable_adv();

	while (!datetime_synchronized) {
		/* Wait for a connection (or timestamp arriving before connect, unlikely) */
		while (!ble_connected && !datetime_synchronized) {
			k_sleep(K_MSEC(100));
		}

		if (datetime_synchronized) {
			break;
		}

		/* Connected — LED is solid ON via on_connected callback */
		/* Wait until disconnected or timestamp received */
		while (ble_connected && !datetime_synchronized) {
			k_sleep(K_MSEC(100));
		}

		if (!datetime_synchronized) {
			/* Disconnected without timestamp — restart advertising */
			LOG_WRN("Datetime sync: disconnected without timestamp, retrying");
			set_led_mode(LED_MODE_SLOW_BLINK);
			k_sleep(K_MSEC(500));
			(void)bt_le_adv_stop();
			(void)start_connectable_adv();
		}
	}

	/* Timestamp received — wait for disconnect if still connected */
	if (ble_connected) {
		while (ble_connected) {
			k_sleep(K_MSEC(50));
		}
	}

	/* 5× blink signals successful sync, then LED off for production */
	LOG_INF("Datetime sync: success — 5 blinks then production init");
	led_blink(5U, 50U, 100U);
	set_led_mode(LED_MODE_OFF);
}

/* ---------------------------------------------------------------------------
 * Post-scan: write JXB rows for unique peers
 * -------------------------------------------------------------------------*/
static void flush_scan_table(void)
{
	uint32_t now = juxta_time_now();

	for (uint8_t i = 0; i < scan_count; i++) {
		(void)juxta_log_append_ble_observation(&log_ctx, now, device_id,
						       scan_table[i].peer_id,
						       scan_table[i].rssi);
	}

	LOG_INF("Scan burst: %u unique peers", scan_count);
	scan_count = 0;
	memset(scan_table, 0, sizeof(scan_table));
}

static void process_scan_events(void)
{
	scan_event_t evt;

	while (k_msgq_get(&scan_event_q, &evt, K_NO_WAIT) == 0) {
		bool found = false;

		for (uint8_t i = 0; i < scan_count; i++) {
			if (strcmp(scan_table[i].peer_id, evt.peer_id) == 0) {
				if (evt.rssi > scan_table[i].rssi) {
					scan_table[i].rssi = evt.rssi;
				}
				found = true;
				break;
			}
		}
		if (!found && scan_count < MAX_JUXTA_SCAN_ENTRIES) {
			(void)snprintf(scan_table[scan_count].peer_id,
				       sizeof(scan_table[scan_count].peer_id), "%s", evt.peer_id);
			scan_table[scan_count].rssi = evt.rssi;
			scan_count++;
		}
	}
}

/* ---------------------------------------------------------------------------
 * Operational state machine
 * -------------------------------------------------------------------------*/
#define ADV_BURST_MS   500U
#define SCAN_BURST_MS  3000U
#define GATEWAY_ADV_MS 30000U

typedef enum {
	BLE_STATE_IDLE = 0,
	BLE_STATE_ADVERTISING,
	BLE_STATE_SCANNING,
	BLE_STATE_CONNECTABLE_ADV,
} ble_state_t;

static ble_state_t ble_state = BLE_STATE_IDLE;
static uint32_t last_adv_ts;
static uint32_t last_scan_ts;
static struct k_work state_work;
static struct k_timer state_timer;

/* Advertising interval is fixed: we broadcast our identity once every
 * ADV_INTERVAL_S seconds so nearby Juxta devices can detect us.
 * It is intentionally shorter than the scan interval so we advertise
 * multiple times between each passive scan burst. */
#define ADV_INTERVAL_S 10U

static uint32_t get_adv_interval_s(void)
{
	return ADV_INTERVAL_S;
}

static uint32_t get_scan_interval_s(void)
{
	return juxta_settings_get()->scan_interval_s;
}

static void state_timer_cb(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_work_submit(&state_work);
}

static void state_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!hardware_ready || ble_connected) {
		return;
	}

	uint32_t now = juxta_time_now();

	if (ble_state == BLE_STATE_ADVERTISING) {
		(void)bt_le_adv_stop();
		ble_state = BLE_STATE_IDLE;
	}

	if (ble_state == BLE_STATE_CONNECTABLE_ADV) {
		(void)bt_le_adv_stop();
		ble_state = BLE_STATE_IDLE;
	}

	if (ble_state == BLE_STATE_SCANNING) {
		(void)bt_le_scan_stop();
		process_scan_events();
		flush_scan_table();
		last_scan_ts = now;
		ble_state = BLE_STATE_IDLE;
	}

	if (ble_state != BLE_STATE_IDLE) {
		return;
	}

	if (do_gateway_adv) {
		do_gateway_adv = false;
		if (start_connectable_adv() == 0) {
			ble_state = BLE_STATE_CONNECTABLE_ADV;
			k_timer_start(&state_timer, K_MSEC(GATEWAY_ADV_MS), K_NO_WAIT);
			return;
		}
	}

	bool scan_due = (now >= last_scan_ts + get_scan_interval_s());
	bool adv_due  = (now >= last_adv_ts  + get_adv_interval_s());

	if (scan_due) {
		if (start_scanning() == 0) {
			ble_state = BLE_STATE_SCANNING;
			k_timer_start(&state_timer, K_MSEC(SCAN_BURST_MS), K_NO_WAIT);
			return;
		}
	}

	if (adv_due) {
		if (start_nonconn_adv() == 0) {
			ble_state = BLE_STATE_ADVERTISING;
			last_adv_ts = now;
			k_timer_start(&state_timer, K_MSEC(ADV_BURST_MS), K_NO_WAIT);
			return;
		}
	}

	uint32_t wait_adv = (now < last_adv_ts + get_adv_interval_s())
				    ? (last_adv_ts + get_adv_interval_s() - now) : 0U;
	uint32_t wait_scan = (now < last_scan_ts + get_scan_interval_s())
				     ? (last_scan_ts + get_scan_interval_s() - now) : 0U;
	uint32_t next_s = MIN(wait_adv, wait_scan);
	uint32_t jitter_ms = sys_rand32_get() % 1000U;

	k_timer_start(&state_timer, K_MSEC((uint64_t)next_s * 1000U + jitter_ms), K_NO_WAIT);
}

/* ---------------------------------------------------------------------------
 * Callbacks required by ble_service.c
 * -------------------------------------------------------------------------*/
void juxta_ble_timing_update_trigger(void)
{
	/* Called from the BT RX thread when settings change.  state_work and
	 * vitals_timer are not initialized until production init (Step 9), so
	 * submitting them before hardware_ready is set causes a kernel panic. */
	if (!hardware_ready) {
		return;
	}

	const struct juxta_settings *s = juxta_settings_get();

	k_timer_start(&vitals_timer, K_SECONDS(s->vitals_interval_s),
		      K_SECONDS(s->vitals_interval_s));
	k_work_submit(&state_work);
}

void juxta_ble_datetime_synchronized(void)
{
	datetime_synchronized = true;
	LOG_INF("Datetime synchronized");
}

void juxta_ble_reset_requested(void)
{
	/* Called from the BT RX thread via the gateway "reset" command.
	 * Stop radio, give BLE stack a moment to send the disconnect, then
	 * enter shelf mode.  enter_shelf_mode() does not return. */
	LOG_INF("Gateway reset: entering shelf mode");
	k_sleep(K_MSEC(200)); /* allow BLE disconnect PDU to be sent */
	enter_shelf_mode();
}

/* ---------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------*/
int main(void)
{
	int rc;

	/* ----------------------------------------------------------------
	 * Step 1: GPIO
	 * -------------------------------------------------------------- */
	if (!gpio_is_ready_dt(&led) || !gpio_is_ready_dt(&magnet) ||
	    !gpio_is_ready_dt(&accel_int)) {
		return -ENODEV;
	}

	(void)gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	(void)gpio_pin_configure_dt(&magnet, GPIO_INPUT);
	(void)gpio_pin_configure_dt(&accel_int, GPIO_INPUT);

	/* ----------------------------------------------------------------
	 * Step 2: LED timer/work — initialized early so set_led_mode()
	 * works throughout the entire boot sequence.
	 * -------------------------------------------------------------- */
	k_work_init(&led_work, led_work_handler);
	k_timer_init(&led_timer, led_timer_cb, NULL);

	/* ----------------------------------------------------------------
	 * Step 3: Check reset reason to determine boot path.
	 * -------------------------------------------------------------- */
	uint32_t resetreas = NRF_POWER->RESETREAS;
	NRF_POWER->RESETREAS = resetreas; /* write-1-to-clear */

	/* CoreDebug DHCSR bit 0 (C_DEBUGEN) is set whenever a debugger has
	 * enabled the debug interface (J-Link, SWD, etc.).  When active we skip
	 * shelf mode and magnet-hold gating so the full boot path can be exercised
	 * without a true power-cycle. */
	bool debugger_active = (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) != 0U;
	bool from_system_off = (resetreas & POWER_RESETREAS_OFF_Msk) != 0U;
	bool fresh_boot      = (resetreas == 0U);
	bool need_datetime_sync = false;

	LOG_INF("Boot: RESETREAS=0x%08x system_off=%d fresh=%d debugger=%d",
		resetreas, (int)from_system_off, (int)fresh_boot, (int)debugger_active);

	if (debugger_active) {
		/* --------------------------------------------------------
		 * Debug simulation: emulate the full shelf → wake cycle.
		 *
		 * LED goes off (shelf mode), then the loop waits for the
		 * magnet to be applied.  Magnet hold is measured exactly as
		 * in production so all LED patterns and mode branches work
		 * identically.  DFU → shelf calls enter_shelf_mode() which
		 * does sys_reboot(), restarting and re-entering this block.
		 * ------------------------------------------------------ */
		LOG_INF("[DBG] Simulated shelf mode — apply magnet to wake");
		set_led_mode(LED_MODE_OFF);

		while (gpio_pin_get_dt(&magnet) == 0) {
			k_sleep(K_MSEC(50));
		}

		LOG_INF("[DBG] Magnet applied — measuring hold");
		gpio_pin_set_dt(&led, 1);

		int64_t hold_ms = measure_magnet_hold();

		LOG_INF("[DBG] Magnet hold: %lld ms (DFU threshold: %u ms)",
			(long long)hold_ms, DFU_HOLD_THRESHOLD_MS);

		if (hold_ms >= (int64_t)DFU_HOLD_THRESHOLD_MS) {
			enter_dfu_mode(); /* does not return */
		} else {
			gpio_pin_set_dt(&led, 0);
			need_datetime_sync = true;
		}
	} else {
		/* --------------------------------------------------------
		 * Production path (no debugger)
		 * ------------------------------------------------------ */
		if (fresh_boot) {
			LOG_INF("Fresh power-on → shelf mode");
			enter_shelf_mode(); /* does not return */
		}

		if (from_system_off) {
			gpio_pin_set_dt(&led, 1);

			int64_t hold_ms = measure_magnet_hold();

			LOG_INF("Magnet hold: %lld ms", (long long)hold_ms);

			if (hold_ms >= (int64_t)DFU_HOLD_THRESHOLD_MS) {
				enter_dfu_mode(); /* does not return */
			} else {
				gpio_pin_set_dt(&led, 0);
				need_datetime_sync = true;
			}
		}
	}

	/* ----------------------------------------------------------------
	 * Step 4: Watchdog
	 * -------------------------------------------------------------- */
	(void)init_watchdog();

	/* ----------------------------------------------------------------
	 * Step 5: Battery ADC
	 * -------------------------------------------------------------- */
	if (adc_is_ready_dt(&fuel)) {
		(void)adc_channel_setup_dt(&fuel);
		(void)sample_battery_mv();
	} else {
		LOG_WRN("FUEL ADC not ready");
	}

	/* ----------------------------------------------------------------
	 * Step 6: Bluetooth (required before derive_device_id)
	 * -------------------------------------------------------------- */
	rc = bt_enable(NULL);
	if (rc != 0) {
		LOG_ERR("BT init failed: %d", rc);
		return rc;
	}

	juxta_time_init();
	derive_device_id();
	(void)bt_set_name(adv_name);

	/* ----------------------------------------------------------------
	 * Step 7: Internal flash settings + NOR log + BLE service
	 * -------------------------------------------------------------- */
	rc = juxta_settings_init(device_id);
	if (rc != 0) {
		LOG_ERR("Settings init failed: %d", rc);
		return rc;
	}

	rc = juxta_log_init(&log_ctx, juxta_settings_get(), device_id);
	if (rc != 0) {
		LOG_ERR("NOR log init failed: %d", rc);
		return rc;
	}

	rc = juxta_ble_service_init(&log_ctx);
	if (rc != 0) {
		LOG_ERR("BLE service init failed: %d", rc);
		return rc;
	}

	juxta_ble_set_battery_mv_source(batt_mv_get);

	/* ----------------------------------------------------------------
	 * Step 8: Datetime sync gate (shelf wake only).
	 * Slow-blink while waiting, solid ON when connected, 5 blinks
	 * on success. Never proceeds without a valid timestamp.
	 * -------------------------------------------------------------- */
	if (need_datetime_sync) {
		wait_for_datetime_sync();
		/* Clock is now valid. File creation and event logging are deferred
		 * to production init so no files exist until data is actually logged. */
	}

	/* ----------------------------------------------------------------
	 * Step 9: Full production init
	 * -------------------------------------------------------------- */
	rc = init_accel();
	if (rc != 0) {
		LOG_WRN("LIS2DH12 init failed: %d — continuing without accel", rc);
	}

	hardware_ready = true;
	juxta_ble_set_production_ready();

	/* "boot" is the first NOR write — creates JXS for today's date.
	 * Skipped silently if clock is still unset (ensure_file returns 0 for time == 0). */
	(void)juxta_log_append_event(&log_ctx, juxta_settings_get(), device_id,
				     "boot", juxta_time_now());

	k_work_init(&state_work, state_work_handler);
	k_work_init(&vitals_work, vitals_work_handler);
	k_timer_init(&state_timer, state_timer_cb, NULL);
	k_timer_init(&vitals_timer, vitals_timer_cb, NULL);

	const struct juxta_settings *settings = juxta_settings_get();

	k_timer_start(&vitals_timer, K_SECONDS(settings->vitals_interval_s),
		      K_SECONDS(settings->vitals_interval_s));

	k_work_submit(&state_work);

	LOG_INF("Production init complete: %s fw=%s", device_id, JUXTA_FIRMWARE_VERSION);

	/* Production magnet monitor: check every 500 ms when not connected.
	 * Holding the magnet triggers 5× blink → 5 s debounce → shelf mode.
	 * The 5 s window lets the user remove the magnet before poweroff.
	 * Ignored while iOS is connected so an accidental touch doesn't abort a session. */
	for (;;) {
		k_sleep(K_MSEC(500));

		if (!ble_connected && gpio_pin_get_dt(&magnet) != 0) {
			LOG_INF("Magnet detected — 5 blinks, then shelf mode in 5 s");

			/* Stop radio before visual sequence so state machine
			 * doesn't restart advertising during the countdown. */
			(void)bt_le_adv_stop();
			(void)bt_le_scan_stop();
			ble_state = BLE_STATE_IDLE;

			led_blink(5U, 50U, 100U);

			LOG_INF("Remove magnet — entering shelf mode");
			(void)juxta_log_append_event(&log_ctx, juxta_settings_get(),
						     device_id, "user_disconnected",
						     juxta_time_now());
			k_sleep(K_SECONDS(5));
			enter_shelf_mode(); /* does not return */
		}
	}

	return 0;
}
