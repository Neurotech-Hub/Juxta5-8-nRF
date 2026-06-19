/*
 * JUXTA-AXY Application - Accelerometer Playground
 * Based on juxta-mvp but focused on LIS2DH12 accelerometer functionality
 *
 * Copyright (c) 2024 NeurotechHub
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* External function from juxta_axy_example.c */
extern int juxta_axy_example_main(void);

int main(void)
{
    LOG_INF("Starting JUXTA-AXY Application v1.0");
    LOG_INF("Accelerometer playground with LIS2DH12 support");

    return juxta_axy_example_main();
}