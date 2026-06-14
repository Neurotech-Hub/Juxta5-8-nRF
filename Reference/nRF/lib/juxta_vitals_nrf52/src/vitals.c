/*
 * JUXTA Vitals Library for nRF52 - Implementation
 *
 * Copyright (c) 2024 NeurotechHub
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/sys/util.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <hal/nrf_rtc.h>
#include "juxta_vitals_nrf52/vitals.h"

LOG_MODULE_REGISTER(juxta_vitals_nrf52, CONFIG_JUXTA_VITALS_NRF52_LOG_LEVEL);

/* Forward declarations for static functions */
static int juxta_vitals_read_battery_voltage(struct juxta_vitals_ctx *ctx);
static int juxta_vitals_read_temperature(struct juxta_vitals_ctx *ctx);

/* ADC configuration */
static const struct device *adc_dev;
static struct adc_channel_cfg adc_cfg;
static struct adc_sequence adc_seq = {
    .buffer = NULL,
    .buffer_size = 0,
};

/* Temperature sensor device */
static const struct device *temp_dev;

/* RTC device for power-efficient timing (using counter API) - DISABLED for BLE app */
static const struct device *rtc_dev = NULL; /* Disabled to avoid conflicts with BLE RTC */
static bool rtc_alarm_set = false;
static bool rtc_alarm_fired = false;
static uint32_t rtc_start_time = 0;

/* ADC buffer */
static int16_t adc_sample_buffer;

/* ========================================================================
 * Core Functions
 * ======================================================================== */

int juxta_vitals_init(struct juxta_vitals_ctx *ctx, bool enable_battery_monitoring)
{
    if (!ctx)
    {
        return -EINVAL;
    }

    /* Initialize context */
    memset(ctx, 0, sizeof(*ctx));
    ctx->initialized = true;
    ctx->battery_monitoring = enable_battery_monitoring;
    ctx->temperature_monitoring = true;        // Always enable temperature monitoring
    ctx->microsecond_tracking_enabled = false; // Will be enabled when BLE timestamp is set

    /* RTC device disabled for BLE application to avoid conflicts */
    LOG_INF("RTC device disabled - using uptime-based timing for BLE compatibility");

    /* Initialize temperature sensor */
    temp_dev = DEVICE_DT_GET(DT_NODELABEL(temp));
    if (!device_is_ready(temp_dev))
    {
        LOG_ERR("Temperature sensor not ready");
        ctx->temperature_monitoring = false;
        return JUXTA_VITALS_ERROR_HARDWARE;
    }
    LOG_INF("Temperature monitoring enabled");

    /* Initialize ADC for battery monitoring */
    if (ctx->battery_monitoring)
    {
        adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc));
        if (!device_is_ready(adc_dev))
        {
            LOG_ERR("ADC device not ready");
            ctx->battery_monitoring = false;
            return JUXTA_VITALS_ERROR_HARDWARE;
        }

        LOG_DBG("ADC Device Info:");
        LOG_DBG("  Name: %s", adc_dev->name);
        LOG_DBG("  Status: %s", device_is_ready(adc_dev) ? "Ready" : "Not Ready");

        /* Configure ADC channel for VDD measurement */
        memset(&adc_cfg, 0, sizeof(adc_cfg));
        adc_cfg.gain = ADC_GAIN_1_6;
        adc_cfg.reference = ADC_REF_INTERNAL;
        adc_cfg.acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40);
        adc_cfg.channel_id = 1;
        adc_cfg.input_positive = SAADC_CH_PSELP_PSELP_VDD;

        int ret = adc_channel_setup(adc_dev, &adc_cfg);
        if (ret != 0)
        {
            LOG_ERR("Failed to setup ADC channel: %d", ret);
            LOG_ERR("  Gain: %d", adc_cfg.gain);
            LOG_ERR("  Reference: %d", adc_cfg.reference);
            LOG_ERR("  Input Positive: %d", adc_cfg.input_positive);
            ctx->battery_monitoring = false;
            return JUXTA_VITALS_ERROR_HARDWARE;
        }

        /* Configure ADC sequence */
        memset(&adc_seq, 0, sizeof(adc_seq));
        adc_seq.channels = BIT(1);
        adc_seq.buffer = &adc_sample_buffer;
        adc_seq.buffer_size = sizeof(adc_sample_buffer);
        adc_seq.resolution = 14;
        adc_seq.oversampling = 8;
        adc_seq.calibrate = true;

        LOG_DBG("ADC Configuration Complete:");
        LOG_DBG("  Channel: %d", adc_cfg.channel_id);
        LOG_DBG("  Resolution: %d bits", adc_seq.resolution);
        LOG_DBG("  Oversampling: %d", adc_seq.oversampling);
        LOG_DBG("  Buffer Size: %d bytes", adc_seq.buffer_size);

        /* Try an initial reading to verify setup */
        ret = juxta_vitals_read_battery_voltage(ctx);
        if (ret != 0)
        {
            LOG_ERR("Initial battery reading failed: %d", ret);
            /* Don't fail initialization, just warn */
        }

        LOG_INF("Battery monitoring enabled");
    }

    return JUXTA_VITALS_OK;
}

