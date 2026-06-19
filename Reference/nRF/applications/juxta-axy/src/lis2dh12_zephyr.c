/*
 * LIS2DH12 Zephyr Integration Layer Implementation
 * Wraps STMicroelectronics LIS2DH12 library for Zephyr SPI
 *
 * Copyright (c) 2024 NeurotechHub
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lis2dh12_zephyr.h"
#include "lis2dh12_reg.h"
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(lis2dh12_zephyr, LOG_LEVEL_DBG);

/* Device tree nodes */
#define SPI_NODE DT_ALIAS(spi_accel)
#define SPI_BUS DT_BUS(SPI_NODE)

/* Global context for platform functions */
static struct lis2dh12_zephyr_dev *g_lis2dh12_dev = NULL;

/**
 * @brief Platform-specific SPI read function for STMicroelectronics library
 *
 * This function implements the required interface for the library's function pointers.
 */
int32_t lis2dh12_platform_read(void *handle, uint8_t reg, uint8_t *data, uint16_t len)
{
    if (!g_lis2dh12_dev || !g_lis2dh12_dev->initialized)
    {
        LOG_ERR("LIS2DH12 device not initialized");
        return -ENODEV;
    }

    /* For SPI, set read bit (bit 7) and increment bit (bit 6) for multi-byte reads */
    uint8_t tx_reg = reg | 0x80; /* Set read bit */
    if (len > 1)
    {
        tx_reg |= 0x40; /* Set increment bit for multi-byte reads */
    }

    uint8_t tx_buf[1 + len]; /* Register + dummy bytes */
    uint8_t rx_buf[1 + len]; /* Response */

    tx_buf[0] = tx_reg;
    memset(&tx_buf[1], 0x00, len); /* Dummy bytes */

    const struct spi_buf tx_bufs = {
        .buf = tx_buf,
        .len = sizeof(tx_buf)};
    const struct spi_buf rx_bufs = {
        .buf = rx_buf,
        .len = sizeof(rx_buf)};
    const struct spi_buf_set tx = {
        .buffers = &tx_bufs,
        .count = 1};
    const struct spi_buf_set rx = {
        .buffers = &rx_bufs,
        .count = 1};

    int ret = spi_transceive(g_lis2dh12_dev->spi_dev, &g_lis2dh12_dev->spi_cfg, &tx, &rx);
    if (ret < 0)
    {
        LOG_ERR("SPI read failed: %d", ret);
        return ret;
    }

    /* Copy received data (skip first byte which is the register echo) */
    memcpy(data, &rx_buf[1], len);

    LOG_DBG("Read reg 0x%02X: %02X", reg, data[0]);
    return 0;
}

/**
 * @brief Platform-specific SPI write function for STMicroelectronics library
 *
 * This function implements the required interface for the library's function pointers.
 */
int32_t lis2dh12_platform_write(void *handle, uint8_t reg, const uint8_t *data, uint16_t len)
{
    if (!g_lis2dh12_dev || !g_lis2dh12_dev->initialized)
    {
        LOG_ERR("LIS2DH12 device not initialized");
        return -ENODEV;
    }

    /* For SPI, clear read bit (bit 7) and set increment bit (bit 6) for multi-byte writes */
    uint8_t tx_reg = reg & 0x7F; /* Clear read bit */
    if (len > 1)
    {
        tx_reg |= 0x40; /* Set increment bit for multi-byte writes */
    }

    uint8_t tx_buf[1 + len]; /* Register + data */
    tx_buf[0] = tx_reg;
    memcpy(&tx_buf[1], data, len);

    const struct spi_buf tx_bufs = {
        .buf = tx_buf,
        .len = sizeof(tx_buf)};
    const struct spi_buf_set tx = {
        .buffers = &tx_bufs,
        .count = 1};

    int ret = spi_write(g_lis2dh12_dev->spi_dev, &g_lis2dh12_dev->spi_cfg, &tx);
    if (ret < 0)
    {
        LOG_ERR("SPI write failed: %d", ret);
        return ret;
    }

    LOG_DBG("Write reg 0x%02X: %02X", reg, data[0]);
    return 0;
}

