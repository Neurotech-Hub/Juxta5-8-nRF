/*
 * JUXTA BLE Application
 *
 * Copyright (c) 2025 NeurotechHub
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <zephyr/random/random.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/watchdog.h>
#include "juxta_vitals_nrf52/vitals.h"
#include "juxta_framfs/framfs.h"
#include "juxta_fram/fram.h"
#include "ble_service.h"
#include "adc.h"
#include <stdio.h>
#include <time.h>
#include "lis2dh12.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

typedef enum
{
    BLE_STATE_IDLE = 0,
    BLE_STATE_ADVERTISING,
    BLE_STATE_SCANNING,
    BLE_STATE_WAITING,
    BLE_STATE_GATEWAY_ADVERTISING
} ble_state_t;

static ble_state_t ble_state = BLE_STATE_IDLE;

// Add gateway advertising flag and timer
static bool doGatewayAdvertise = false;
static struct k_timer ten_minute_timer;
static bool ble_connected = false; // Track connection state

// Production flow tracking
static bool magnet_activated = false;
static bool datetime_synchronized = false;
static uint8_t datetime_sync_retry_count = 0;

// Work queue for async connectable advertising restart
static struct k_work datetime_sync_restart_work;

// Track whether connectable advertising is currently active
static bool connectable_adv_active = false;

// Hardware state
static struct juxta_fram_device fram_dev; /* Global FRAM device for framfs */
static bool hardware_verified = false;
static bool watchdog_reset_detected = false; // Set true after LIS2DH + FRAM verification

// Watchdog timer
static const struct device *wdt = DEVICE_DT_GET(DT_NODELABEL(wdt0));
static int wdt_channel_id;
static struct k_timer wdt_feed_timer;

// Watchdog feed timer callback - ensures watchdog is always fed
static void wdt_feed_timer_callback(struct k_timer *timer)
{
    if (wdt && wdt_channel_id >= 0)
    {
        int err = wdt_feed(wdt, wdt_channel_id);
        if (err < 0)
        {
            LOG_ERR("Failed to feed watchdog: %d", err);
        }
    }
}

/**
 * @brief Consolidated FRAM and framfs initialization function
 * @param fram_device Pointer to FRAM device structure to initialize
 * @param framfs_context Pointer to framfs context to initialize (if init_framfs is true)
 * @param init_framfs Whether to initialize framfs context
 * @return 0 on success, negative error code on failure
 */
static int init_fram_and_framfs(struct juxta_fram_device *fram_device, struct juxta_framfs_context *framfs_context, bool init_framfs)
{
    const struct device *spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi0));
    if (!spi_dev || !device_is_ready(spi_dev))
    {
        LOG_ERR("❌ SPI0 device not ready");
        return -ENODEV;
    }

    static const struct gpio_dt_spec fram_cs = GPIO_DT_SPEC_GET_BY_IDX(DT_NODELABEL(spi0), cs_gpios, 0);
    if (!device_is_ready(fram_cs.port))
    {
        LOG_ERR("❌ FRAM CS not ready");
        return -ENODEV;
    }

    int ret = juxta_fram_init(fram_device, spi_dev, 8000000, &fram_cs);
    if (ret < 0)
    {
        LOG_ERR("❌ FRAM init failed: %d", ret);
        if (ret == JUXTA_FRAM_ERROR_ID)
        {
            LOG_ERR("❌ FRAM chip not detected - check hardware connections");
        }
        return ret;
    }

    if (init_framfs)
    {
        if (!framfs_context)
        {
            LOG_ERR("❌ Framfs context pointer is NULL");
            return -EINVAL;
        }
        ret = juxta_framfs_init(framfs_context, fram_device);
        if (ret < 0)
        {
            LOG_ERR("❌ Framfs init failed: %d", ret);
            return ret;
        }
        LOG_INF("✅ Framfs initialized");
    }

    return 0;
}

/**
 * @brief Quick FRAM test to verify basic functionality
 */
static void test_fram_functionality(void)
{
    struct juxta_fram_device fram_test_dev;
    int ret = init_fram_and_framfs(&fram_test_dev, NULL, false);
    if (ret < 0)
    {
        LOG_ERR("❌ FRAM functionality test failed: %d", ret);
    }
}

#define BLE_MIN_INTER_BURST_DELAY_MS 100

static struct juxta_vitals_ctx vitals_ctx;
static struct juxta_framfs_context framfs_ctx;
static struct juxta_framfs_ctx time_ctx; /* Time-aware file system context */

// Unused burst tracking variables removed - state machine handles this
static uint32_t last_adv_timestamp = 0;
static uint32_t last_scan_timestamp = 0;

/* Simple JUXTA device tracking for single scan burst */
#define MAX_JUXTA_DEVICES 64
static uint16_t last_logged_minute = 0xFFFF;
typedef struct
{
    uint32_t mac_id;
    int8_t rssi;
} juxta_scan_entry_t;
static juxta_scan_entry_t juxta_scan_table[MAX_JUXTA_DEVICES];
static uint8_t juxta_scan_count = 0;

static void juxta_scan_table_reset(void)
{
    juxta_scan_count = 0;
    memset(juxta_scan_table, 0, sizeof(juxta_scan_table));
}

static void juxta_scan_table_print_and_clear(void)
{
    if (juxta_scan_count > 0)
    {
        LOG_INF("=== JUXTA SCAN TABLE ===");
        for (uint8_t i = 0; i < juxta_scan_count && i < MAX_JUXTA_DEVICES; i++)
        {
            LOG_INF("MAC: %06X, RSSI: %d", juxta_scan_table[i].mac_id, juxta_scan_table[i].rssi);
        }
        LOG_INF("=== END SCAN TABLE ===");
    }
    juxta_scan_count = 0;
    memset(juxta_scan_table, 0, sizeof(juxta_scan_table));
}

static struct k_work state_work;
static struct k_timer state_timer;
static bool state_system_ready = false;

// ADC timer for mode 1 (ADC_ONLY mode)
static struct k_timer adc_timer;
static struct k_work adc_work;

/* Static buffers for ADC sampling to avoid sysworkq stack overflow */
#define ADC_MAX_SAMPLES 1000
static int32_t adc_samples_buffer[ADC_MAX_SAMPLES];
static uint8_t adc_scaled_buffer[ADC_MAX_SAMPLES];

/* Operating mode definitions */
#define OPERATING_MODE_NORMAL 0x00   /* State machine/BLE bursts/motion counting */
#define OPERATING_MODE_ADC_ONLY 0x01 /* Purely ADC recordings */

#define ADV_BURST_DURATION_MS 100
#define SCAN_BURST_DURATION_MS 500
#define ADV_INTERVAL_SECONDS 5
#define SCAN_INTERVAL_SECONDS 20
#define GATEWAY_ADV_TIMEOUT_SECONDS 30
#define WDT_TIMEOUT_MS 30000