int juxta_vitals_update(struct juxta_vitals_ctx *ctx)
{
    if (!ctx || !ctx->initialized)
    {
        return JUXTA_VITALS_ERROR_NOT_READY;
    }

    uint32_t current_time = k_uptime_get_32();
    uint32_t elapsed = current_time - ctx->last_update_time;

    /* Update uptime */
    ctx->uptime_seconds = k_uptime_get_32() / 1000;

    /* Update battery voltage if monitoring is enabled */
    if (ctx->battery_monitoring && elapsed >= CONFIG_JUXTA_VITALS_NRF52_BATTERY_UPDATE_INTERVAL * 1000)
    {
        int ret = juxta_vitals_read_battery_voltage(ctx);
        if (ret < 0)
        {
            LOG_WRN("Failed to read battery voltage: %d", ret);
        }
        ctx->last_update_time = current_time;
    }

    /* Update temperature if monitoring is enabled */
    if (ctx->temperature_monitoring)
    {
        int ret = juxta_vitals_read_temperature(ctx);
        if (ret < 0)
        {
            LOG_WRN("Failed to read temperature: %d", ret);
        }
    }

    return JUXTA_VITALS_OK;
}

int juxta_vitals_get_summary(struct juxta_vitals_ctx *ctx, char *buffer, size_t size)
{
    if (!ctx || !buffer || size == 0)
    {
        return JUXTA_VITALS_ERROR_INVALID_PARAM;
    }

    int written = snprintf(buffer, size,
                           "Vitals: Uptime=%us, Battery=%d%%(%dmV), Temp=%d°C",
                           ctx->uptime_seconds,
                           ctx->battery_percent,
                           ctx->battery_mv,
                           ctx->temperature);

    if (written >= (int)size)
    {
        return JUXTA_VITALS_ERROR_INVALID_PARAM; /* Buffer too small */
    }

    return written;
}

/* ========================================================================
 * RTC Functions
 * ======================================================================== */

int juxta_vitals_set_timestamp(struct juxta_vitals_ctx *ctx, uint32_t timestamp)
{
    if (!ctx)
    {
        return JUXTA_VITALS_ERROR_INVALID_PARAM;
    }

    /* Store the timestamp and record the current uptime for reference */
    ctx->current_timestamp = timestamp;
    rtc_start_time = k_uptime_get_32();

    /* Set microsecond reference for RTC0-based timing */
    ctx->microsecond_reference = NRF_RTC0->COUNTER;
    ctx->microsecond_tracking_enabled = true;

    LOG_INF("Timestamp set to %u (uptime: %u ms, RTC0: %u)", timestamp, rtc_start_time, ctx->microsecond_reference);
    return JUXTA_VITALS_OK;
}

uint32_t juxta_vitals_get_timestamp(struct juxta_vitals_ctx *ctx)
{
    if (!ctx)
    {
        return 0;
    }

    if (ctx->current_timestamp == 0)
    {
        LOG_DBG("No timestamp set yet");
        return 0;
    }

    /* Calculate elapsed time since timestamp was set */
    uint32_t current_uptime = k_uptime_get_32();
    uint32_t elapsed_seconds = (current_uptime - rtc_start_time) / 1000;

    /* Add elapsed seconds to the stored timestamp */
    uint32_t current_timestamp = ctx->current_timestamp + elapsed_seconds;

    LOG_DBG("Current timestamp: %u (elapsed: %u seconds)", current_timestamp, elapsed_seconds);

    return current_timestamp;
}

