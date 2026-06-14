#include "lis2dh12_zephyr.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "lis2dh12_reg.h"

LOG_MODULE_REGISTER(lis2dh12_zephyr, LOG_LEVEL_INF);

#define LIS2DH12_NODE DT_ALIAS(spi_accel)
#define LIS2DH12_SPI_FREQ_HZ 8000000U

static struct lis2dh12_dev *g_dev;

static int32_t platform_read(void *handle, uint8_t reg, uint8_t *data, uint16_t len)
{
	ARG_UNUSED(handle);

	if (!g_dev || !g_dev->initialized) {
		return -ENODEV;
	}

	uint8_t tx_reg = reg | 0x80U;
	if (len > 1) {
		tx_reg |= 0x40U;
	}

	uint8_t tx_data[1 + 16] = {0};
	uint8_t rx_data[1 + 16] = {0};

	if (len > 16) {
		return -EINVAL;
	}

	tx_data[0] = tx_reg;

	const struct spi_buf tx_buf = {.buf = tx_data, .len = (size_t)(1 + len)};
	const struct spi_buf rx_buf = {.buf = rx_data, .len = (size_t)(1 + len)};
	const struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};
	const struct spi_buf_set rx = {.buffers = &rx_buf, .count = 1};

	int ret = spi_transceive(g_dev->spi_dev, &g_dev->spi_cfg, &tx, &rx);
	if (ret < 0) {
		LOG_ERR("SPI read failed reg=0x%02x ret=%d", reg, ret);
		return ret;
	}

	memcpy(data, &rx_data[1], len);
	return 0;
}

static int32_t platform_write(void *handle, uint8_t reg, const uint8_t *data, uint16_t len)
{
	ARG_UNUSED(handle);

	if (!g_dev || !g_dev->initialized) {
		return -ENODEV;
	}

	uint8_t tx_reg = reg & 0x7FU;
	if (len > 1) {
		tx_reg |= 0x40U;
	}

	uint8_t tx_data[1 + 16] = {0};
	if (len > 16) {
		return -EINVAL;
	}

	tx_data[0] = tx_reg;
	memcpy(&tx_data[1], data, len);

	const struct spi_buf tx_buf = {.buf = tx_data, .len = (size_t)(1 + len)};
	const struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};

	int ret = spi_write(g_dev->spi_dev, &g_dev->spi_cfg, &tx);
	if (ret < 0) {
		LOG_ERR("SPI write failed reg=0x%02x ret=%d", reg, ret);
		return ret;
	}

	return 0;
}

int lis2dh12_zephyr_read_whoami(struct lis2dh12_dev *dev, uint8_t *whoami)
{
	if (!dev || !dev->initialized || !whoami) {
		return -EINVAL;
	}

	return platform_read(NULL, LIS2DH12_WHO_AM_I, whoami, 1);
}

int lis2dh12_zephyr_init(struct lis2dh12_dev *dev)
{
	if (!dev) {
		return -EINVAL;
	}

	dev->spi_dev = DEVICE_DT_GET(DT_BUS(LIS2DH12_NODE));
	if (!device_is_ready(dev->spi_dev)) {
		LOG_ERR("LIS2DH12 SPI not ready");
		return -ENODEV;
	}

	dev->spi_cs.gpio = (struct gpio_dt_spec)SPI_CS_GPIOS_DT_SPEC_GET(LIS2DH12_NODE);
	dev->spi_cs.delay = 0U;

	if (!gpio_is_ready_dt(&dev->spi_cs.gpio)) {
		LOG_ERR("LIS2DH12 CS GPIO not ready");
		return -ENODEV;
	}

	dev->spi_cfg.frequency = LIS2DH12_SPI_FREQ_HZ;
	dev->spi_cfg.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB; /* Mode 0 */
	dev->spi_cfg.slave = DT_REG_ADDR(LIS2DH12_NODE);
	dev->spi_cfg.cs = dev->spi_cs;

	dev->initialized = true;
	g_dev = dev;

	uint8_t whoami = 0;
	int ret = lis2dh12_zephyr_read_whoami(dev, &whoami);
	if (ret < 0) {
		return ret;
	}
	if (whoami != 0x33) {
		LOG_ERR("Unexpected WHO_AM_I=0x%02x", whoami);
		return -ENODEV;
	}

	uint8_t ctrl_reg1 = 0x2F; /* 10Hz, LP mode, XYZ on */
	uint8_t ctrl_reg4 = 0x80; /* BDU on, +-2g */
	uint8_t temp_cfg = 0xC0;  /* temperature enabled */

	ret = platform_write(NULL, 0x20, &ctrl_reg1, 1);
	ret |= platform_write(NULL, 0x23, &ctrl_reg4, 1);
	ret |= platform_write(NULL, 0x1F, &temp_cfg, 1);
	if (ret < 0) {
		LOG_ERR("Failed LIS2DH12 base config");
		return ret;
	}

	k_sleep(K_MSEC(50));
	LOG_INF("LIS2DH12 ready WHO_AM_I=0x%02x SPI=Mode0", whoami);
	return 0;
}

