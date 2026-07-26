cmake_minimum_required(VERSION 3.20.0)
set(KCONFIG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/Kconfig)
# The analog tests exercise the pinned badjeff driver, not a mock.  Twister
# passes the module under test through the cached ZEPHYR_EXTRA_MODULES value;
# replace that cache entry before Zephyr discovers modules so the real driver
# contributes its Kconfig, DTS bindings, and source.  The Codex module is
# added below with a distinct binary directory because its relative module
# name ("..") otherwise resolves to the application binary directory.
set(ZEPHYR_EXTRA_MODULES
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../zmk-analog-input-driver"
  CACHE STRING "Modules required by Codex input tests" FORCE
)
# The adapter is added manually below, so declare its DTS root now. The
# fixture also needs ZMK's vendor-prefix registry for zmk,analog-input; DTS
# generation occurs during find_package(Zephyr), before add_subdirectory.
list(APPEND DTS_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/../.."
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../zmk/app"
)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(codex_input)

if(NOT TARGET codex_micro)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_BINARY_DIR}/codex_micro_module)
endif()

execute_process(
  COMMAND west list zmk -f {abspath}
  OUTPUT_VARIABLE ZMK_SOURCE_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY
)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../..
  ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)
target_include_directories(codex_micro PRIVATE ${ZMK_SOURCE_DIR}/app/include)
target_link_libraries(app PRIVATE codex_micro)
target_sources(app PRIVATE
  src/key_test.c
  src/analog_test.c
)