uint64_t juxta_vitals_get_timestamp_with_microseconds(struct juxta_vitals_ctx *ctx)
{
    if (!ctx || !ctx->initialized)
    {
        return 0;
    }

    uint32_t unix_timestamp = juxta_vitals_get_timestamp(ctx);
    if (unix_timestamp == 0)
    {
        return 0;
    }

    if (!ctx->microsecond_tracking_enabled)
    {
        /* Return Unix timestamp with zero microseconds if microsecond tracking not available */
        return ((uint64_t)unix_timestamp << 32);
    }

    /* Use the new helper function to get microsecond offset within current second */
    uint32_t microseconds = juxta_vitals_get_microsecond_offset(ctx);

    /* Combine Unix timestamp (upper 32 bits) with microseconds (lower 32 bits) */
    return ((uint64_t)unix_timestamp << 32) | microseconds;
}

uint32_t juxta_vitals_get_microsecond_offset(struct juxta_vitals_ctx *ctx)
{
    if (!ctx || !ctx->initialized || !ctx->microsecond_tracking_enabled)
    {
        return 0;
    }

    /* Calculate microsecond offset from RTC0 counter */
    uint32_t current_rtc_ticks = NRF_RTC0->COUNTER;
    uint32_t elapsed_ticks = current_rtc_ticks - ctx->microsecond_reference;

    /* Convert ticks to microseconds (32kHz = 32768 ticks per second) */
    uint32_t microseconds = (elapsed_ticks * 1000000) / 32768;

    /* Return microseconds within current second (0-999999) */
    return microseconds % 1000000;
}

uint32_t juxta_vitals_get_rel_microseconds(struct juxta_vitals_ctx *ctx)
{
    if (!ctx || !ctx->initialized || !ctx->microsecond_tracking_enabled)
    {
        return 0;
    }

    /* Calculate microsecond offset from RTC0 counter */
    uint32_t current_rtc_ticks = NRF_RTC0->COUNTER;
    uint32_t elapsed_ticks = current_rtc_ticks - ctx->microsecond_reference;

    /* Convert ticks to microseconds (32kHz = 32768 ticks per second) */
    uint32_t microseconds = (elapsed_ticks * 1000000) / 32768;

    /* Return total microseconds since BLE sync (no modulo - full 32-bit range) */
    return microseconds;
}

uint32_t juxta_vitals_get_rel_microseconds_to_unix(struct juxta_vitals_ctx *ctx)
{
    if (!ctx || !ctx->initialized || !ctx->microsecond_tracking_enabled)
    {
        return 0;
    }

    /* Calculate microsecond offset from RTC0 counter */
    uint32_t current_rtc_ticks = NRF_RTC0->COUNTER;
    uint32_t elapsed_ticks = current_rtc_ticks - ctx->microsecond_reference;

    /* Convert ticks to microseconds (32kHz = 32768 ticks per second) */
    uint32_t microseconds = (elapsed_ticks * 1000000) / 32768;

    /* Return microseconds within current second (0-999999) */
    return microseconds % 1000000;
}

uint32_t juxta_vitals_get_date_yyyymmdd(struct juxta_vitals_ctx *ctx)
{
    if (!ctx || ctx->current_timestamp == 0)
    {
        return 0;
    }

    /* Convert Unix timestamp to date */
    uint32_t days_since_epoch = ctx->current_timestamp / 86400;
    uint32_t year = 1970;
    uint32_t month = 1;
    uint32_t day = 1;

    /* Simple date calculation (approximate) */
    while (days_since_epoch > 365)
    {
        if ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)
        {
            if (days_since_epoch > 366)
            {
                days_since_epoch -= 366;
                year++;
            }
            else
            {
                break;
            }
        }
        else
        {
            days_since_epoch -= 365;
            year++;
        }
    }

    /* Calculate month and day */
    uint32_t days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)
    {
        days_in_month[1] = 29; /* February in leap year */
    }

    for (int i = 0; i < 12; i++)
    {
        if (days_since_epoch <= days_in_month[i])
        {
            month = i + 1;
            day = days_since_epoch + 1; /* Add 1 since days are 1-based */
            break;
        }
        days_since_epoch -= days_in_month[i];
    }

    return year * 10000 + month * 100 + day;
}

uint32_t juxta_vitals_get_time_hhmmss(struct juxta_vitals_ctx *ctx)
{
    if (!ctx || ctx->current_timestamp == 0)
    {
        return 0;
    }

    /* Get seconds since midnight */
    uint32_t seconds_since_midnight = ctx->current_timestamp % 86400;

    uint32_t hour = seconds_since_midnight / 3600;
    uint32_t minute = (seconds_since_midnight % 3600) / 60;
    uint32_t second = seconds_since_midnight % 60;

    return hour * 10000 + minute * 100 + second;
}