int lis2dh12_zephyr_read_accel_mg(struct lis2dh12_dev *dev, int16_t *x_mg, int16_t *y_mg, int16_t *z_mg)
{
	if (!dev || !dev->initialized || !x_mg || !y_mg || !z_mg) {
		return -EINVAL;
	}

	uint8_t raw[6] = {0};
	int ret = platform_read(NULL, 0x28, raw, 6);
	if (ret < 0) {
		return ret;
	}

	*x_mg = (int16_t)((raw[1] << 8) | raw[0]);
	*y_mg = (int16_t)((raw[3] << 8) | raw[2]);
	*z_mg = (int16_t)((raw[5] << 8) | raw[4]);
	return 0;
}

int lis2dh12_zephyr_read_temp_c(struct lis2dh12_dev *dev, int8_t *temp_c)
{
	if (!dev || !dev->initialized || !temp_c) {
		return -EINVAL;
	}

	/* Single multi-byte SPI transaction across OUT_TEMP_L (0x0C) and
	 * OUT_TEMP_H (0x0D) inside one CS-low window.  platform_read sets
	 * the MS auto-increment bit when len>1, so the chip returns both
	 * bytes from the same sample regardless of BDU state — this is the
	 * datasheet-recommended burst-read pattern and avoids any chance of
	 * mixing L from sample N with H from sample N+1. */
	uint8_t temp_lh[2] = {0};
	int ret = platform_read(NULL, 0x0C, temp_lh, 2);
	if (ret < 0) {
		return ret;
	}

	int16_t lsb = (int16_t)(((uint16_t)temp_lh[1] << 8) | temp_lh[0]);
	*temp_c = (int8_t)lis2dh12_from_lsb_lp_to_celsius(lsb);
	return 0;
}

int lis2dh12_zephyr_read_temp_diag(struct lis2dh12_dev *dev,
				   struct lis2dh12_temp_diag *out)
{
	if (!dev || !dev->initialized || !out) {
		return -EINVAL;
	}

	memset(out, 0, sizeof(*out));

	/* Single multi-byte SPI transaction across OUT_TEMP_L (0x0C) and
	 * OUT_TEMP_H (0x0D).  platform_read sets the MS (auto-increment) bit
	 * when len>1, so this is one CS-low window covering both bytes —
	 * datasheet-recommended pattern, also bypasses any BDU torn-read
	 * concern. */
	uint8_t temp_lh[2] = {0};
	int ret = platform_read(NULL, 0x0C, temp_lh, 2);
	if (ret < 0) {
		return ret;
	}
	out->temp_l = temp_lh[0];
	out->temp_h = temp_lh[1];
	out->lsb = (int16_t)(((uint16_t)out->temp_h << 8) | out->temp_l);
	out->temp_c = (int8_t)lis2dh12_from_lsb_lp_to_celsius(out->lsb);

