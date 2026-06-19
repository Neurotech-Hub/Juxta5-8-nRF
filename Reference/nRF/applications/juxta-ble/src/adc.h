/*
 * JUXTA ADC Module Header
 * Provides interface for reading differential ADC measurements
 *
 * Copyright (c) 2025 NeurotechHub
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef JUXTA_ADC_H_
#define JUXTA_ADC_H_

#include <zephyr/kernel.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Initialize the ADC module for differential measurements
     *
     * This function sets up the ADC for reading differential measurements
     * on AIN0 (P0.02) and AIN1 (P0.03) using channel 0.
     *
     * @return 0 on success, negative error code on failure
     */
    int juxta_adc_init(void);

    /**
     * @brief Read a differential ADC measurement
     *
     * Reads the differential voltage between AIN0 (P0.02) and AIN1 (P0.03).
     * The measurement uses the configuration from the device tree:
     * - 12-bit resolution
     * - 8x oversampling
     * - 1/6 gain
     * - Internal reference
     *
     * @param value_mv Pointer to store the measurement result in millivolts
     * @return 0 on success, negative error code on failure
     */
    int juxta_adc_read_differential(int32_t *value_mv);

    /**
     * @brief Check if ADC module is initialized and ready
     *
     * @return true if ADC is ready, false otherwise
     */
    bool juxta_adc_is_ready(void);

    /**
     * @brief Perform a burst of ADC samples with precise timing
     *
     * Performs continuous ADC sampling for approximately 2ms with ~10μs intervals.
     * Uses RTC0 counter for precise timing measurement.
     *
     * @param samples Buffer to store ADC samples (must be at least 200 samples)
     * @param max_samples Maximum number of samples to store
     * @param actual_samples Pointer to store actual number of samples taken
     * @param duration_us Pointer to store actual duration in microseconds
     * @return 0 on success, negative error code on failure
     */
    int juxta_adc_burst_sample(int32_t *samples, uint32_t max_samples,
                               uint32_t *actual_samples, uint32_t *duration_us);

#ifdef __cplusplus
}
#endif

#endif /* JUXTA_ADC_H_ */