cmake_minimum_required(VERSION 3.20.0)
set(KCONFIG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/Kconfig)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(codex_input)

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
target_sources(app PRIVATE src/key_test.c)