/* Dynamic advertising name based on MAC address */
static char adv_name[12] = "JX_000000"; /* Initialized placeholder */

/* Forward declarations */
static int juxta_start_advertising(void);
static int juxta_stop_advertising(void);
static int juxta_start_scanning(void);
static int juxta_stop_scanning(void);
static uint32_t get_rtc_timestamp(void);
static int juxta_start_connectable_advertising(void);
static void juxta_log_simple(uint8_t type);
static int init_fram_and_framfs(struct juxta_fram_device *fram_device, struct juxta_framfs_context *framfs_context, bool init_framfs);

/* Dynamic advertising name setup */
static void setup_dynamic_adv_name(void);

#define SCAN_EVENT_QUEUE_SIZE 16

typedef struct
{
    uint32_t mac_id;
    int8_t rssi;
} scan_event_t;

K_MSGQ_DEFINE(scan_event_q, sizeof(scan_event_t), SCAN_EVENT_QUEUE_SIZE, 4);

/* Scan callback for BLE scanning - runs in ISR context */
__no_optimization static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type, struct net_buf_simple *ad)
{
    ARG_UNUSED(adv_type);
    if (!addr || !ad || ad->len == 0)
    {
        return;
    }

    const char *name = NULL;
    char dev_name[32] = {0};
    struct net_buf_simple_state state;
    net_buf_simple_save(ad, &state);

    while (ad->len > 1)
    {
        uint8_t len = net_buf_simple_pull_u8(ad);
        if (len == 0 || len > ad->len)
            break;
        uint8_t type = net_buf_simple_pull_u8(ad);
        len--;
        if (len > ad->len)
            break;
        if ((type == BT_DATA_NAME_COMPLETE || type == BT_DATA_NAME_SHORTENED) && len < sizeof(dev_name))
        {
            memset(dev_name, 0, sizeof(dev_name));
            memcpy(dev_name, ad->data, len);
            dev_name[len] = '\0';
            name = dev_name;
        }
        net_buf_simple_pull(ad, len);
    }
    net_buf_simple_restore(ad, &state);

    // New logic: recognize JXGA_XXXX (gateway) and JX_XXXXXX (peripheral)
    char mac_str[7] = {0}; // Always 6 chars for logging
    if (name)
    {
        if (strncmp(name, "JXGA_", 5) == 0 && strlen(name) == 9) // JXGA_XXXX (gateway)
        {
            snprintf(mac_str, sizeof(mac_str), "FF%.4s", name + 5); // Prepend FF
            if (!doGatewayAdvertise)                                // Only set if not already set
            {
                doGatewayAdvertise = true;
                LOG_INF("🔔 Gateway detected: %s - will trigger connectable advertising", mac_str);
            }
        }
        else if (strncmp(name, "JX_", 3) == 0 && strlen(name) == 9) // JX_XXXXXX (peripheral)
        {
            snprintf(mac_str, sizeof(mac_str), "%.6s", name + 3);
        }
        else
        {
            // Not a recognized pattern, ignore
            return;
        }

        // Convert to uint32_t for storage (first 6 hex digits)
        uint32_t mac_id = 0;
        if (sscanf(mac_str, "%6x", &mac_id) == 1 && mac_id != 0)
        {
            scan_event_t evt = {.mac_id = mac_id, .rssi = rssi};
            (void)k_msgq_put(&scan_event_q, &evt, K_NO_WAIT);

            // Use printk for ISR context logging
            char addr_str[BT_ADDR_LE_STR_LEN];
            bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
            printk("Found JUXTA device: %s (%s), RSSI: %d\n", mac_str, addr_str, rssi);
        }
    }
}

static uint32_t get_adv_interval(void)
{
    uint8_t adv_interval = ADV_INTERVAL_SECONDS; /* Default fallback */

    /* Get interval from framfs user settings */
    if (framfs_ctx.initialized)
    {
        if (juxta_framfs_get_adv_interval(&framfs_ctx, &adv_interval) == 0)
        {
            LOG_DBG("📡 Using adv_interval from settings: %d", adv_interval);
        }
        else
        {
            LOG_WRN("📡 Failed to get adv_interval from settings, using default: %d", ADV_INTERVAL_SECONDS);
            adv_interval = ADV_INTERVAL_SECONDS;
        }
    }
    else
    {
        LOG_WRN("📡 Framfs not initialized, using default adv_interval: %d", ADV_INTERVAL_SECONDS);
        adv_interval = ADV_INTERVAL_SECONDS;
    }

    /* Apply motion-based interval adjustment */
    if (lis2dh12_should_use_extended_intervals())
    {
        adv_interval *= 2; /* Double the interval when no motion detected */
        LOG_DBG("📡 No motion detected, using extended adv_interval: %d", adv_interval);
    }

    return adv_interval;
}

static uint32_t get_scan_interval(void)
{
    uint8_t scan_interval = SCAN_INTERVAL_SECONDS; /* Default fallback */

    /* Get interval from framfs user settings */
    if (framfs_ctx.initialized)
    {
        if (juxta_framfs_get_scan_interval(&framfs_ctx, &scan_interval) == 0)
        {
            LOG_DBG("🔍 Using scan_interval from settings: %d", scan_interval);
        }
        else
        {
            LOG_WRN("🔍 Failed to get scan_interval from settings, using default: %d", SCAN_INTERVAL_SECONDS);
            scan_interval = SCAN_INTERVAL_SECONDS;
        }
    }
    else
    {
        LOG_WRN("🔍 Framfs not initialized, using default scan_interval: %d", SCAN_INTERVAL_SECONDS);
        scan_interval = SCAN_INTERVAL_SECONDS;
    }

    /* Apply motion-based interval adjustment */
    if (lis2dh12_should_use_extended_intervals())
    {
        scan_interval *= 2; /* Double the interval when no motion detected */
        LOG_DBG("🔍 No motion detected, using extended scan_interval: %d", scan_interval);
    }

    return scan_interval;
}

/**
 * @brief Trigger timing update when settings change
 * Called from BLE service when user settings are updated
 */
void juxta_ble_timing_update_trigger(void)
{
    LOG_INF("⏰ Timing update triggered - recalculating intervals");

    /* Force recalculation of next action times */
    uint32_t current_time = get_rtc_timestamp();
    if (current_time > 0)
    {
        last_adv_timestamp = current_time - get_adv_interval();
        last_scan_timestamp = current_time - get_scan_interval();
        LOG_INF("⏰ Updated timing: adv_interval=%d, scan_interval=%d",
                get_adv_interval(), get_scan_interval());
    }
}

