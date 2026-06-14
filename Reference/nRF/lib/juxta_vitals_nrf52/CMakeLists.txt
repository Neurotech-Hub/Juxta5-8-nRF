# JUXTA Vitals Library for nRF52
#
# Copyright (c) 2024 NeurotechHub
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_JUXTA_VITALS_NRF52)

# Create the library target
zephyr_library()

# Add the source files
zephyr_library_sources(src/vitals.c)

# Add include directories
zephyr_library_include_directories(include)

# The library depends on ADC driver
zephyr_library_link_libraries_ifdef(CONFIG_ADC adc)

endif() # CONFIG_JUXTA_VITALS_NRF52 