int lis2dh12_zephyr_init(struct lis2dh12_zephyr_dev *dev,
                         const struct device *spi_node,
                         const struct gpio_dt_spec *int_spec)
{
    if (!dev || !int_spec)
    {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    /* Get the SPI bus device using device tree */
    const struct device *spi_dev = DEVICE_DT_GET(SPI_BUS);
    if (!device_is_ready(spi_dev))
    {
        LOG_ERR("SPI device not ready");
        return -ENODEV;
    }

    if (!gpio_is_ready_dt(int_spec))
    {
        LOG_ERR("Interrupt GPIO not ready");
        return -ENODEV;
    }

    /* Configure SPI for LIS2DH12 */
    dev->spi_dev = spi_dev;
    dev->spi_cfg.frequency = 8000000; /* 8MHz max for LIS2DH12 */
    dev->spi_cfg.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_MODE_CPOL | SPI_MODE_CPHA;
    dev->spi_cfg.slave = 1; /* Use slave 1 (accel@1 in device tree) */
    dev->spi_cfg.cs.delay = 0;

    /* Store interrupt GPIO spec */
    dev->int_gpio = *int_spec;

    /* Set global context for platform functions */
    g_lis2dh12_dev = dev;
    dev->initialized = true;

    LOG_INF("LIS2DH12 initialized: freq=%d Hz, slave=%d, INT=P0.%02d",
            dev->spi_cfg.frequency, dev->spi_cfg.slave, int_spec->pin);

    return 0;
}

int lis2dh12_zephyr_verify_who_am_i(struct lis2dh12_zephyr_dev *dev)
{
    if (!dev || !dev->initialized)
    {
        LOG_ERR("Device not initialized");
        return -ENODEV;
    }

    uint8_t who_am_i = 0;

    /* Create STMicroelectronics library context */
    stmdev_ctx_t ctx = {
        .write_reg = lis2dh12_platform_write,
        .read_reg = lis2dh12_platform_read,
        .handle = dev /* Not used by our implementation */
    };

    /* Use STMicroelectronics library function */
    int32_t ret = lis2dh12_device_id_get(&ctx, &who_am_i);
    if (ret != 0)
    {
        LOG_ERR("Failed to read WHO_AM_I register: %d", ret);
        return -EIO;
    }

    if (who_am_i != LIS2DH12_WHO_AM_I_VAL)
    {
        LOG_ERR("Invalid WHO_AM_I: 0x%02X (expected 0x%02X)",
                who_am_i, LIS2DH12_WHO_AM_I_VAL);
        return -ENODEV;
    }

    LOG_INF("✅ LIS2DH12 WHO_AM_I verified: 0x%02X", who_am_i);
    return 0;
}

int lis2dh12_zephyr_read_device_id(struct lis2dh12_zephyr_dev *dev, uint8_t *id)
{
    if (!dev || !dev->initialized || !id)
    {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    /* Create STMicroelectronics library context */
    stmdev_ctx_t ctx = {
        .write_reg = lis2dh12_platform_write,
        .read_reg = lis2dh12_platform_read,
        .handle = dev};

    /* Use STMicroelectronics library function */
    int32_t ret = lis2dh12_device_id_get(&ctx, id);
    if (ret != 0)
    {
        LOG_ERR("Failed to read device ID: %d", ret);
        return -EIO;
    }

    LOG_DBG("Device ID read: 0x%02X", *id);
    return 0;
}

int lis2dh12_zephyr_test(struct lis2dh12_zephyr_dev *dev)
{
    if (!dev || !dev->initialized)
    {
        LOG_ERR("Device not initialized");
        return -ENODEV;
    }

    LOG_INF("🧪 Starting LIS2DH12 accelerometer test...");

    /* Test WHO_AM_I register */
    int ret = lis2dh12_zephyr_verify_who_am_i(dev);
    if (ret < 0)
    {
        LOG_ERR("❌ WHO_AM_I test failed: %d", ret);
        return ret;
    }

    /* TODO: Add more tests here as we expand functionality */
    /* Future: Test basic configuration, read acceleration data, etc. */

    LOG_INF("✅ LIS2DH12 accelerometer test passed!");
    return 0;
}