static void init_randomization(void)
{
    LOG_INF("🎲 Randomization enabled for state machine timing");
}

static uint32_t get_rtc_timestamp(void)
{
    uint32_t timestamp = juxta_vitals_get_timestamp(&vitals_ctx);
    LOG_DBG("Timestamp: %u", timestamp);
    return timestamp;
}

/* Battery check helper for FRAM operations */
static bool should_allow_fram_write(void)
{
    if (juxta_vitals_is_low_battery(&vitals_ctx))
    {
        LOG_WRN("⚠️ Battery critically low (%d mV) - preventing FRAM write",
                juxta_vitals_get_battery_mv(&vitals_ctx));
        return false;
    }
    return true;
}

// ADC work handler - runs in thread context to avoid ISR issues
static void adc_work_handler(struct k_work *work)
{
    if (!hardware_verified || !framfs_ctx.initialized || ble_connected || !juxta_adc_is_ready())
    {
        return;
    }

    /* Check battery before FRAM operations */
    if (!should_allow_fram_write())
    {
        LOG_INF("📊 Skipping ADC burst due to low battery");
        return;
    }

    // Trigger ADC burst sampling into static buffer
    int32_t *samples = adc_samples_buffer; // Use static buffer to avoid stack usage
    uint32_t actual_samples;
    uint32_t duration_us;

    int ret = juxta_adc_burst_sample(samples, ADC_MAX_SAMPLES, &actual_samples, &duration_us);
    if (ret == 0 && actual_samples > 0)
    {
        uint32_t start_time = juxta_vitals_get_timestamp(&vitals_ctx) * 1000000; // Convert to microseconds

        // Convert int32_t samples to uint8_t for storage (scale appropriately) using static buffer
        uint8_t *scaled_samples = adc_scaled_buffer;
        uint32_t count = (actual_samples > ADC_MAX_SAMPLES) ? ADC_MAX_SAMPLES : actual_samples;
        for (uint32_t i = 0; i < count; i++)
        {
            // Scale int32_t voltage (mV) to uint8_t (0-255 range)
            // Assuming voltage range is roughly -2000 to +2000 mV
            int32_t scaled = (samples[i] + 2000) * 255 / 4000;
            if (scaled < 0)
                scaled = 0;
            if (scaled > 255)
                scaled = 255;
            scaled_samples[i] = (uint8_t)scaled;
        }

        ret = juxta_framfs_append_adc_burst_data(&time_ctx, start_time, scaled_samples, (uint16_t)count, duration_us);
        if (ret == 0)
        {
            LOG_INF("📊 ADC burst saved: %u samples, %u us", (unsigned)count, (unsigned)duration_us);
        }
        else
        {
            LOG_ERR("📊 Failed to record ADC burst: %d", ret);
        }
    }
    else
    {
        LOG_WRN("📊 ADC burst sampling failed or no samples: %d", ret);
    }
}

// ADC timer callback - triggers ADC work in thread context
static void adc_timer_callback(struct k_timer *timer)
{
    // Only submit work, do not call FRAMFS functions in ISR context
    k_work_submit(&adc_work);
}

/* Definition: simple record logger (BOOT/CONNECTED/NO_ACTIVITY/ERROR) */
static void juxta_log_simple(uint8_t type)
{
    if (!hardware_verified || !framfs_ctx.initialized || ble_connected)
    {
        return;
    }

    if (!should_allow_fram_write())
    {
        return;
    }

    uint16_t minute = juxta_vitals_get_minute_of_day(&vitals_ctx);
    (void)juxta_framfs_append_simple_record_data(&time_ctx, minute, type);
}

/* Wrapper to provide YYMMDD date for framfs time API using vitals */
static uint32_t juxta_vitals_get_file_date_wrapper(void)
{
    return juxta_vitals_get_file_date(&vitals_ctx);
}

static void setup_dynamic_adv_name(void)
{
    bt_addr_le_t addr;
    size_t count = 1;

    bt_id_get(&addr, &count); // NCS v3.0.2: returns void
    if (count > 0)
    {
        snprintf(adv_name, sizeof(adv_name), "JX_%02X%02X%02X",
                 addr.a.val[3], addr.a.val[2], addr.a.val[1]);
        LOG_INF("📛 Set advertising name: %s", adv_name);
    }
    else
    {
        LOG_ERR("Failed to get BLE MAC address");
        strcpy(adv_name, "JX_ERROR");
    }
}

static bool is_time_to_advertise(void)
{
    uint32_t current_time = get_rtc_timestamp();
    if (current_time == 0)
        return false;
    return (current_time - last_adv_timestamp) >= get_adv_interval();
}

static bool is_time_to_scan(void)
{
    uint32_t current_time = get_rtc_timestamp();
    if (current_time == 0)
        return false;
    return (current_time - last_scan_timestamp) >= get_scan_interval();
}

// Harden timer/event scheduling
// Only post events to the workqueue from timer callbacks
static enum {
    EVENT_NONE = 0,
    EVENT_TIMER_EXPIRED,
} state_event;

static void state_timer_callback(struct k_timer *timer)
{
    // Only post an event, do not call BLE APIs or change state here
    state_event = EVENT_TIMER_EXPIRED;
    k_work_submit(&state_work);
}

static void process_scan_events(void)
{
    scan_event_t evt;
    while (k_msgq_get(&scan_event_q, &evt, K_NO_WAIT) == 0)
    {
        if (evt.mac_id == 0)
        {
            LOG_WRN("⚠️ Ignoring scan event with MAC ID 0");
            continue;
        }
        if (juxta_scan_count >= MAX_JUXTA_DEVICES)
        {
            LOG_ERR("⚠️ Scan table full (%u/%u), cannot add MAC %06X", juxta_scan_count, MAX_JUXTA_DEVICES, evt.mac_id);
            continue;
        }
        bool found = false;
        uint8_t found_index = 0;
        for (uint8_t i = 0; i < juxta_scan_count; i++)
        {
            if (juxta_scan_table[i].mac_id == evt.mac_id)
            {
                found = true;
                found_index = i;
                break;
            }
        }
        if (!found)
        {
            juxta_scan_table[juxta_scan_count].mac_id = evt.mac_id;
            juxta_scan_table[juxta_scan_count].rssi = evt.rssi;
            LOG_INF("🔍 Added to scan table: MAC: %06X, RSSI: %d, count: %u", evt.mac_id, evt.rssi, juxta_scan_count + 1);
            juxta_scan_count++;
        }
        else
        {
            // Update RSSI if this one is stronger (higher value)
            if (evt.rssi > juxta_scan_table[found_index].rssi)
            {
                LOG_DBG("🔍 Updated RSSI for MAC %06X: %d -> %d (stronger signal)",
                        evt.mac_id, juxta_scan_table[found_index].rssi, evt.rssi);
                juxta_scan_table[found_index].rssi = evt.rssi;
            }
        }
    }
}

