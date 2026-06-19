cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(juxta-axy LANGUAGES C)

# Add STMicroelectronics LIS2DH12 library include path
target_include_directories(app PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/lisd2h12
)

target_sources(app PRIVATE 
    src/main.c
    src/juxta_axy_example.c
    src/lis2dh12_zephyr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/lisd2h12/lis2dh12_reg.c
)

# Future FRAM integration: Add conditional compilation here
# target_sources_ifdef(CONFIG_JUXTA_FRAM app PRIVATE src/fram_integration.c) 