/* ========================================================================
 * Battery Functions
 * ======================================================================== */

static int juxta_vitals_read_battery_voltage(struct juxta_vitals_ctx *ctx)
{
    if (!ctx || !ctx->battery_monitoring || !adc_dev)
    {
        LOG_WRN("Battery monitoring not ready");
        return JUXTA_VITALS_ERROR_NOT_READY;
    }

    LOG_DBG("ADC Configuration:");
    LOG_DBG("  Device: %s", adc_dev->name);
    LOG_DBG("  Channel: %d", adc_cfg.channel_id);
    LOG_DBG("  Gain: %d", adc_cfg.gain);
    LOG_DBG("  Reference: %d", adc_cfg.reference);
    LOG_DBG("  Input Positive: %d", adc_cfg.input_positive);
    LOG_DBG("  Resolution: %d", adc_seq.resolution);
    LOG_DBG("  Oversampling: %d", adc_seq.oversampling);

    /* Read VDD using ADC */
    int ret = adc_read(adc_dev, &adc_seq);
    if (ret != 0)
    {
        LOG_ERR("ADC read failed: %d", ret);
        return ret;
    }

    LOG_DBG("ADC Raw Value: %d", adc_sample_buffer);

    /* Convert to millivolts */
    int32_t vdd_mv = adc_sample_buffer;
    ret = adc_raw_to_millivolts(adc_ref_internal(adc_dev),
                                adc_cfg.gain,
                                adc_seq.resolution,
                                &vdd_mv);
    if (ret != 0)
    {
        LOG_ERR("ADC conversion failed: %d", ret);
        return ret;
    }

    LOG_DBG("ADC Conversion:");
    LOG_DBG("  Raw Value: %d", adc_sample_buffer);
    LOG_DBG("  Reference (mV): %d", adc_ref_internal(adc_dev));
    LOG_DBG("  Converted (mV): %d", vdd_mv);
    LOG_DBG("  Expected VDD (3V): ~3000 mV");
    LOG_DBG("  Expected ADC reading (1/6 gain): ~500 mV");

    ctx->battery_mv = vdd_mv;

    /* Calculate battery percentage based on range between FULL and CRITICAL */
    if (ctx->battery_mv >= JUXTA_VITALS_BATTERY_FULL_MV)
    {
        ctx->battery_percent = 100;
    }
    else if (ctx->battery_mv <= JUXTA_VITALS_BATTERY_CRITICAL_MV)
    {
        ctx->battery_percent = 0;
    }
    else
    {
        uint32_t range = JUXTA_VITALS_BATTERY_FULL_MV - JUXTA_VITALS_BATTERY_CRITICAL_MV;
        uint32_t current = ctx->battery_mv - JUXTA_VITALS_BATTERY_CRITICAL_MV;
        ctx->battery_percent = (current * 100) / range;
    }

    /* Set low battery flag when voltage drops below critical threshold */
    ctx->low_battery = (ctx->battery_mv <= JUXTA_VITALS_BATTERY_CRITICAL_MV);

    return JUXTA_VITALS_OK;
}

uint16_t juxta_vitals_get_battery_mv(struct juxta_vitals_ctx *ctx)
{
    if (!ctx)
    {
        return 0;
    }
    return ctx->battery_mv;
}

uint8_t juxta_vitals_get_battery_percent(struct juxta_vitals_ctx *ctx)
{
    if (!ctx)
    {
        return 0;
    }
    return ctx->battery_percent;
}

bool juxta_vitals_is_low_battery(struct juxta_vitals_ctx *ctx)
{
    if (!ctx)
    {
        return false;
    }
    return ctx->low_battery;
}

/* ========================================================================
 * System Functions
 * ======================================================================== */

uint32_t juxta_vitals_get_uptime(struct juxta_vitals_ctx *ctx)
{
    if (!ctx)
    {
        return 0;
    }
    return ctx->uptime_seconds;
}