static void state_work_handler(struct k_work *work)
{
    uint32_t current_time = get_rtc_timestamp();

    // Process all scan events from the queue
    process_scan_events();

    // Minute-of-day logging and scan table clearing
    uint16_t current_minute = juxta_vitals_get_minute_of_day(&vitals_ctx);
    if (current_minute != last_logged_minute)
    {
        /* Consolidated minute logging to FRAMFS (devices + motion + battery + temperature) */
        if (hardware_verified && framfs_ctx.initialized && !ble_connected)
        {
            /* Check battery before FRAM operations */
            if (!should_allow_fram_write())
            {
                LOG_INF("📊 Skipping FRAMFS minute logging due to low battery");
                return;
            }

            /* Get battery level */
            uint8_t battery_level = 0;
            (void)juxta_vitals_update(&vitals_ctx);
            if (juxta_vitals_get_validated_battery_level(&vitals_ctx, &battery_level) != 0)
            {
                battery_level = 0; // Default if read fails
            }

            /* Get temperature from LIS2DH */
            int8_t temperature = 0;
            // TODO: Add temperature reading through motion system interface

            if (juxta_scan_count > 0)
            {
                /* Convert scan table to FRAMFS packed format */
                uint8_t mac_ids[MAX_JUXTA_DEVICES][3];
                int8_t rssi_values[MAX_JUXTA_DEVICES];
                uint8_t device_count = MIN(juxta_scan_count, (uint8_t)MAX_JUXTA_DEVICES);
                for (uint8_t i = 0; i < device_count; i++)
                {
                    mac_ids[i][0] = (juxta_scan_table[i].mac_id >> 16) & 0xFF;
                    mac_ids[i][1] = (juxta_scan_table[i].mac_id >> 8) & 0xFF;
                    mac_ids[i][2] = juxta_scan_table[i].mac_id & 0xFF;
                    rssi_values[i] = juxta_scan_table[i].rssi;
                }
                int ret = juxta_framfs_append_device_scan_data(&time_ctx, current_minute, lis2dh12_get_motion_count(),
                                                               battery_level, temperature,
                                                               mac_ids, rssi_values, device_count);
                if (ret == 0)
                {
                    LOG_INF("📊 FRAMFS minute record: devices=%d, motion=%d, battery=%d%%, temp=%d°C",
                            device_count, lis2dh12_get_motion_count(), battery_level, temperature);
                }
            }
            else
            {
                /* No devices found - use NO_ACTIVITY type but still include battery/temperature */
                int ret = juxta_framfs_append_device_scan_data(&time_ctx, current_minute, lis2dh12_get_motion_count(),
                                                               battery_level, temperature,
                                                               NULL, NULL, 0);
                if (ret == 0)
                {
                    LOG_INF("📊 FRAMFS minute record: no activity, battery=%d%%, temp=%d°C",
                            battery_level, temperature);
                }
            }
        }
        else if (ble_connected)
        {
            LOG_DBG("⏸️ FRAMFS minute logging paused during BLE connection");
        }

        /* Print and clear after logging to preserve contents */
        juxta_scan_table_print_and_clear();

        // Process motion events and adjust intervals based on activity
        lis2dh12_process_motion_events();

        last_logged_minute = current_minute;
        LOG_INF("Minute of day: %u", current_minute);
    }

    // Pause state machine if connected
    if (ble_connected)
    {
        LOG_DBG("⏸️ State machine paused - BLE connection active");
        return;
    }

    // Only handle BLE state transitions if triggered by timer event
    if (state_event == EVENT_TIMER_EXPIRED)
    {
        state_event = EVENT_NONE;

        LOG_DBG("State work handler: current_time=%u, ble_state=%d, doGatewayAdvertise=%s",
                current_time, ble_state, doGatewayAdvertise ? "true" : "false");

        // Handle gateway advertising state
        if (ble_state == BLE_STATE_GATEWAY_ADVERTISING)
        {
            int err = juxta_stop_advertising();
            if (err == 0)
            {
                ble_state = BLE_STATE_WAITING;
                last_adv_timestamp = current_time;
                // LOG_INF("Gateway advertising burst completed at timestamp %u", last_adv_timestamp);
                k_timer_start(&state_timer, K_MSEC(BLE_MIN_INTER_BURST_DELAY_MS), K_NO_WAIT);
            }
            else
            {
                LOG_ERR("Failed to stop gateway advertising burst, skipping transition");
            }
            return;
        }

        if (ble_state == BLE_STATE_SCANNING)
        {
            int err = juxta_stop_scanning();
            if (err == 0)
            {
                ble_state = BLE_STATE_WAITING;
                last_scan_timestamp = current_time;
                // LOG_INF("Scan burst completed at timestamp %u", last_scan_timestamp);
                k_timer_start(&state_timer, K_MSEC(BLE_MIN_INTER_BURST_DELAY_MS), K_NO_WAIT);
            }
            else
            {
                LOG_ERR("Failed to stop scan burst, skipping transition");
            }
            return;
        }
        if (ble_state == BLE_STATE_ADVERTISING)
        {
            int err = juxta_stop_advertising();
            if (err == 0)
            {
                ble_state = BLE_STATE_WAITING;
                last_adv_timestamp = current_time;
                // LOG_INF("Advertising burst completed at timestamp %u", last_adv_timestamp);
                k_timer_start(&state_timer, K_MSEC(BLE_MIN_INTER_BURST_DELAY_MS), K_NO_WAIT);
            }
            else
            {
                LOG_ERR("Failed to stop advertising burst, skipping transition");
            }
            return;
        }

        bool scan_due = is_time_to_scan();
        bool adv_due = is_time_to_advertise();

        LOG_DBG("Checking for new bursts: scan_due=%d, adv_due=%d, doGatewayAdvertise=%s",
                scan_due, adv_due, doGatewayAdvertise ? "true" : "false");

        if (scan_due && ble_state == BLE_STATE_IDLE)
        {
            juxta_scan_table_reset();
            ble_state = BLE_STATE_SCANNING;
            int err = juxta_start_scanning();
            if (err == 0)
            {
                LOG_INF("Starting scan burst (%d ms)", SCAN_BURST_DURATION_MS);
                k_timer_start(&state_timer, K_MSEC(SCAN_BURST_DURATION_MS), K_NO_WAIT);
            }
            else
            {
                ble_state = BLE_STATE_IDLE;
                LOG_ERR("Scan failed, retrying in 1 second");
                k_timer_start(&state_timer, K_SECONDS(1), K_NO_WAIT);
            }
            return;
        }

        // Check for gateway advertising first (higher priority)
        if (adv_due && ble_state == BLE_STATE_IDLE && doGatewayAdvertise)
        {
            ble_state = BLE_STATE_GATEWAY_ADVERTISING;
            // Clear the gateway advertise flag so we don't advertise again
            doGatewayAdvertise = false;
            int err = juxta_start_connectable_advertising();
            if (err == 0)
            {
                LOG_INF("Starting gateway advertising burst (%ds connectable)", GATEWAY_ADV_TIMEOUT_SECONDS);
                k_timer_start(&state_timer, K_SECONDS(GATEWAY_ADV_TIMEOUT_SECONDS), K_NO_WAIT);
            }
            else
            {
                ble_state = BLE_STATE_IDLE;
                LOG_ERR("Gateway advertising failed, continuing with normal operation");
                // Don't retry - move on to normal state machine operation
                k_work_submit(&state_work);
            }
            return;
        }

        if (adv_due && ble_state == BLE_STATE_IDLE)
        {
            ble_state = BLE_STATE_ADVERTISING;
            int err = juxta_start_advertising();
            if (err == 0)
            {
                LOG_INF("Starting advertising burst (%d ms)", ADV_BURST_DURATION_MS);
                k_timer_start(&state_timer, K_MSEC(ADV_BURST_DURATION_MS), K_NO_WAIT);
            }
            else
            {
                ble_state = BLE_STATE_IDLE;
                LOG_ERR("Advertising failed, retrying in 1 second");
                k_timer_start(&state_timer, K_SECONDS(1), K_NO_WAIT);
            }
            return;
        }
        if (ble_state == BLE_STATE_WAITING)
        {
            LOG_DBG("Transitioning from WAITING to IDLE");
            ble_state = BLE_STATE_IDLE;
        }

        uint32_t time_until_adv = 0;
        uint32_t time_until_scan = 0;

        if (ble_state == BLE_STATE_IDLE)
        {
            uint32_t time_since_adv = current_time - last_adv_timestamp;
            uint32_t time_since_scan = current_time - last_scan_timestamp;

            time_until_adv = (time_since_adv >= get_adv_interval()) ? 0 : (get_adv_interval() - time_since_adv);
            time_until_scan = (time_since_scan >= get_scan_interval()) ? 0 : (get_scan_interval() - time_since_scan);
        }

        uint32_t next_delay_ms = MIN(time_until_adv, time_until_scan) * 1000;
        /* Add minimum delay to prevent rapid start/stop cycles */
        next_delay_ms = MAX(next_delay_ms, 100);

        /* Add small random offset (0-1000ms) to prevent device synchronization */
        uint32_t random_offset = sys_rand32_get() % 1000;
        next_delay_ms += random_offset;

        LOG_DBG("Sleeping for %u ms until next action (including %u ms random offset)",
                next_delay_ms, random_offset);
        k_timer_start(&state_timer, K_MSEC(next_delay_ms), K_NO_WAIT);

        uint32_t ts = juxta_vitals_get_timestamp(&vitals_ctx);
        uint32_t uptime = k_uptime_get_32();
        LOG_DBG("Timestamp: %u, Uptime(ms): %u", ts, uptime);
    }
}

