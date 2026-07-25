cmake_minimum_required(VERSION 3.20.0)
set(KCONFIG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/Kconfig)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(codex_usb_integration)

get_filename_component(ZMK_SOURCE_DIR "${ZEPHYR_BASE}/../zmk" ABSOLUTE)
target_include_directories(app PRIVATE "${ZMK_SOURCE_DIR}/app/include")
target_include_directories(codex_micro PRIVATE "${ZMK_SOURCE_DIR}/app/include")

target_sources(app PRIVATE
  src/usb_fakes.c
  src/usb_identity_test.c
)