static int juxta_vitals_read_temperature(struct juxta_vitals_ctx *ctx)
{
    if (!ctx || !ctx->temperature_monitoring || !temp_dev)
    {
        LOG_WRN("Temperature monitoring not properly initialized");
        return JUXTA_VITALS_ERROR_NOT_READY;
    }

    /* Read internal temperature sensor */
    struct sensor_value temp;
    int ret = sensor_sample_fetch(temp_dev);
    if (ret < 0)
    {
        LOG_ERR("Failed to fetch temperature: %d", ret);
        return ret;
    }

    ret = sensor_channel_get(temp_dev, SENSOR_CHAN_DIE_TEMP, &temp);
    if (ret < 0)
    {
        LOG_ERR("Failed to get temperature: %d", ret);
        return ret;
    }

    /* Convert to integer Celsius */
    ctx->temperature = temp.val1;
    LOG_DBG("Temperature read: %d°C", ctx->temperature);

    return JUXTA_VITALS_OK;
}

int8_t juxta_vitals_get_temperature(struct juxta_vitals_ctx *ctx)
{
    if (!ctx)
    {
        return 0;
    }
    return ctx->temperature;
}

/* ========================================================================
 * Configuration Functions
 * ======================================================================== */

int juxta_vitals_set_battery_monitoring(struct juxta_vitals_ctx *ctx, bool enable)
{
    if (!ctx)
    {
        return JUXTA_VITALS_ERROR_INVALID_PARAM;
    }

    ctx->battery_monitoring = enable;
    LOG_INF("Battery monitoring %s", enable ? "enabled" : "disabled");
    return JUXTA_VITALS_OK;
}

int juxta_vitals_set_temperature_monitoring(struct juxta_vitals_ctx *ctx, bool enable)
{
    if (!ctx || !ctx->initialized)
    {
        return JUXTA_VITALS_ERROR_NOT_READY;
    }

    ctx->temperature_monitoring = enable;
    LOG_INF("Temperature monitoring %s", enable ? "enabled" : "disabled");
    return JUXTA_VITALS_OK;
}

/* ========================================================================
 * File System Integration Functions
 * ======================================================================== */

uint32_t juxta_vitals_get_file_date(struct juxta_vitals_ctx *ctx)
{
    if (!ctx || !ctx->initialized)
    {
        return 0;
    }

    /* Use the new YYMMDD format by default */
    return juxta_vitals_get_file_date_yymmdd(ctx);
}

uint32_t juxta_vitals_get_file_date_yymmdd(struct juxta_vitals_ctx *ctx)
{
    if (!ctx || !ctx->initialized || ctx->current_timestamp == 0)
    {
        return 0;
    }

    /* Get the live timestamp (includes elapsed time since sync) */
    uint32_t live_timestamp = juxta_vitals_get_timestamp(ctx);
    if (live_timestamp == 0)
    {
        return 0;
    }

    /* Convert live Unix timestamp to date */
    uint32_t days_since_epoch = live_timestamp / 86400;
    uint32_t year = 1970;
    uint32_t month = 1;
    uint32_t day = 1;

    /* Simple date calculation (approximate) */
    while (days_since_epoch > 365)
    {
        if ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)
        {
            if (days_since_epoch > 366)
            {
                days_since_epoch -= 366;
                year++;
            }
            else
            {
                break;
            }
        }
        else
        {
            days_since_epoch -= 365;
            year++;
        }
    }

    /* Calculate month and day */
    uint32_t days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)
    {
        days_in_month[1] = 29; /* February in leap year */
    }

    for (int i = 0; i < 12; i++)
    {
        if (days_since_epoch <= days_in_month[i])
        {
            month = i + 1;
            day = days_since_epoch + 1; /* Add 1 since days are 1-based */
            break;
        }
        days_since_epoch -= days_in_month[i];
    }

    /* Convert to YYMMDD format (assume 20XX) */
    uint32_t short_year = year % 100; /* Extract last 2 digits */
    uint32_t yymmdd = short_year * 10000 + month * 100 + day;

    LOG_DBG("📅 File date calculation: timestamp=%u, YYMMDD=%06u", live_timestamp, yymmdd);
    LOG_INF("📅 Current file date: %06u (timestamp: %u)", yymmdd, live_timestamp);

    return yymmdd;
}

uint16_t juxta_vitals_get_minute_of_day(struct juxta_vitals_ctx *ctx)
{
    if (!ctx || !ctx->initialized)
        return 0;

    uint32_t live_timestamp = juxta_vitals_get_timestamp(ctx);
    if (live_timestamp == 0)
        return 0;

    struct tm timeinfo;
    time_t time_sec = (time_t)live_timestamp;
    gmtime_r(&time_sec, &timeinfo);

    return (uint16_t)(timeinfo.tm_hour * 60 + timeinfo.tm_min);
}