static int juxta_start_advertising(void)
{
    LOG_INF("📢 Starting advertising burst (%d ms)", ADV_BURST_DURATION_MS);

    // Use non-connectable advertising for energy efficiency
    struct bt_le_adv_param adv_param = {
        .id = BT_ID_DEFAULT,
        .sid = 0,
        .secondary_max_skip = 0,
        .options = 0, // Non-connectable for energy efficiency (0 = non-connectable by default)
        .interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
        .interval_max = BT_GAP_ADV_FAST_INT_MAX_2,
        .peer = NULL,
    };

    /* Set advertising data with dynamic name */
    struct bt_data adv_data[] = {
        BT_DATA(BT_DATA_NAME_COMPLETE, adv_name, strlen(adv_name))};

    int ret = bt_le_adv_start(&adv_param, adv_data, ARRAY_SIZE(adv_data), NULL, 0);
    if (ret < 0)
    {
        LOG_ERR("Advertising failed to start (err %d)", ret);
        return ret;
    }

    LOG_INF("BLE advertising started as '%s' (non-connectable burst)", adv_name);
    return 0;
}

static int juxta_stop_advertising(void)
{
    if (ble_state != BLE_STATE_ADVERTISING && ble_state != BLE_STATE_GATEWAY_ADVERTISING)
    {
        LOG_WRN("❗ Attempted to stop advertising when not in advertising burst");
        return -1;
    }

    int ret = bt_le_adv_stop();
    if (ret < 0)
    {
        LOG_ERR("Advertising failed to stop (err %d)", ret);
        return ret;
    }

    ble_state = BLE_STATE_WAITING;
    return 0;
}

static int juxta_start_scanning(void)
{
    LOG_INF("🔍 Starting scan burst (%d ms)", SCAN_BURST_DURATION_MS);

    struct bt_le_scan_param scan_param = {
        .type = BT_LE_SCAN_TYPE_PASSIVE,
        .options = BT_LE_SCAN_OPT_FILTER_DUPLICATE,
        .interval = BT_GAP_SCAN_FAST_INTERVAL,
        .window = BT_GAP_SCAN_FAST_WINDOW,
        .timeout = 0,
    };

    /* Ensure advertising is fully stopped and add a longer delay before scanning */
    bt_le_adv_stop();
    k_sleep(K_MSEC(200)); // Increased delay for radio stability

    LOG_INF("🔍 About to call bt_le_scan_start with interval=0x%04x, window=0x%04x...",
            scan_param.interval, scan_param.window);

    int ret = bt_le_scan_start(&scan_param, scan_cb);
    LOG_INF("🔍 bt_le_scan_start returned: %d", ret);

    if (ret < 0)
    {
        LOG_ERR("Scanning failed to start (err %d)", ret);
        return ret;
    }

    LOG_INF("🔍 BLE scanning started (passive mode)");
    return 0;
}

static int juxta_stop_scanning(void)
{
    if (ble_state != BLE_STATE_SCANNING)
    {
        LOG_WRN("❗ Attempted to stop scan when not in burst");
        return -1;
    }

    int ret = bt_le_scan_stop();
    if (ret < 0)
    {
        LOG_ERR("Scanning failed to stop (err %d)", ret);
        return ret;
    }

    ble_state = BLE_STATE_WAITING;
    LOG_INF("Scanning stopped successfully");
    return 0;
}

