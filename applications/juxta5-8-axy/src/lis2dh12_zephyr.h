#ifndef JUXTA5_8_LIS2DH12_ZEPHYR_H
#define JUXTA5_8_LIS2DH12_ZEPHYR_H

#include <stdint.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>

struct lis2dh12_dev {
	const struct device *spi_dev;
	struct spi_config spi_cfg;
	struct spi_cs_control spi_cs;
	bool initialized;
};

/* Bundle of raw bytes useful for diagnosing why the temperature sensor
 * appears to be returning a fixed value.  Each field is the raw chip
 * register read; `temp_c` is the converted value using the same ST helper
 * as `lis2dh12_zephyr_read_temp_c`.  See lis2dh12_zephyr_read_temp_diag(). */
struct lis2dh12_temp_diag {
	int8_t temp_c;       /* same conversion as read_temp_c */
	int16_t lsb;         /* assembled (temp_h << 8) | temp_l */
	uint8_t temp_h;      /* OUT_TEMP_H  (0x0D) */
	uint8_t temp_l;      /* OUT_TEMP_L  (0x0C) */
	uint8_t status_aux;  /* STATUS_REG_AUX (0x07): bit2=TDA, bit6=TOR */
	uint8_t temp_cfg;    /* TEMP_CFG_REG   (0x1F): bits7:6 must be 11 to enable */
	uint8_t ctrl_reg1;   /* CTRL_REG1      (0x20): ODR[7:4], LPen[3], Zen Yen Xen */
	uint8_t ctrl_reg4;   /* CTRL_REG4      (0x23): BDU[7] must be 1 for safe L+H reads */
	uint8_t whoami;      /* WHO_AM_I       (0x0F): must be 0x33; SPI sanity check */
};

int lis2dh12_zephyr_init(struct lis2dh12_dev *dev);
int lis2dh12_zephyr_read_whoami(struct lis2dh12_dev *dev, uint8_t *whoami);
int lis2dh12_zephyr_read_accel_mg(struct lis2dh12_dev *dev, int16_t *x_mg, int16_t *y_mg, int16_t *z_mg);
int lis2dh12_zephyr_read_temp_c(struct lis2dh12_dev *dev, int8_t *temp_c);
/* Diagnostic read: fetches OUT_TEMP_L+OUT_TEMP_H in a single multi-byte SPI
 * transaction (datasheet-recommended pattern, avoids any BDU torn-read
 * concern) and also reads STATUS_REG_AUX, TEMP_CFG_REG, CTRL_REG1, CTRL_REG4,
 * and WHO_AM_I so the caller can decide whether the sensor is enabled, is
 * producing fresh data, and whether the SPI bus is healthy. */
int lis2dh12_zephyr_read_temp_diag(struct lis2dh12_dev *dev,
				   struct lis2dh12_temp_diag *out);
int lis2dh12_zephyr_config_motion(struct lis2dh12_dev *dev, uint8_t threshold, uint8_t duration);
int lis2dh12_zephyr_power_down(struct lis2dh12_dev *dev);
int lis2dh12_zephyr_read_int1_src(struct lis2dh12_dev *dev, uint8_t *int1_src);

#endif