bool juxta_vitals_validate_battery_level(uint8_t level)
{
    /* Battery level should be 0-100 */
    return level <= 100;
}

int juxta_vitals_get_validated_battery_level(struct juxta_vitals_ctx *ctx, uint8_t *level)
{
    if (!ctx || !ctx->initialized || !level)
    {
        return JUXTA_VITALS_ERROR_NOT_READY;
    }

    if (!ctx->battery_monitoring)
    {
        LOG_WRN("Battery monitoring not enabled");
        return JUXTA_VITALS_ERROR_NOT_READY;
    }

    /* Get current battery level */
    uint8_t current_level = juxta_vitals_get_battery_percent(ctx);

    /* Validate the level */
    if (!juxta_vitals_validate_battery_level(current_level))
    {
        LOG_WRN("Invalid battery level: %d", current_level);
        return JUXTA_VITALS_ERROR_INVALID_PARAM;
    }

    *level = current_level;
    return JUXTA_VITALS_OK;
}

/* ========================================================================
 * RTC Alarm Functions for Power-Efficient Timing
 * ======================================================================== */

int juxta_vitals_set_rtc_alarm(struct juxta_vitals_ctx *ctx, uint32_t seconds_from_now)
{
    if (!ctx || !ctx->initialized)
    {
        return JUXTA_VITALS_ERROR_NOT_READY;
    }

    uint32_t current_time = juxta_vitals_get_timestamp(ctx);
    if (current_time == 0)
    {
        LOG_ERR("RTC timestamp not set");
        return JUXTA_VITALS_ERROR_NOT_READY;
    }

    /* For now, just track the alarm time without using hardware RTC alarm */
    /* In a full implementation, this would set the actual RTC hardware alarm */
    rtc_alarm_set = true;
    rtc_alarm_fired = false;

    LOG_DBG("RTC alarm set for %u seconds from now (timestamp: %u)",
            seconds_from_now, current_time + seconds_from_now);
    LOG_WRN("Note: RTC alarm is a placeholder - not using hardware alarm");

    return JUXTA_VITALS_OK;
}

int juxta_vitals_cancel_rtc_alarm(struct juxta_vitals_ctx *ctx)
{
    if (!ctx)
    {
        return JUXTA_VITALS_ERROR_NOT_READY;
    }

    if (rtc_alarm_set)
    {
        rtc_alarm_set = false;
        LOG_DBG("RTC alarm cancelled");
    }

    return JUXTA_VITALS_OK;
}

bool juxta_vitals_rtc_alarm_fired(struct juxta_vitals_ctx *ctx)
{
    if (!ctx || !rtc_dev)
    {
        return false;
    }

    /* Check if alarm has fired */
    if (rtc_alarm_set)
    {
        uint32_t current_time = juxta_vitals_get_timestamp(ctx);
        if (current_time > 0)
        {
            /* Simple check - in a real implementation, you'd use RTC interrupt */
            rtc_alarm_fired = true;
            rtc_alarm_set = false;
        }
    }

    return rtc_alarm_fired;
}

uint32_t juxta_vitals_get_time_until_next_action(struct juxta_vitals_ctx *ctx,
                                                 uint32_t adv_interval_seconds,
                                                 uint32_t scan_interval_seconds,
                                                 uint32_t last_adv_time,
                                                 uint32_t last_scan_time)
{
    if (!ctx || !ctx->initialized)
    {
        return 0;
    }

    uint32_t current_time = juxta_vitals_get_timestamp(ctx);
    if (current_time == 0)
    {
        return 0; /* No timestamp set */
    }

    /* Calculate time until next advertising */
    uint32_t time_since_adv = current_time - last_adv_time;
    uint32_t time_until_adv = (time_since_adv >= adv_interval_seconds) ? 0 : (adv_interval_seconds - time_since_adv);

    /* Calculate time until next scanning */
    uint32_t time_since_scan = current_time - last_scan_time;
    uint32_t time_until_scan = (time_since_scan >= scan_interval_seconds) ? 0 : (scan_interval_seconds - time_since_scan);

    /* Return the minimum time (whichever action is due first) */
    if (time_until_adv == 0 || time_until_scan == 0)
    {
        return 0; /* Action is due now */
    }

    return (time_until_adv < time_until_scan) ? time_until_adv : time_until_scan;
}