static int test_rtc_functionality(void)
{
    int ret;

    LOG_INF("🧪 Testing RTC functionality...");

    /* Check if vitals are already initialized (from BLE timestamp sync) */
    uint32_t current_timestamp = juxta_vitals_get_timestamp(&vitals_ctx);
    if (current_timestamp > 0)
    {
        LOG_INF("⏰ Vitals already initialized with timestamp: %u", current_timestamp);
        LOG_INF("✅ Skipping vitals reinitialization to preserve BLE timestamp");
    }
    else
    {
        /* Only initialize if not already done */
        ret = juxta_vitals_init(&vitals_ctx, true); // Enable battery monitoring
        if (ret < 0)
        {
            LOG_ERR("Failed to initialize vitals library: %d", ret);
            return ret;
        }

        // Set RTC/Unix timestamp for correct minute-of-day tracking
        // Example: 2024-01-20 12:00:00 UTC (1705752000)
        uint32_t initial_timestamp = 1705752000;
        ret = juxta_vitals_set_timestamp(&vitals_ctx, initial_timestamp);
        if (ret < 0)
        {
            LOG_ERR("Failed to set timestamp: %d", ret);
            return ret;
        }

        LOG_INF("✅ RTC timestamp set to: %u", initial_timestamp);
    }

    /* Get current timestamp (either from BLE or test) */
    current_timestamp = juxta_vitals_get_timestamp(&vitals_ctx);
    LOG_INF("📅 Current timestamp: %u", current_timestamp);

    uint32_t date = juxta_vitals_get_date_yyyymmdd(&vitals_ctx);
    uint32_t time = juxta_vitals_get_time_hhmmss(&vitals_ctx);
    LOG_INF("📅 Date: %u, Time: %u", date, time);

    uint32_t time_until_action = juxta_vitals_get_time_until_next_action(
        &vitals_ctx, ADV_INTERVAL_SECONDS, SCAN_INTERVAL_SECONDS, 0, 0);
    LOG_INF("⏱️ Time until next action: %u seconds", time_until_action);

    LOG_INF("✅ RTC functionality test completed successfully");
    return 0;
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err)
    {
        LOG_ERR("Connection failed (err %u)", err);
        return;
    }

    LOG_INF("🔗 Connected to peer device");
    ble_connected = true; // Mark as connected

    // Stop any ongoing advertising or scanning (guarded)
    (void)juxta_stop_advertising();
    (void)juxta_stop_scanning();
    connectable_adv_active = false;

    /* Notify BLE service of connection */
    juxta_ble_connection_established(conn);

    /* Log CONNECTED event (before pausing FRAMFS operations) */
    juxta_log_simple(JUXTA_FRAMFS_RECORD_TYPE_CONNECTED);

    LOG_INF("⏸️ FRAMFS logging operations paused during BLE connection");

    LOG_INF("📤 Hublink gateway connected - ready for data exchange");
    LOG_INF("⏸️ State machine paused - will resume after disconnection");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("🔌 Disconnected from peer (reason %u)", reason);
    ble_connected = false; // Mark as disconnected
    ble_state = BLE_STATE_IDLE;

    /* Notify BLE service of disconnection */
    juxta_ble_connection_terminated();

    /* Production flow: Check if datetime was synchronized during initial boot */
    if (magnet_activated && !datetime_synchronized)
    {
        datetime_sync_retry_count++;
        LOG_INF("⏰ Initial boot: Datetime not yet synchronized - scheduling connectable advertising restart (attempt %d)", datetime_sync_retry_count);

        // Limit retries to prevent infinite loops
        if (datetime_sync_retry_count > 5)
        {
            LOG_ERR("❌ Too many datetime sync retries - proceeding to normal operation");
            datetime_synchronized = true; // Force proceed to avoid infinite loop
            datetime_sync_retry_count = 0;
        }
        else
        {
            // Schedule async restart to avoid BLE stack conflicts
            if (!connectable_adv_active)
            {
                k_work_submit(&datetime_sync_restart_work);
            }
        }
    }
    else
    {
        /* Normal operation - resume appropriate mode */
        {
            /* Avoid submitting work before state system is initialized */
            if (!state_system_ready)
            {
                LOG_DBG("State system not ready yet; skipping resume on disconnect");
                return;
            }
            uint8_t current_mode = OPERATING_MODE_NORMAL; // Default to normal mode
            if (framfs_ctx.initialized)
            {
                juxta_framfs_get_operating_mode(&framfs_ctx, &current_mode);
            }

            if (current_mode == OPERATING_MODE_NORMAL)
            {
                last_adv_timestamp = get_rtc_timestamp() - get_adv_interval();
                last_scan_timestamp = get_rtc_timestamp() - get_scan_interval();

                LOG_INF("▶️ FRAMFS logging operations resumed");
                LOG_INF("▶️ State machine resumed - resuming normal operation");
                k_work_submit(&state_work);
            }
            else if (current_mode == OPERATING_MODE_ADC_ONLY)
            {
                LOG_INF("▶️ ADC_ONLY mode resumed - ADC timer continues");
                // ADC timer continues automatically
            }
        }
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

// Add the missing connectable advertising function
static int juxta_start_connectable_advertising(void)
{
    // Explicit connectable advertising parameters using modern option
    struct bt_le_adv_param adv_param = {
        .id = BT_ID_DEFAULT,
        .sid = 0,
        .secondary_max_skip = 0,
        .options = BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_IDENTITY,
        .interval_min = BT_GAP_ADV_FAST_INT_MIN_1,
        .interval_max = BT_GAP_ADV_FAST_INT_MAX_1,
        .peer = NULL,
    };

    // Include the JUXTA Hublink service UUID (derive bytes from Zephyr UUID)
    const struct bt_uuid_128 *svc_uuid = (const struct bt_uuid_128 *)BT_UUID_JUXTA_HUBLINK_SERVICE;
    uint8_t juxta_service_uuid_le[16];
    memcpy(juxta_service_uuid_le, svc_uuid->val, sizeof(juxta_service_uuid_le));

    // Comprehensive advertising data for maximum discoverability
    struct bt_data adv_data[] = {
        BT_DATA(BT_DATA_FLAGS, (uint8_t[]){BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR}, 1),
        BT_DATA(BT_DATA_UUID128_ALL, juxta_service_uuid_le, sizeof(juxta_service_uuid_le)),
        BT_DATA(BT_DATA_NAME_COMPLETE, adv_name, strlen(adv_name)),
    };

    // Add scan response data for additional information
    struct bt_data scan_data[] = {
        BT_DATA(BT_DATA_NAME_COMPLETE, adv_name, strlen(adv_name)),
    };

    int ret = bt_le_adv_start(&adv_param, adv_data, ARRAY_SIZE(adv_data),
                              scan_data, ARRAY_SIZE(scan_data));
    if (ret < 0)
    {
        LOG_ERR("Connectable advertising failed to start (err %d)", ret);
    }
    else
    {
        LOG_INF("🔔 Connectable advertising started as '%s' (30s window, public, ~200ms intervals)", adv_name);
    }
    return ret;
}

// Magnet sensor and LED definitions using Zephyr device tree (currently unused)
static const struct gpio_dt_spec magnet_sensor __unused = GPIO_DT_SPEC_GET(DT_PATH(gpio_keys, magnet_sensor), gpios);
static const struct gpio_dt_spec led __unused = GPIO_DT_SPEC_GET(DT_PATH(leds, led_0), gpios);

static void blink_led_three_times(void) __unused;
static void blink_led_three_times(void)
{
    LOG_INF("💡 Blinking LED three times to indicate wake-up");
    for (int i = 0; i < 3; i++)
    {
        gpio_pin_set_dt(&led, 1); // LED ON (active high)
        k_sleep(K_MSEC(200));
        gpio_pin_set_dt(&led, 0); // LED OFF
        k_sleep(K_MSEC(200));
    }
    gpio_pin_set_dt(&led, 0); // Ensure LED is off
    LOG_INF("✅ LED blink sequence completed");
}

static void wait_for_magnet_sensor(void) __unused;
static void wait_for_magnet_sensor(void)
{
    LOG_INF("🧲 Waiting for magnet sensor to go high (active)...");
    if (!device_is_ready(magnet_sensor.port))
    {
        LOG_ERR("❌ Magnet sensor device not ready");
        return;
    }
    if (!device_is_ready(led.port))
    {
        LOG_ERR("❌ LED device not ready");
        return;
    }

    // Configure pins manually since device tree doesn't always configure them
    int ret = gpio_pin_configure(magnet_sensor.port, magnet_sensor.pin, GPIO_INPUT); // No flags, no pull-up
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to configure magnet sensor: %d", ret);
        return;
    }
    ret = gpio_pin_configure(led.port, led.pin, GPIO_OUTPUT_ACTIVE | GPIO_ACTIVE_HIGH);
    if (ret < 0)
    {
        LOG_ERR("❌ Failed to configure LED: %d", ret);
        return;
    }

    gpio_pin_set_dt(&led, 0); // LED OFF initially
    while (gpio_pin_get_dt(&magnet_sensor))
    {
        LOG_INF("💤 Waiting for magnet sensor activation (debug every 1s)...");
        k_sleep(K_SECONDS(1));
    }
    LOG_INF("🔔 Magnet sensor activated! Waking up...");
    blink_led_three_times();
}

