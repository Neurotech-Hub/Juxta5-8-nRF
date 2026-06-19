/*
 * P0.05 and P0.15 GPIO Toggle Test
 *
 * Minimal application to test P0.05 and P0.15 hardware connectivity.
 * This eliminates all potential software conflicts by using
 * only basic GPIO functionality.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(p0_05_test, LOG_LEVEL_INF);

/* Direct GPIO device reference for P0.05 (LED) and P0.15 (LIS2DH CS) */
#define GPIO_DEVICE_NODE DT_NODELABEL(gpio0)
#define P0_05_PIN 5  /* LED pin */
#define P0_15_PIN 15 /* LIS2DH CS pin */

int main(void)
{
    const struct device *gpio_dev;
    int ret;
    uint32_t toggle_count = 0;
    bool pin_state = false;

    /* Early boot message - if you see this, main() is being called */
    LOG_INF("🚀 MAIN FUNCTION ENTERED");

    /* Force immediate output */
    k_sleep(K_MSEC(100));
    LOG_INF("🔧 P0.05 (LED) and P0.15 (LIS2DH CS) GPIO Toggle Test Starting");
    LOG_INF("📋 Target: nRF52840 P0.05 (LED) and P0.15 (LIS2DH CS) pins");
    LOG_INF("🎯 Purpose: Hardware connectivity verification");
    LOG_INF("⏱️  Toggle interval: 500ms (2Hz)");
    LOG_INF("🔄 Mode: Continuous toggle until power off");

    /* Get GPIO device */
    gpio_dev = DEVICE_DT_GET(GPIO_DEVICE_NODE);
    if (!device_is_ready(gpio_dev))
    {
        LOG_ERR("❌ GPIO device not ready");
        return -1;
    }

    LOG_INF("✅ GPIO device ready: %s", gpio_dev->name);

    /* Configure P0.05 as GPIO output */
    ret = gpio_pin_configure(gpio_dev, P0_05_PIN, GPIO_OUTPUT | GPIO_ACTIVE_HIGH);
    if (ret != 0)
    {
        LOG_ERR("❌ Failed to configure P0.05 as output: %d", ret);
        return ret;
    }

    /* Configure P0.15 as GPIO output */
    ret = gpio_pin_configure(gpio_dev, P0_15_PIN, GPIO_OUTPUT | GPIO_ACTIVE_HIGH);
    if (ret != 0)
    {
        LOG_ERR("❌ Failed to configure P0.15 as output: %d", ret);
        return ret;
    }

    LOG_INF("✅ P0.05 (LED) and P0.15 (LIS2DH CS) configured as GPIO outputs");
    LOG_INF("🔧 Starting continuous toggle test...");
    LOG_INF("📊 Monitor P0.05 (LED) and P0.15 (LIS2DH CS) with oscilloscope/logic analyzer");
    LOG_INF("💡 Expected: 2Hz square wave (500ms period) on both pins");

    /* Continuous toggle loop */
    while (1)
    {
        /* Toggle both pins */
        ret = gpio_pin_set(gpio_dev, P0_05_PIN, pin_state);
        if (ret != 0)
        {
            LOG_ERR("❌ Failed to set P0.05 (LED): %d", ret);
        }

        ret = gpio_pin_set(gpio_dev, P0_15_PIN, pin_state);
        if (ret != 0)
        {
            LOG_ERR("❌ Failed to set P0.15 (LIS2DH CS): %d", ret);
        }

        if (ret == 0) /* Only increment if both pins were set successfully */
        {
            toggle_count++;
                        if (toggle_count % 10 == 0)
            { /* Log every 10 toggles */
                LOG_INF("🔄 Toggle #%u: P0.05 (LED) = %s, P0.15 (LIS2DH CS) = %s", toggle_count,
                         pin_state ? "HIGH" : "LOW", pin_state ? "HIGH" : "LOW");
            }
        }

        /* Toggle state for next iteration */
        pin_state = !pin_state;

        /* Wait 500ms */
        k_sleep(K_MSEC(500));
    }

    return 0;
}