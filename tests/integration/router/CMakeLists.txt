cmake_minimum_required(VERSION 3.20.0)
set(KCONFIG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/Kconfig)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(codex_router_integration)

execute_process(
  COMMAND west list zmk -f {abspath}
  OUTPUT_VARIABLE ZMK_SOURCE_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY
)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../..
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/protocol
  ${ZMK_SOURCE_DIR}/app/include
)
target_sources(app PRIVATE
  src/router_fakes.c
  src/router_test.c
  ../../../src/transport/report_router.c
  ../../../src/protocol/framing.c
  ../../../src/protocol/json_value.c
  ../../../src/protocol/rpc.c
  ../../../src/protocol/device_status.c
  ../../../src/lighting/model.c
  ../../../src/lighting/effects.c
)
zephyr_ld_options(-Wl,--wrap=zmk_hid_mouse_clear)