static void ten_minute_timeout(struct k_timer *timer)
{
    doGatewayAdvertise = false;
}

/**
 * @brief Callback function called when datetime is synchronized via BLE
 */
static void datetime_synchronized_callback(void)
{
    datetime_synchronized = true;
    datetime_sync_retry_count = 0; // Reset retry counter on success
    LOG_INF("✅ Datetime synchronization callback triggered");
}

/**
 * @brief Work handler to restart connectable advertising asynchronously
 */
static void datetime_sync_restart_work_handler(struct k_work *work)
{
    LOG_INF("🔄 Attempting to restart connectable advertising (async)");

    // Ensure BLE is in a clean state
    k_sleep(K_MSEC(500)); // Longer delay for BLE stack cleanup

    // Stop any ongoing advertising first
    bt_le_adv_stop();
    k_sleep(K_MSEC(200)); // Additional delay for radio stability

    // Restart connectable advertising for datetime sync
    int ret = juxta_start_connectable_advertising();
    if (ret < 0)
    {
        LOG_ERR("Async connectable advertising restart failed: %d", ret);
    }
    else
    {
        LOG_INF("🔔 Connectable advertising restarted asynchronously");
        connectable_adv_active = true;
    }
}

int main(void)
{
    int ret;

    LOG_INF("🚀 Starting JUXTA BLE Application");

    /* Check for watchdog reset */
    uint32_t reset_reason = NRF_POWER->RESETREAS;
    if (reset_reason & POWER_RESETREAS_DOG_Msk)
    {
        watchdog_reset_detected = true;
        LOG_INF("🔍 Watchdog reset detected (RESETREAS: 0x%08X)", reset_reason);
    }
    else
    {
        LOG_INF("🔍 Normal boot (RESETREAS: 0x%08X)", reset_reason);
    }

    /* Clear reset reason register */
    NRF_POWER->RESETREAS = reset_reason;

    // Wait for magnet sensor activation before starting BLE
    wait_for_magnet_sensor();
    magnet_activated = true;
    LOG_INF("🧲 Magnet activated - starting datetime synchronization phase");

    LOG_INF("⏰ Starting connectable advertising for datetime synchronization...");
    // Start connectable advertising and wait for datetime sync
    ret = bt_enable(NULL);
    if (ret)
    {
        LOG_ERR("Bluetooth init failed (err %d)", ret);
        return ret;
    }

    LOG_INF("Bluetooth initialized for datetime sync");

    /* Initialize vitals early so timestamp sync can succeed */
    ret = juxta_vitals_init(&vitals_ctx, true);
    if (ret < 0)
    {
        LOG_ERR("Vitals init failed (err %d)", ret);
        return ret;
    }
    juxta_ble_set_vitals_context(&vitals_ctx);

    /* Initialize watchdog feed timer early */
    k_timer_init(&wdt_feed_timer, wdt_feed_timer_callback, NULL);

    /* Minimal FRAM + framfs init so sendFilenames can work during the
     * initial connectable session. Heavier init (time-aware FS, LIS2DH, etc.)
     * happens after we disconnect from the gateway.
     */
    LOG_INF("📁 Initializing FRAM device (pre-sync minimal)...");
    ret = init_fram_and_framfs(&fram_dev, &framfs_ctx, true);
    if (ret < 0)
    {
        LOG_ERR("FRAM/framfs init failed: %d", ret);
        return ret;
    }
    juxta_ble_set_framfs_context(&framfs_ctx);

    setup_dynamic_adv_name();
    ret = juxta_ble_service_init();
    if (ret < 0)
    {
        LOG_ERR("BLE service init failed (err %d)", ret);
        return ret;
    }

    /* Set up datetime synchronization callback for production flow */
    juxta_ble_set_datetime_sync_callback(datetime_synchronized_callback);

    // Start connectable advertising and wait for datetime synchronization
    // Ensure work handler is initialized before any scheduling
    k_work_init(&datetime_sync_restart_work, datetime_sync_restart_work_handler);
    ret = juxta_start_connectable_advertising();
    if (ret < 0)
    {
        LOG_ERR("Failed to start connectable advertising for datetime sync: %d", ret);
        return ret;
    }

    LOG_INF("🔔 Connectable advertising started - waiting for datetime synchronization...");
    connectable_adv_active = true;

    // Wait for datetime synchronization (this will be set in the BLE service)
    while (!datetime_synchronized)
    {
        k_sleep(K_MSEC(100));
    }

    LOG_INF("✅ Datetime synchronized successfully");
    /* Stay connected and serve filenames. Do not proceed with production
     * initialization until we disconnect from the gateway.
     */
    LOG_INF("⏳ Waiting for disconnect before production initialization...");
    while (ble_connected)
    {
        k_sleep(K_MSEC(50));
    }

    /* Initialize FRAM device and framfs */
    LOG_INF("📁 Initializing FRAM device...");
    ret = init_fram_and_framfs(&fram_dev, &framfs_ctx, true);
    if (ret < 0)
    {
        LOG_ERR("FRAM/framfs init failed: %d", ret);
        return ret;
    }

    /* Link framfs context to BLE service */
    juxta_ble_set_framfs_context(&framfs_ctx);

    ret = test_rtc_functionality();
    if (ret < 0)
    {
        LOG_ERR("RTC test failed (err %d)", ret);
        return ret;
    }

    /* Link vitals context to BLE service for timestamp synchronization */
    juxta_ble_set_vitals_context(&vitals_ctx);

    /* Initialize time-aware file system after vitals are ready */
    LOG_INF("📁 Initializing time-aware file system...");
    ret = juxta_framfs_init_with_time(&time_ctx, &framfs_ctx, juxta_vitals_get_file_date_wrapper, true);
    if (ret < 0)
    {
        LOG_ERR("Time-aware framfs init failed: %d", ret);
        return ret;
    }

    init_randomization();
    k_work_init(&state_work, state_work_handler);
    k_timer_init(&state_timer, state_timer_callback, NULL);
    state_system_ready = true;

    /* Quick vitals sanity read in thread context */
    (void)juxta_vitals_update(&vitals_ctx);
    uint8_t battery_level = juxta_vitals_get_battery_percent(&vitals_ctx);
    int8_t temperature = juxta_vitals_get_temperature(&vitals_ctx);
    LOG_DBG("Vitals init: battery=%u%%, temp=%dC", battery_level, temperature);

    // Initialize 10-minute timer (now only for gateway advertising timeout)
    k_timer_init(&ten_minute_timer, ten_minute_timeout, NULL);

    uint32_t current_time = get_rtc_timestamp();
    last_adv_timestamp = current_time - get_adv_interval();
    last_scan_timestamp = current_time - get_scan_interval();
    last_logged_minute = 0xFFFF; // Initialize last_logged_minute

    // Quick hardware verification
    LOG_INF("🔧 Hardware verification...");

    // Test FRAM functionality
    test_fram_functionality();

    // Initialize LIS2DH motion system
    ret = lis2dh12_init_motion_system();
    if (ret < 0)
    {
        LOG_WRN("⚠️ LIS2DH motion system initialization failed, continuing without motion detection");
    }

    // Initialize ADC system
    ret = juxta_adc_init();
    if (ret < 0)
    {
        LOG_WRN("⚠️ ADC initialization failed, continuing without ADC functionality");
    }
    else
    {
        LOG_INF("✅ ADC system initialized successfully");
    }

    LOG_INF("✅ Hardware verification complete (FRAM + LIS2DH + ADC)");
    hardware_verified = true;

    /* Log BOOT event now that hardware is verified */
    juxta_log_simple(JUXTA_FRAMFS_RECORD_TYPE_BOOT);

    /* Start appropriate mode based on operating mode setting */
    uint8_t current_mode = OPERATING_MODE_NORMAL; // Default to normal mode
    if (framfs_ctx.initialized)
    {
        juxta_framfs_get_operating_mode(&framfs_ctx, &current_mode);
        LOG_INF("🔧 Operating mode read from FRAMFS: %d", current_mode);
    }
    else
    {
        LOG_WRN("⚠️ FRAMFS not initialized, using default operating mode: %d", current_mode);
    }

    if (current_mode == OPERATING_MODE_NORMAL)
    {
        /* Mode 0: Start state machine for BLE bursts/motion counting */
        k_work_submit(&state_work);
        k_timer_start(&state_timer, K_NO_WAIT, K_NO_WAIT); // triggers EVENT_TIMER_EXPIRED immediately
        LOG_INF("✅ JUXTA BLE Application started in NORMAL mode (BLE bursts/motion counting)");
    }
    else if (current_mode == OPERATING_MODE_ADC_ONLY)
    {
        /* Mode 1: Start ADC timer for pure ADC recordings - no state machine needed */
        k_work_init(&adc_work, adc_work_handler);
        k_timer_init(&adc_timer, adc_timer_callback, NULL);
        k_timer_start(&adc_timer, K_SECONDS(1), K_SECONDS(1)); // Every 1 second
        LOG_INF("✅ JUXTA BLE Application started in ADC_ONLY mode (pure ADC recordings)");
        LOG_INF("📊 ADC_ONLY mode: State machine disabled - ADC timer active (1s intervals)");
    }
    else
    {
        LOG_WRN("⚠️ Unknown operating mode: %d, defaulting to NORMAL mode", current_mode);
        k_work_submit(&state_work);
        k_timer_start(&state_timer, K_NO_WAIT, K_NO_WAIT);
    }

    // Initialize watchdog timer
    if (!device_is_ready(wdt))
    {
        LOG_ERR("Watchdog device not ready");
        return -ENODEV;
    }

    struct wdt_timeout_cfg wdt_config = {
        .window = {
            .min = 0,
            .max = WDT_TIMEOUT_MS,
        },
        .callback = NULL,
        .flags = WDT_FLAG_RESET_SOC,
    };

    wdt_channel_id = wdt_install_timeout(wdt, &wdt_config);
    if (wdt_channel_id < 0)
    {
        LOG_ERR("Failed to install watchdog timeout: %d", wdt_channel_id);
        return wdt_channel_id;
    }

    int setup_err = wdt_setup(wdt, 0);
    if (setup_err < 0)
    {
        LOG_ERR("Failed to setup watchdog: %d", setup_err);
        return setup_err;
    }

    LOG_INF("🛡️ Watchdog timer initialized (30s timeout)");

    /* Start watchdog feed timer immediately after watchdog initialization */
    k_timer_start(&wdt_feed_timer, K_SECONDS(5), K_SECONDS(5));
    LOG_INF("🛡️ Watchdog feed timer started (5s intervals)");

    uint32_t heartbeat_counter = 0;
    while (1)
    {
        k_sleep(K_SECONDS(10));
        heartbeat_counter++;
        LOG_INF("System heartbeat: %u (uptime: %u seconds)",
                heartbeat_counter, heartbeat_counter * 10);
    }

    return 0;
}