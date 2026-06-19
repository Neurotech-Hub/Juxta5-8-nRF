/*
 * Custom LIS2DH12 Zephyr Wrapper
 *
 * Copyright (c) 2024 NeurotechHub
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LIS2DH12_H
#define LIS2DH12_H

#include <zephyr/kernel.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include "lis2dh12_reg.h"

/* LIS2DH12 device context */
struct lis2dh12_dev
{
    const struct device *spi_dev;
    struct spi_config spi_cfg;
    struct gpio_dt_spec cs_gpio;
    struct gpio_dt_spec int_gpio;

    bool initialized;
};

/* Function prototypes */
int lis2dh12_init(struct lis2dh12_dev *dev);
int lis2dh12_read_accel(struct lis2dh12_dev *dev, float *x, float *y, float *z);
int lis2dh12_read_temperature_lowres(struct lis2dh12_dev *dev, int8_t *temperature);
int lis2dh12_configure_motion_detection(struct lis2dh12_dev *dev,
                                        uint8_t threshold, uint8_t duration);
int lis2dh12_read_device_id(struct lis2dh12_dev *dev, uint8_t *id);
bool lis2dh12_is_ready(struct lis2dh12_dev *dev);
int lis2dh12_read_int1_source(struct lis2dh12_dev *dev, uint8_t *source);
int lis2dh12_clear_int1_interrupt(struct lis2dh12_dev *dev);
int lis2dh12_reset_motion_detection(struct lis2dh12_dev *dev);
int lis2dh12_test_interrupt_clearing(struct lis2dh12_dev *dev);
int lis2dh12_analyze_interrupt_trigger(struct lis2dh12_dev *dev);

/* Motion system management functions */
int lis2dh12_init_motion_system(void);
void lis2dh12_process_motion_events(void);
bool lis2dh12_should_use_extended_intervals(void);
uint8_t lis2dh12_get_motion_count(void);
void lis2dh12_reset_motion_count(void);

/* Platform functions for direct register access */
int32_t lis2dh12_platform_read(void *handle, uint8_t reg, uint8_t *data, uint16_t len);
int32_t lis2dh12_platform_write(void *handle, uint8_t reg, const uint8_t *data, uint16_t len);

#endif /* LIS2DH12_H */