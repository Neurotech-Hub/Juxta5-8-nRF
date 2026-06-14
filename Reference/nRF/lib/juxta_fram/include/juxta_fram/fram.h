/*
 * JUXTA FRAM Library
 *
 * Copyright (c) 2024 NeurotechHub
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef JUXTA_FRAM_H_
#define JUXTA_FRAM_H_

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief FRAM device structure
     */
    struct juxta_fram_device
    {
        const struct device *spi_dev;
        struct spi_config spi_cfg;
        struct gpio_dt_spec cs_gpio; /* Store GPIO spec for CS control */
        bool initialized;
    };

    /**
     * @brief FRAM device ID structure
     */
    struct juxta_fram_id
    {
        uint8_t manufacturer_id;   /* Expected: 0x04 (Fujitsu) */
        uint8_t continuation_code; /* Expected: 0x7F */
        uint8_t product_id_1;      /* Expected: 0x27 (1Mbit) */
        uint8_t product_id_2;      /* Expected: 0x03 */
    };

/**
 * @brief FRAM command codes
 */
#define JUXTA_FRAM_CMD_WREN 0x06  /* Write Enable */
#define JUXTA_FRAM_CMD_WRDI 0x04  /* Write Disable */
#define JUXTA_FRAM_CMD_RDSR 0x05  /* Read Status Register */
#define JUXTA_FRAM_CMD_WRSR 0x01  /* Write Status Register */
#define JUXTA_FRAM_CMD_READ 0x03  /* Read Data */
#define JUXTA_FRAM_CMD_WRITE 0x02 /* Write Data */
#define JUXTA_FRAM_CMD_RDID 0x9F  /* Read Device ID */

/**
 * @brief FRAM expected device ID values
 */
#define JUXTA_FRAM_MANUFACTURER_ID 0x04
#define JUXTA_FRAM_CONTINUATION_CODE 0x7F
#define JUXTA_FRAM_PRODUCT_ID_1 0x27
#define JUXTA_FRAM_PRODUCT_ID_2 0x03

/**
 * @brief FRAM memory specifications (MB85RS1MT)
 */
#define JUXTA_FRAM_SIZE_BYTES (128 * 1024) /* 128KB */
#define JUXTA_FRAM_ADDRESS_BITS 17         /* 17-bit addressing */
#define JUXTA_FRAM_MAX_FREQ_HZ 8000000     /* 8MHz max SPI frequency */

/**
 * @brief Error codes
 */
#define JUXTA_FRAM_OK 0
#define JUXTA_FRAM_ERROR -1
#define JUXTA_FRAM_ERROR_INIT -2
#define JUXTA_FRAM_ERROR_ID -3
#define JUXTA_FRAM_ERROR_ADDR -4
#define JUXTA_FRAM_ERROR_SPI -5
#define JUXTA_FRAM_ERROR_MODE -6

    /**
     * @brief Initialize FRAM device from device tree node
     *
     * @param fram_dev Pointer to FRAM device structure
     * @param fram_node Device tree node for FRAM (use DT_ALIAS(spi_fram))
     * @param cs_spec GPIO specification for chip select (use GPIO_DT_SPEC_GET)
     * @return 0 on success, negative error code on failure
     */
    /* Commented out due to missing device tree macros
    int juxta_fram_init_dt(struct juxta_fram_device *fram_dev,
                           const struct device *fram_node,
                           const struct gpio_dt_spec *cs_spec);
    */

    /**
     * @brief Initialize FRAM device with manual configuration
     *
     * @param fram_dev Pointer to FRAM device structure
     * @param spi_dev SPI device
     * @param frequency SPI frequency in Hz
     * @param cs_spec GPIO specification for chip select
     * @return 0 on success, negative error code on failure
     */
    int juxta_fram_init(struct juxta_fram_device *fram_dev,
                        const struct device *spi_dev,
                        uint32_t frequency,
                        const struct gpio_dt_spec *cs_spec);

    /**
     * @brief Read FRAM device ID and verify it matches expected values
     *
     * @param fram_dev Pointer to initialized FRAM device
     * @param id Pointer to store device ID (can be NULL)
     * @return 0 on success (ID matches), negative error code on failure
     */
    int juxta_fram_read_id(struct juxta_fram_device *fram_dev,
                           struct juxta_fram_id *id);

    /**
     * @brief Write data to FRAM
     *
     * @param fram_dev Pointer to initialized FRAM device
     * @param address 24-bit address (0 to JUXTA_FRAM_SIZE_BYTES-1)
     * @param data Pointer to data buffer
     * @param length Number of bytes to write
     * @return 0 on success, negative error code on failure
     */
    int juxta_fram_write(struct juxta_fram_device *fram_dev,
                         uint32_t address,
                         const uint8_t *data,
                         size_t length);

    /**
     * @brief Read data from FRAM
     *
     * @param fram_dev Pointer to initialized FRAM device
     * @param address 24-bit address (0 to JUXTA_FRAM_SIZE_BYTES-1)
     * @param data Pointer to data buffer
     * @param length Number of bytes to read
     * @return 0 on success, negative error code on failure
     */
    int juxta_fram_read(struct juxta_fram_device *fram_dev,
                        uint32_t address,
                        uint8_t *data,
                        size_t length);

    /**
     * @brief Write a single byte to FRAM
     *
     * @param fram_dev Pointer to initialized FRAM device
     * @param address 24-bit address
     * @param data Byte to write
     * @return 0 on success, negative error code on failure
     */
    static inline int juxta_fram_write_byte(struct juxta_fram_device *fram_dev,
                                            uint32_t address,
                                            uint8_t data)
    {
        return juxta_fram_write(fram_dev, address, &data, 1);
    }

    /**
     * @brief Read a single byte from FRAM
     *
     * @param fram_dev Pointer to initialized FRAM device
     * @param address 24-bit address
     * @param data Pointer to store read byte
     * @return 0 on success, negative error code on failure
     */
    static inline int juxta_fram_read_byte(struct juxta_fram_device *fram_dev,
                                           uint32_t address,
                                           uint8_t *data)
    {
        return juxta_fram_read(fram_dev, address, data, 1);
    }

    /**
     * @brief Test FRAM functionality with write/read verification
     *
     * @param fram_dev Pointer to initialized FRAM device
     * @param test_address Address to use for testing (will be overwritten)
     * @return 0 on success, negative error code on failure
     */
    int juxta_fram_test(struct juxta_fram_device *fram_dev, uint32_t test_address);

#ifdef __cplusplus
}
#endif

#endif /* JUXTA_FRAM_H_ */