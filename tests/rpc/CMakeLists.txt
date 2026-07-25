cmake_minimum_required(VERSION 3.20.0)
set(KCONFIG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/Kconfig)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(codex_rpc_tests)

get_filename_component(ZMK_SOURCE_DIR "${ZEPHYR_BASE}/../zmk" ABSOLUTE)
target_include_directories(app PRIVATE
  "${ZMK_SOURCE_DIR}/app/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/protocol"
)

target_sources(app PRIVATE
  src/main.c
  src/zmk_fakes.c
  ../../src/protocol/json_value.c
  ../../src/protocol/rpc.c
  ../../src/protocol/device_status.c
)
