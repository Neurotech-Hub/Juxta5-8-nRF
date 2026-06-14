/*
 * JUXTA Vitals Library for nRF52
 *
 * Copyright (c) 2024 NeurotechHub
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef JUXTA_VITALS_NRF52_H
#define JUXTA_VITALS_NRF52_H

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ========================================================================
 * Constants
 * ======================================================================== */

/* Voltage thresholds (millivolts) */
#define JUXTA_VITALS_BATTERY_FULL_MV 2800     /* 2.8V - 100% */
#define JUXTA_VITALS_BATTERY_CRITICAL_MV 2100 /* 2.1V - critical threshold before 1.8V minimum */

/* Temperature limits */
#define JUXTA_VITALS_TEMP_MIN_C -40 /* Minimum temperature */
#define JUXTA_VITALS_TEMP_MAX_C 85  /* Maximum temperature */

/* Error codes */
#define JUXTA_VITALS_OK 0
#define JUXTA_VITALS_ERROR_INIT -1
#define JUXTA_VITALS_ERROR_NOT_READY -2
#define JUXTA_VITALS_ERROR_INVALID_PARAM -3
#define JUXTA_VITALS_ERROR_HARDWARE -4

    /* ========================================================================
     * Vitals Context Structure
     * ======================================================================== */

    /**
     * @brief Vitals monitoring context
     */
    struct juxta_vitals_ctx
    {
        /* RTC state */
        uint32_t current_timestamp; /* Current Unix timestamp */
        uint32_t last_update_time;  /* Last update time (uptime) */

        /* Microsecond precision timing */
        uint32_t microsecond_reference;    /* RTC0 counter when timestamp was set */
        bool microsecond_tracking_enabled; /* Whether microsecond tracking is active */

        /* Battery state */
        uint16_t battery_mv;     /* Battery voltage in millivolts */
        uint8_t battery_percent; /* Battery percentage (0-100) */
        bool low_battery;        /* Low battery flag */

        /* System state */
        uint32_t uptime_seconds; /* System uptime in seconds */
        int8_t temperature;      /* Internal temperature (°C) */

        /* State flags */
        bool initialized;            /* Initialization state */
        bool battery_monitoring;     /* Battery monitoring enabled */
        bool temperature_monitoring; /* Temperature monitoring enabled */
    };

    /* ========================================================================
     * Core Functions
     * ======================================================================== */

    /**
     * Initialize the vitals context
     *
     * @param ctx Context to initialize
     * @param enable_battery_monitoring Whether to enable battery monitoring
     * @return 0 on success, negative error code on failure
     */
    int juxta_vitals_init(struct juxta_vitals_ctx *ctx, bool enable_battery_monitoring);

    /**
     * @brief Update all vitals readings
     *
     * This function updates battery voltage, temperature, and uptime.
     * Call this periodically to keep vitals current.
     *
     * @param ctx Vitals context
     * @return 0 on success, negative error code on failure
     */
    int juxta_vitals_update(struct juxta_vitals_ctx *ctx);

    /**
     * @brief Get vitals summary as text
     *
     * @param ctx Vitals context
     * @param buffer Output buffer for summary text
     * @param size Buffer size
     * @return Number of bytes written, or negative error code
     */
    int juxta_vitals_get_summary(struct juxta_vitals_ctx *ctx,
                                 char *buffer, size_t size);

    /* ========================================================================
     * RTC Functions
     * ======================================================================== */

    /**
     * @brief Set current Unix timestamp
     *
     * @param ctx Vitals context
     * @param timestamp Unix timestamp (seconds since epoch)
     * @return 0 on success, negative error code on failure
     */
    int juxta_vitals_set_timestamp(struct juxta_vitals_ctx *ctx, uint32_t timestamp);

    /**
     * @brief Get current Unix timestamp
     *
     * @param ctx Vitals context
     * @return Current Unix timestamp, or 0 if not set
     */
    uint32_t juxta_vitals_get_timestamp(struct juxta_vitals_ctx *ctx);

    /**
     * @brief Get current timestamp with microsecond precision
     *
     * This function returns a 64-bit timestamp combining the Unix timestamp
     * with microsecond precision. The upper 32 bits contain the Unix timestamp
     * (seconds since epoch), and the lower 32 bits contain microseconds.
     *
     * @param ctx Vitals context
     * @return 64-bit timestamp (seconds << 32 | microseconds), or 0 if not set
     */
    uint64_t juxta_vitals_get_timestamp_with_microseconds(struct juxta_vitals_ctx *ctx);

    /**
     * @brief Get microsecond offset from current Unix timestamp
     *
     * This function returns the number of microseconds that have elapsed
     * since the start of the current second, based on the microsecond
     * reference captured during BLE timestamp synchronization.
     *
     * @param ctx Vitals context
     * @return Microseconds since start of current second (0-999999), or 0 if not available
     */
    uint32_t juxta_vitals_get_microsecond_offset(struct juxta_vitals_ctx *ctx);

    /**
     * @brief Get relative microseconds since BLE timestamp synchronization
     *
     * This function returns the number of microseconds that have elapsed
     * since the microsecond reference was captured during BLE timestamp
     * synchronization. This provides a consistent 32-bit microsecond
     * timestamp that's relative to the BLE sync point.
     *
     * @param ctx Vitals context
     * @return Microseconds since BLE sync (0-4294967295), or 0 if not available
     */
    uint32_t juxta_vitals_get_rel_microseconds(struct juxta_vitals_ctx *ctx);

    /**
     * @brief Get microsecond offset within current second relative to Unix timestamp
     *
     * This function returns the number of microseconds within the current second
     * (0-999999), providing microsecond precision for absolute timestamps.
     * This is ideal for the 8-byte header format combining Unix timestamp + microseconds.
     *
     * @param ctx Vitals context
     * @return Microseconds within current second (0-999999), or 0 if not available
     */
    uint32_t juxta_vitals_get_rel_microseconds_to_unix(struct juxta_vitals_ctx *ctx);

    /**
     * @brief Get current date in YYYYMMDD format
     *
     * @param ctx Vitals context
     * @return Date in YYYYMMDD format, or 0 if timestamp not set
     */
    uint32_t juxta_vitals_get_date_yyyymmdd(struct juxta_vitals_ctx *ctx);

    /**
     * @brief Get current time in HHMMSS format
     *
     * @param ctx Vitals context
     * @return Time in HHMMSS format, or 0 if timestamp not set
     */
    uint32_t juxta_vitals_get_time_hhmmss(struct juxta_vitals_ctx *ctx);

    /**
     * @brief Set RTC alarm for power-efficient wake-up
     *
     * This function sets an RTC alarm to wake the system at a specific time.
     * Used for power-efficient scheduling of BLE activities.
     *
     * @param ctx Vitals context
     * @param seconds_from_now Seconds from current time to set alarm
     * @return 0 on success, negative error code on failure
     */
    int juxta_vitals_set_rtc_alarm(struct juxta_vitals_ctx *ctx, uint32_t seconds_from_now);

    /**
     * @brief Cancel RTC alarm
     *
     * @param ctx Vitals context
     * @return 0 on success, negative error code on failure
     */
    int juxta_vitals_cancel_rtc_alarm(struct juxta_vitals_ctx *ctx);

    /**
     * @brief Check if RTC alarm has fired
     *
     * @param ctx Vitals context
     * @return true if alarm has fired, false otherwise
     */
    bool juxta_vitals_rtc_alarm_fired(struct juxta_vitals_ctx *ctx);

    /**
     * @brief Get time until next scheduled action
     *
     * Calculates the time until the next advertising or scanning burst
     * should occur based on the current timestamp.
     *
     * @param ctx Vitals context
     * @param adv_interval_seconds Advertising interval in seconds
     * @param scan_interval_seconds Scanning interval in seconds
     * @param last_adv_time Last advertising time (Unix timestamp)
     * @param last_scan_time Last scanning time (Unix timestamp)
     * @return Seconds until next action, or 0 if action is due now
     */
    uint32_t juxta_vitals_get_time_until_next_action(struct juxta_vitals_ctx *ctx,
                                                     uint32_t adv_interval_seconds,
                                                     uint32_t scan_interval_seconds,
                                                     uint32_t last_adv_time,
                                                     uint32_t last_scan_time);

    /* ========================================================================
     * Battery Functions
     * ======================================================================== */

    /**
     * @brief Get battery voltage in millivolts
     *
     * @param ctx Vitals context
     * @return Battery voltage in millivolts
     */
    uint16_t juxta_vitals_get_battery_mv(struct juxta_vitals_ctx *ctx);

    /**
     * @brief Get battery percentage (0-100)
     *
     * @param ctx Vitals context
     * @return Battery percentage (0-100)
     */
    uint8_t juxta_vitals_get_battery_percent(struct juxta_vitals_ctx *ctx);

    /**
     * @brief Check if battery is low
     *
     * @param ctx Vitals context
     * @return true if battery is low, false otherwise
     */
    bool juxta_vitals_is_low_battery(struct juxta_vitals_ctx *ctx);

    /* ========================================================================
     * System Functions
     * ======================================================================== */

    /**
     * @brief Get system uptime in seconds
     *
     * @param ctx Vitals context
     * @return System uptime in seconds
     */
    uint32_t juxta_vitals_get_uptime(struct juxta_vitals_ctx *ctx);

    /**
     * @brief Get internal temperature
     *
     * @param ctx Vitals context
     * @return Temperature in degrees Celsius
     */
    int8_t juxta_vitals_get_temperature(struct juxta_vitals_ctx *ctx);

    /* ========================================================================
     * Configuration Functions
     * ======================================================================== */

    /**
     * @brief Enable or disable battery monitoring
     *
     * @param ctx Vitals context
     * @param enable true to enable, false to disable
     * @return 0 on success, negative error code on failure
     */
    int juxta_vitals_set_battery_monitoring(struct juxta_vitals_ctx *ctx, bool enable);

    /**
     * @brief Enable or disable temperature monitoring
     *
     * @param ctx Vitals context
     * @param enable true to enable, false to disable
     * @return 0 on success, negative error code on failure
     */
    int juxta_vitals_set_temperature_monitoring(struct juxta_vitals_ctx *ctx, bool enable);

    /* ========================================================================
     * File System Integration Functions
     * ======================================================================== */

    /**
     * @brief Get current date in YYYYMMDD format for file system operations
     *
     * This function is designed for integration with file systems that need
     * daily file management. It returns the current date in YYYYMMDD format
     * suitable for use as a filename.
     *
     * @param ctx Vitals context
     * @return Date in YYYYMMDD format, or 0 if timestamp not set
     */
    uint32_t juxta_vitals_get_file_date(struct juxta_vitals_ctx *ctx);

    /**
     * @brief Get current date in YYMMDD format for file system operations
     *
     * This function returns the current date in YYMMDD format (6 digits)
     * for use with the updated file system that uses shorter filenames.
     *
     * @param ctx Vitals context
     * @return Date in YYMMDD format, or 0 if timestamp not set
     */
    uint32_t juxta_vitals_get_file_date_yymmdd(struct juxta_vitals_ctx *ctx);

    /**
     * @brief Get current minute of day (0-1439) for time-series data
     *
     * This function converts the current timestamp to minutes since midnight,
     * which is commonly used for time-series data logging and file system
     * record timestamps.
     *
     * @param ctx Vitals context
     * @return Minutes since midnight (0-1439), or 0 if timestamp not set
     */
    uint16_t juxta_vitals_get_minute_of_day(struct juxta_vitals_ctx *ctx);

    /**
     * @brief Validate battery level for file system operations
     *
     * This function validates that a battery level is within acceptable
     * range (0-100) for storage in file system records.
     *
     * @param level Battery level to validate
     * @return true if valid (0-100), false otherwise
     */
    bool juxta_vitals_validate_battery_level(uint8_t level);

    /**
     * @brief Get battery level with validation for file system storage
     *
     * This function gets the current battery level and validates it for
     * storage in file system records. It's designed for integration with
     * file systems that need to store battery data.
     *
     * @param ctx Vitals context
     * @param level Pointer to store validated battery level
     * @return 0 on success, negative error code on failure
     */
    int juxta_vitals_get_validated_battery_level(struct juxta_vitals_ctx *ctx, uint8_t *level);

#ifdef __cplusplus
}
#endif

#endif /* JUXTA_VITALS_NRF52_H */