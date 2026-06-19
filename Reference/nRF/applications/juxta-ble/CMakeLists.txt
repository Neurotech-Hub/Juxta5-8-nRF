#-------------------------------------------------------------------------------
# JUXTA BLE Application
#
# Copyright (c) 2024 NeurotechHub
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(juxta_ble LANGUAGES C)

target_sources(app PRIVATE
    src/main.c
    src/ble_service.c
    src/lis2dh12.c
    src/adc.c
)

# Add include directories for our libraries
target_include_directories(app PRIVATE 
    ../../lib/juxta_vitals_nrf52/include
    ../../lib/juxta_framfs/include
    ../../lib/juxta_fram/include
    ../../lib/lisd2h12
)

# Add library source files directly
target_sources(app PRIVATE 
    ../../lib/juxta_vitals_nrf52/src/vitals.c
    ../../lib/juxta_framfs/src/framfs.c
    ../../lib/juxta_fram/src/fram.c
    ../../lib/lisd2h12/lis2dh12_reg.c
) 