#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_gpio.h>

LOG_MODULE_REGISTER(juxta5_8_blink, LOG_LEVEL_INF);

#if !DT_NODE_EXISTS(DT_PATH(zephyr_user)) || !DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#error "Board must define zephyr,user io-channels for FUEL (AIN6, P0.30)"
#endif

#define BLINK_PERIOD_MS 200U

/* README: VBATT = VFUEL * 7.82 (divider Rtop 1.5M, Rbottom 220k) */
#define FUEL_DIV_NUM 782L
#define FUEL_DIV_DEN 100L

/* Rough 1S LiPo percent from corrected battery voltage (linear 3.0 V … 4.2 V). */
#define VBATT_EMPTY_MV 3000
#define VBATT_FULL_MV 4200

#define FUEL_LOG_PERIOD_MS 2000U

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec magnet = GPIO_DT_SPEC_GET(DT_ALIAS(magnet_sensor), gpios);
static const struct device *const gpio1_dev = DEVICE_DT_GET(DT_NODELABEL(gpio1));
static const struct adc_dt_spec fuel = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));

static int magnet_read_fallback(void)
{
	uint32_t absolute_pin = magnet.pin;

	if (magnet.port == gpio1_dev) {
		absolute_pin += 32U;
	}

	return nrf_gpio_pin_read(absolute_pin) ? 1 : 0;
}

/** @return State of charge 0–100 from pack voltage in mV. */
static unsigned int vbatt_soc_pct(int32_t vbatt_mv)
{
	if (vbatt_mv <= VBATT_EMPTY_MV) {
		return 0U;
	}
	if (vbatt_mv >= VBATT_FULL_MV) {
		return 100U;
	}
	return (unsigned int)((vbatt_mv - VBATT_EMPTY_MV) * 100L /
			      (VBATT_FULL_MV - VBATT_EMPTY_MV));
}

/**
 * Sample FUEL pin via SAADC; converts to VFUEL mV at divider tap, then VBATT mV.
 *
 * @retval 0 success
 * @retval negative adc API error
 */
static int fuel_read_mv(int32_t *vfuel_mv, int32_t *vbatt_mv)
{
	uint16_t raw;
	struct adc_sequence seq = {
		.buffer = &raw,
		.buffer_size = sizeof(raw),
	};
	int err;
	int32_t val;

	err = adc_sequence_init_dt(&fuel, &seq);
	if (err != 0) {
		return err;
	}

	err = adc_read_dt(&fuel, &seq);
	if (err != 0) {
		return err;
	}

	if (fuel.channel_cfg.differential) {
		val = (int32_t)(int16_t)raw;
	} else {
		val = (int32_t)raw;
	}

	err = adc_raw_to_millivolts_dt(&fuel, &val);
	if (err != 0) {
		return err;
	}

	*vfuel_mv = val;
	*vbatt_mv = (val * FUEL_DIV_NUM) / FUEL_DIV_DEN;
	return 0;
}

int main(void)
{
	int ret;
	int magnet_state = 0;
	int previous_magnet_state = -1;
	bool led_blink_state = false;
	bool warned_fallback = false;
	bool fuel_ok = false;
	int64_t next_fuel_log_ms = 0;

	if (!gpio_is_ready_dt(&led) || !gpio_is_ready_dt(&magnet)) {
		LOG_ERR("GPIOs not ready (LED or magnet input)");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		LOG_ERR("Failed to configure LED0 (%d)", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&magnet, GPIO_INPUT);
	if (ret != 0) {
		LOG_ERR("Failed to configure MAG_INT (%d)", ret);
		return ret;
	}

	if (!adc_is_ready_dt(&fuel)) {
		LOG_WRN("FUEL ADC not ready; blink continues without fuel logs");
	} else {
		ret = adc_channel_setup_dt(&fuel);
		if (ret != 0) {
			LOG_WRN("FUEL adc_channel_setup failed (%d); blink continues", ret);
		} else {
			fuel_ok = true;
			next_fuel_log_ms = k_uptime_get();
		}
	}

	LOG_INF("Juxta5-8 blink bring-up started");
	LOG_INF("Behavior: 200 ms blink when MAG_INT low, solid ON when MAG_INT high");
	LOG_INF("LED0 mapped to P%d.%02d", (led.port == gpio1_dev) ? 1 : 0, led.pin);
	if (fuel_ok) {
		LOG_INF("FUEL: RTT every %u ms — vfuel/vbatt (mV), soc (%% linear %d–%d mV)",
			FUEL_LOG_PERIOD_MS, VBATT_EMPTY_MV, VBATT_FULL_MV);
	}

	while (1) {
		magnet_state = gpio_pin_get_dt(&magnet);
		if (magnet_state < 0) {
			if (!warned_fallback) {
				LOG_WRN("gpio_pin_get_dt(MAG_INT) failed (%d), using HAL fallback",
					magnet_state);
				warned_fallback = true;
			}
			magnet_state = magnet_read_fallback();
		}

		if (magnet_state != previous_magnet_state) {
			LOG_INF("MAG_INT is %s", magnet_state ? "HIGH (LED forced ON)" : "LOW (blink mode)");
			previous_magnet_state = magnet_state;
		}

		if (fuel_ok) {
			int64_t now = k_uptime_get();
			if (now >= next_fuel_log_ms) {
				int32_t vfuel_mv = 0;
				int32_t vbatt_mv = 0;

				next_fuel_log_ms = now + FUEL_LOG_PERIOD_MS;
				ret = fuel_read_mv(&vfuel_mv, &vbatt_mv);
				if (ret != 0) {
					LOG_WRN("FUEL sample failed (%d)", ret);
				} else {
					unsigned int soc = vbatt_soc_pct(vbatt_mv);

					LOG_INF("FUEL vfuel=%d mV vbatt=%d mV soc~=%u%%", vfuel_mv, vbatt_mv,
						soc);
				}
			}
		}

		if (magnet_state) {
			(void)gpio_pin_set_dt(&led, 1);
			k_sleep(K_MSEC(50));
		} else {
			led_blink_state = !led_blink_state;
			(void)gpio_pin_set_dt(&led, led_blink_state ? 1 : 0);
			k_sleep(K_MSEC(BLINK_PERIOD_MS));
		}
	}

	return 0;
}
