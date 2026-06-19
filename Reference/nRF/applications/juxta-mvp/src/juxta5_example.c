/*
 * Example code for Juxta5-1 board peripherals
 * This demonstrates how to use SPI FRAM, ADC, GPIO interrupt, and shared LED/CS
 * with low-power sleep until magnet sensor interrupt
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/pm.h>
#include <string.h>

LOG_MODULE_REGISTER(juxta5_example, LOG_LEVEL_DBG);

/* Device tree definitions */
#define MAGNET_SENSOR_NODE DT_ALIAS(magnet_sensor)
#define LED_NODE DT_ALIAS(led0)
#define FRAM_NODE DT_ALIAS(spi_fram)

/* GPIO specifications */
static const struct gpio_dt_spec magnet_sensor = GPIO_DT_SPEC_GET(MAGNET_SENSOR_NODE, gpios);
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED_NODE, gpios);

/* Callback data */
static struct gpio_callback magnet_cb_data;

/* Semaphore for signaling magnet sensor interrupt */
static K_SEM_DEFINE(magnet_sem, 0, 1);

/* Counter for magnet sensor events */
static uint32_t magnet_event_count = 0;

/* ADC configuration */
#define ADC_NODE DT_NODELABEL(adc)
#define ADC_CHANNEL_ID 4
#define ADC_CHANNEL_INPUT ADC_CHANNEL_ID

static const struct device *adc_dev;
static const struct adc_channel_cfg adc_channel_cfg = {
    .gain = ADC_GAIN_1_6,
    .reference = ADC_REF_INTERNAL,
    .acquisition_time = ADC_ACQ_TIME_DEFAULT,
    .channel_id = ADC_CHANNEL_ID,
    .differential = 1,
    .input_positive = 4, /* P0.04 - AIN4 */
    .input_negative = 5, /* P0.05 - AIN5 */
};

/**
 * @brief Magnet sensor interrupt callback
 */
void magnet_sensor_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    magnet_event_count++;

    LOG_INF("🧲 Magnet sensor interrupt triggered! (Event #%u)", magnet_event_count);

    /* Signal the main thread that an interrupt occurred */
    k_sem_give(&magnet_sem);

    /* Note: P0.20 is shared between LED and FRAM CS */
    /* When using SPI, LED will be controlled by SPI CS */
    /* When using LED, you need to manage this carefully */
}

/**
 * @brief Initialize magnet sensor interrupt
 */
static int init_magnet_sensor(void)
{
    int ret;

    if (!gpio_is_ready_dt(&magnet_sensor))
    {
        LOG_ERR("Magnet sensor GPIO not ready");
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&magnet_sensor, GPIO_INPUT);
    if (ret < 0)
    {
        LOG_ERR("Failed to configure magnet sensor pin: %d", ret);
        return ret;
    }

    ret = gpio_pin_interrupt_configure_dt(&magnet_sensor, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0)
    {
        LOG_ERR("Failed to configure magnet sensor interrupt: %d", ret);
        return ret;
    }

    gpio_init_callback(&magnet_cb_data, magnet_sensor_callback, BIT(magnet_sensor.pin));
    gpio_add_callback(magnet_sensor.port, &magnet_cb_data);

    LOG_INF("Magnet sensor initialized on pin %d (interrupt on rising edge)", magnet_sensor.pin);
    return 0;
}

/**
 * @brief Initialize LED (shared with FRAM CS)
 */
