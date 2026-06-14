# JUXTA FRAM Library
#
# Copyright (c) 2024 NeurotechHub
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_JUXTA_FRAM)

# Create the library target
zephyr_library()

# Add the source files
zephyr_library_sources(src/fram.c)

# Add include directories
zephyr_library_include_directories(include)

# The library depends on SPI and GPIO drivers
zephyr_library_link_libraries_ifdef(CONFIG_SPI spi)
zephyr_library_link_libraries_ifdef(CONFIG_GPIO gpio)

endif() # CONFIG_JUXTA_FRAM 