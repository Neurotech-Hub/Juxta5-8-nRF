/*
 * Simple main.c for Juxta5-1_ADC board testing
 * This version only includes the board-specific example without custom drivers
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <app_version.h>

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

// Forward declaration for board-specific example
extern int juxta5_example_main(void);

int main(void)
{
    printk("Running Juxta5-1_ADC Board Example %s\n", APP_VERSION_STRING);
    return juxta5_example_main();
} 