	/* Aux/config register readbacks.  Single-byte reads — order chosen to
	 * surface the most diagnostic information first if any read fails. */
	ret |= platform_read(NULL, 0x07, &out->status_aux, 1);  /* STATUS_REG_AUX */
	ret |= platform_read(NULL, 0x1F, &out->temp_cfg, 1);    /* TEMP_CFG_REG */
	ret |= platform_read(NULL, 0x20, &out->ctrl_reg1, 1);   /* CTRL_REG1 */
	ret |= platform_read(NULL, 0x23, &out->ctrl_reg4, 1);   /* CTRL_REG4 */
	ret |= platform_read(NULL, 0x0F, &out->whoami, 1);      /* WHO_AM_I */

	return (ret < 0) ? ret : 0;
}

int lis2dh12_zephyr_config_motion(struct lis2dh12_dev *dev, uint8_t threshold, uint8_t duration)
{
	if (!dev || !dev->initialized) {
		return -EINVAL;
	}

	uint8_t ctrl1 = 0x57;   /* 100Hz XYZ on */
	uint8_t ctrl2 = 0x09;   /* HP filter on INT1 */
	uint8_t ctrl3 = 0x40;   /* route activity interrupt to INT1 */
	uint8_t ctrl4 = 0x80;   /* BDU on, +-2g (BDU required for aux temp ADC updates) */
	uint8_t ctrl5 = 0x00;   /* non-latched INT1 */
	uint8_t int1_cfg = 0x2A;/* XH/YH/ZH OR */
	uint8_t reference = 0;
	uint8_t int1_src = 0;

	int ret = 0;
	ret |= platform_write(NULL, 0x20, &ctrl1, 1);
	ret |= platform_write(NULL, 0x21, &ctrl2, 1);
	ret |= platform_write(NULL, 0x22, &ctrl3, 1);
	ret |= platform_write(NULL, 0x23, &ctrl4, 1);
	ret |= platform_write(NULL, 0x24, &ctrl5, 1);
	ret |= platform_write(NULL, 0x32, &threshold, 1);
	ret |= platform_write(NULL, 0x33, &duration, 1);
	ret |= platform_read(NULL, 0x26, &reference, 1);
	ret |= platform_write(NULL, 0x30, &int1_cfg, 1);
	ret |= platform_read(NULL, 0x31, &int1_src, 1);
	if (ret < 0) {
		LOG_ERR("Failed motion setup");
		return ret;
	}

	/* Re-arm the aux temperature ADC against the accel mode/ODR we just
	 * programmed above.  The aux ADC's scheduling is tied to CTRL_REG1's
	 * operating mode + ODR; if temp was enabled by lis2dh12_zephyr_init()
	 * while the chip was still in its initial config, the OUT_TEMP_L/H
	 * registers can latch a single sample and never refresh after this
	 * mode change.  Disable-then-enable forces a clean restart of the
	 * temperature conversion path. */
	uint8_t temp_cfg_off = 0x00;
	uint8_t temp_cfg_on = 0xC0;
	ret |= platform_write(NULL, 0x1F, &temp_cfg_off, 1);
	ret |= platform_write(NULL, 0x1F, &temp_cfg_on, 1);
	if (ret < 0) {
		LOG_ERR("Failed temp sensor re-arm");
		return ret;
	}

	LOG_INF("LIS2DH12 motion configured threshold=%u duration=%u", threshold, duration);
	return 0;
}

int lis2dh12_zephyr_power_down(struct lis2dh12_dev *dev)
{
	if (!dev || !dev->initialized) {
		return -EINVAL;
	}

	uint8_t ctrl1 = 0;
	int ret = platform_read(NULL, 0x20, &ctrl1, 1);
	if (ret < 0) {
		return ret;
	}

	ctrl1 &= 0x0FU; /* ODR[3:0] = 0000: power-down mode. */
	ret = platform_write(NULL, 0x20, &ctrl1, 1);
	if (ret < 0) {
		LOG_ERR("Failed LIS2DH12 power-down");
		return ret;
	}

	LOG_INF("LIS2DH12 powered down");
	return 0;
}

int lis2dh12_zephyr_read_int1_src(struct lis2dh12_dev *dev, uint8_t *int1_src)
{
	if (!dev || !dev->initialized || !int1_src) {
		return -EINVAL;
	}

	return platform_read(NULL, 0x31, int1_src, 1);
}