static int init_led(void)
{
    int ret;

    if (!gpio_is_ready_dt(&led))
    {
        LOG_ERR("LED GPIO not ready");
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    if (ret < 0)
    {
        LOG_ERR("Failed to configure LED pin: %d", ret);
        return ret;
    }

    LOG_INF("LED initialized on pin %d (shared with FRAM CS)", led.pin);
    return 0;
}

/**
 * @brief Initialize ADC for differential measurement
 */
static int init_adc(void)
{
    int ret;

    adc_dev = DEVICE_DT_GET(ADC_NODE);
    if (!device_is_ready(adc_dev))
    {
        LOG_ERR("ADC device not ready");
        return -ENODEV;
    }

    ret = adc_channel_setup(adc_dev, &adc_channel_cfg);
    if (ret < 0)
    {
        LOG_ERR("Failed to setup ADC channel: %d", ret);
        return ret;
    }

    LOG_INF("ADC initialized for differential measurement (P0.04/P0.05)");
    return 0;
}

/**
 * @brief Read differential ADC value
 */
static int read_adc(void)
{
    int ret;
    int16_t buf;

    const struct adc_sequence sequence = {
        .channels = BIT(ADC_CHANNEL_ID),
        .buffer = &buf,
        .buffer_size = sizeof(buf),
        .resolution = 12,
        .oversampling = 8,
        .calibrate = true,
    };

    ret = adc_read(adc_dev, &sequence);
    if (ret < 0)
    {
        LOG_ERR("ADC read failed: %d", ret);
        return ret;
    }

    /* Convert to millivolts */
    int32_t val_mv = buf;
    ret = adc_raw_to_millivolts(adc_ref_internal(adc_dev), ADC_GAIN_1_6, 12, &val_mv);
    if (ret < 0)
    {
        LOG_ERR("ADC conversion failed: %d", ret);
        return ret;
    }

    LOG_INF("ADC differential reading: %d mV (raw: %d)", val_mv, buf);
    return 0;
}

/**
 * @brief Test FRAM memory (when LED is not being used)
 */
static int test_fram(void)
{
    int ret;

    LOG_INF("Testing FRAM (MB85RS1MTPW-G-APEWE1)...");

    /* Get SPI device and configure */
    const struct device *spi_dev = DEVICE_DT_GET(DT_BUS(FRAM_NODE));
    if (!device_is_ready(spi_dev))
    {
        LOG_ERR("SPI bus not ready");
        return -ENODEV;
    }

    struct spi_config spi_cfg = {
        .frequency = 8000000, /* 8MHz - standard SPI clock */
        .operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
        .slave = DT_REG_ADDR(FRAM_NODE),
        .cs = {
            .gpio = {
                .port = led.port,
                .pin = led.pin,
                .dt_flags = led.dt_flags},
            .delay = 0}};

    LOG_INF("Testing FRAM with direct SPI communication...");

    /* Step 1: Write Enable */
    uint8_t tx_wren = 0x06;
    const struct spi_buf tx_buf_wren = {
        .buf = &tx_wren,
        .len = 1};
    const struct spi_buf_set tx_wren_set = {
        .buffers = &tx_buf_wren,
        .count = 1};

    ret = spi_write(spi_dev, &spi_cfg, &tx_wren_set);
    if (ret < 0)
    {
        LOG_ERR("Failed to send WREN command: %d", ret);
        return ret;
    }

    k_usleep(30); /* Consistent delay between transactions */

    /* Step 2: Write Data - store the event count */
    uint8_t tx_write[] = {
        0x02,                                /* Write command */
        0x00, 0x10, 0x00,                    /* 24-bit address (0x001000) */
        (uint8_t)(magnet_event_count & 0xFF) /* Store event count as test data */
    };
    const struct spi_buf tx_buf_write = {
        .buf = tx_write,
        .len = sizeof(tx_write)};
    const struct spi_buf_set tx_write_set = {
        .buffers = &tx_buf_write,
        .count = 1};

    ret = spi_write(spi_dev, &spi_cfg, &tx_write_set);
    if (ret < 0)
    {
        LOG_ERR("Failed to write test byte: %d", ret);
        return ret;
    }

    k_usleep(30); /* Consistent delay between transactions */

    /* Step 3: Read Data */
    uint8_t tx_read[] = {
        0x03,             /* Read command */
        0x00, 0x10, 0x00, /* 24-bit address */
        0x00              /* Dummy byte to receive data */
    };
    uint8_t rx_read[sizeof(tx_read)] = {0};

    const struct spi_buf tx_buf_read = {
        .buf = tx_read,
        .len = sizeof(tx_read)};
    const struct spi_buf rx_buf_read = {
        .buf = rx_read,
        .len = sizeof(rx_read)};
    const struct spi_buf_set tx_read_set = {
        .buffers = &tx_buf_read,
        .count = 1};
    const struct spi_buf_set rx_read_set = {
        .buffers = &rx_buf_read,
        .count = 1};

    ret = spi_transceive(spi_dev, &spi_cfg, &tx_read_set, &rx_read_set);
    if (ret < 0)
    {
        LOG_ERR("Failed to read test byte: %d", ret);
        return ret;
    }

    LOG_INF("Direct SPI test - wrote event count %u, read back 0x%02X",
            magnet_event_count, rx_read[4]);

    k_usleep(30); /* Consistent delay before RDID */

    /* Step 4: Device ID Read (moved to end since it has different length) */
    uint8_t tx_rdid[] = {
        0x9F,                  /* RDID command */
        0x00, 0x00, 0x00, 0x00 /* 32 cycles needed for complete ID */
    };
    uint8_t rx_rdid[5] = {0}; /* 1 byte command + 4 bytes response */

    const struct spi_buf tx_buf = {
        .buf = tx_rdid,
        .len = sizeof(tx_rdid)};
    const struct spi_buf rx_buf = {
        .buf = rx_rdid,
        .len = sizeof(rx_rdid)};
    const struct spi_buf_set tx = {
        .buffers = &tx_buf,
        .count = 1};
    const struct spi_buf_set rx = {
        .buffers = &rx_buf,
        .count = 1};

    ret = spi_transceive(spi_dev, &spi_cfg, &tx, &rx);
    if (ret < 0)
    {
        LOG_ERR("Failed to read device ID: %d", ret);
        return ret;
    }

    LOG_INF("Device ID read:");
    LOG_INF("  Manufacturer ID: 0x%02X (expected: 0x04 Fujitsu)", rx_rdid[1]);
    LOG_INF("  Continuation: 0x%02X (expected: 0x7F)", rx_rdid[2]);
    LOG_INF("  Product ID 1: 0x%02X (expected: 0x27 - 1Mbit)", rx_rdid[3]);
    LOG_INF("  Product ID 2: 0x%02X (expected: 0x03)", rx_rdid[4]);

    return 0;
}

/**
 * @brief Handle wake-up activities after magnet sensor interrupt
 */
static void handle_magnet_event(void)
{
    LOG_INF("🔋 Device woke up from sleep due to magnet sensor!");

    /* Read current ADC value */
    LOG_INF("📊 Reading sensors after wake-up...");
    read_adc();

    /* Store event in FRAM for persistence */
    LOG_INF("💾 Storing event data to FRAM...");
    test_fram();

    /* Optional: Flash LED briefly to indicate wake-up */
    /* Note: Commented out because LED shares pin with FRAM CS */
    /*
    gpio_pin_set_dt(&led, 1);
    k_msleep(100);
    gpio_pin_set_dt(&led, 0);
    */

    LOG_INF("✅ Event processing complete. Returning to sleep...");
}

/**
 * @brief Main application entry point
 */
int juxta5_example_main(void)
{
    int ret;

    LOG_INF("Starting Juxta5-1 Low-Power Magnet Sensor Example");

    /* Initialize peripherals */
    ret = init_magnet_sensor();
    if (ret < 0)
    {
        return ret;
    }

    ret = init_led();
    if (ret < 0)
    {
        return ret;
    }

    ret = init_adc();
    if (ret < 0)
    {
        return ret;
    }

    LOG_INF("All peripherals initialized successfully");
    LOG_INF("🔋 Entering low-power mode - device will sleep until magnet sensor interrupt");
    LOG_INF("🧲 Trigger the magnet sensor (P0.%02d) to wake the device", magnet_sensor.pin);

    /* Main loop - sleep until interrupt */
    while (1)
    {
        /* Wait for magnet sensor interrupt (blocks until interrupt occurs) */
        ret = k_sem_take(&magnet_sem, K_FOREVER);
        if (ret == 0)
        {
            /* Process the magnet sensor event */
            handle_magnet_event();
        }

        /* Brief delay before going back to sleep */
        k_msleep(100);

        LOG_INF("💤 Going back to sleep... (Event count: %u)", magnet_event_count);
    }

    return 0;
}