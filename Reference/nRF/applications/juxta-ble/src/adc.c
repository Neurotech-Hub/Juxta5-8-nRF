/*
 * JUXTA ADC Module Implementation
 * Provides functionality for reading differential ADC measurements
 *
 * Copyright (c) 2025 NeurotechHub
 * SPDX-License-Identifier: Apache-2.0
 */

#include "adc.h"
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>
#include <zephyr/devicetree.h>
#include <string.h>
#include <hal/nrf_rtc.h>
#include <hal/nrf_saadc.h>

LOG_MODULE_REGISTER(juxta_adc, LOG_LEVEL_INF);

/* ADC device and configuration */
static const struct device *adc_dev;
static struct adc_channel_cfg adc_cfg;
static struct adc_sequence adc_seq;
static int16_t adc_sample_buffer;
static bool adc_initialized = false;

int juxta_adc_init(void)
{
    int ret;

    /* Get ADC device */
    adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc));
    if (!device_is_ready(adc_dev))
    {
        LOG_ERR("ADC device not ready");
        return -ENODEV;
    }

    LOG_INF("ADC device ready: %s", adc_dev->name);

    /* Configure ADC channel 0 for differential measurement (AIN0/AIN1) */
    memset(&adc_cfg, 0, sizeof(adc_cfg));
    adc_cfg.gain = ADC_GAIN_1_6;
    adc_cfg.reference = ADC_REF_INTERNAL;
    adc_cfg.acquisition_time = ADC_ACQ_TIME_DEFAULT;
    adc_cfg.channel_id = 0;                                     /* Use channel 0 for differential measurement */
    adc_cfg.input_positive = SAADC_CH_PSELP_PSELP_AnalogInput1; /* P0.03 */
    adc_cfg.input_negative = SAADC_CH_PSELN_PSELN_AnalogInput0; /* P0.02 */
    adc_cfg.differential = 1;

    ret = adc_channel_setup(adc_dev, &adc_cfg);
    if (ret != 0)
    {
        LOG_ERR("Failed to setup ADC channel: %d", ret);
        return ret;
    }

    /* Configure ADC sequence */
    memset(&adc_seq, 0, sizeof(adc_seq));
    adc_seq.channels = BIT(0);
    adc_seq.buffer = &adc_sample_buffer;
    adc_seq.buffer_size = sizeof(adc_sample_buffer);
    adc_seq.resolution = 12;
    adc_seq.oversampling = 1;
    adc_seq.calibrate = true;

    LOG_INF("ADC differential measurement configured:");
    LOG_INF("  Channel: %d", adc_cfg.channel_id);
    LOG_INF("  Input: AIN1 (P0.03) - AIN0 (P0.02)");
    LOG_INF("  Resolution: %d bits", adc_seq.resolution);
    LOG_INF("  Oversampling: %d", adc_seq.oversampling);
    LOG_INF("  Gain: 1/6");

    adc_initialized = true;
    return 0;
}

int juxta_adc_read_differential(int32_t *value_mv)
{
    int ret;

    if (!adc_initialized || !adc_dev)
    {
        LOG_ERR("ADC not initialized");
        return -ENODEV;
    }

    if (!value_mv)
    {
        LOG_ERR("Invalid parameter: value_mv is NULL");
        return -EINVAL;
    }

    /* Read ADC value */
    ret = adc_read(adc_dev, &adc_seq);
    if (ret != 0)
    {
        LOG_ERR("ADC read failed: %d", ret);
        return ret;
    }

    LOG_DBG("ADC raw value: %d", adc_sample_buffer);

    /* Convert to millivolts */
    *value_mv = adc_sample_buffer;
    ret = adc_raw_to_millivolts(adc_ref_internal(adc_dev),
                                adc_cfg.gain,
                                adc_seq.resolution,
                                value_mv);
    if (ret != 0)
    {
        LOG_ERR("ADC conversion failed: %d", ret);
        return ret;
    }

    LOG_DBG("ADC differential reading: %d mV (raw: %d)", *value_mv, adc_sample_buffer);
    return 0;
}

bool juxta_adc_is_ready(void)
{
    return adc_initialized && device_is_ready(adc_dev);
}

int juxta_adc_burst_sample(int32_t *samples, uint32_t max_samples,
                           uint32_t *actual_samples, uint32_t *duration_us)
{
    if (!adc_initialized || !adc_dev)
    {
        return -ENODEV;
    }

    if (!samples || !actual_samples || !duration_us)
    {
        return -EINVAL;
    }

    if (max_samples < 200)
    {
        return -EINVAL;
    }

    /* Start timing using RTC0 counter (32kHz clock) */
    uint32_t start_ticks = NRF_RTC0->COUNTER;
    uint32_t sample_count = 0;

    /* Perform burst sampling for approximately 2ms */
    while (sample_count < max_samples)
    {
        int32_t voltage_mv;
        int ret = juxta_adc_read_differential(&voltage_mv);

        if (ret == 0)
        {
            samples[sample_count] = voltage_mv;
            sample_count++;
        }
        else
        {
            /* Continue sampling even if one sample fails */
            continue;
        }

        /* Minimal delay - let ADC conversion time dominate */
        k_busy_wait(5); /* 5μs delay */
    }

    /* End timing */
    uint32_t end_ticks = NRF_RTC0->COUNTER;
    uint32_t duration_ticks = end_ticks - start_ticks;

    /* Calculate actual duration */
    *actual_samples = sample_count;
    *duration_us = (duration_ticks * 1000000) / 32768; /* Convert ticks to microseconds */

    return 0;
}