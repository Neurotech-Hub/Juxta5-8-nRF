/*
 * LIS2DH12 Zephyr Integration Layer
 * Wraps STMicroelectronics LIS2DH12 library for Zephyr SPI
 *
 * Copyright (c) 2024 NeurotechHub
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LIS2DH12_ZEPHYR_H_
#define LIS2DH12_ZEPHYR_H_

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief LIS2DH12 device context for Zephyr
     */
    struct lis2dh12_zephyr_dev
    {
        const struct device *spi_dev;
        struct spi_config spi_cfg;
        struct gpio_dt_spec int_gpio;
        bool initialized;
    };

/**
 * @brief LIS2DH12 WHO_AM_I register value
 */
#define LIS2DH12_WHO_AM_I_VAL 0x33

    /**
     * @brief Initialize LIS2DH12 device from device tree
     *
     * @param dev Pointer to LIS2DH12 device structure
     * @param spi_node Unused parameter (kept for compatibility)
     * @param int_spec GPIO specification for interrupt pin
     * @return 0 on success, negative error code on failure
     */
    int lis2dh12_zephyr_init(struct lis2dh12_zephyr_dev *dev,
                             const struct device *spi_node,
                             const struct gpio_dt_spec *int_spec);

    /**
     * @brief Verify WHO_AM_I register
     *
     * @param dev Pointer to initialized LIS2DH12 device
     * @return 0 on success, negative error code on failure
     */
    int lis2dh12_zephyr_verify_who_am_i(struct lis2dh12_zephyr_dev *dev);

    /**
     * @brief Read device ID using STMicroelectronics library
     *
     * @param dev Pointer to initialized LIS2DH12 device
     * @param id Pointer to store device ID
     * @return 0 on success, negative error code on failure
     */
    int lis2dh12_zephyr_read_device_id(struct lis2dh12_zephyr_dev *dev, uint8_t *id);

    /**
     * @brief Test basic accelerometer functionality
     *
     * @param dev Pointer to initialized LIS2DH12 device
     * @return 0 on success, negative error code on failure
     */
    int lis2dh12_zephyr_test(struct lis2dh12_zephyr_dev *dev);

#ifdef __cplusplus
}
#endif

#endif /* LIS2DH12_ZEPHYR_H_ */