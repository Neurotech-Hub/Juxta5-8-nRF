# JUXTA FRAM File System Library
#
# Copyright (c) 2024 NeurotechHub
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_JUXTA_FRAMFS)

zephyr_library()

zephyr_library_sources(src/framfs.c)

zephyr_library_include_directories(include)

# Link with the juxta_fram library
zephyr_library_link_libraries(juxta_fram)

endif() # CONFIG_JUXTA_FRAMFS 