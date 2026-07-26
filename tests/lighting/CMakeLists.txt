cmake_minimum_required(VERSION 3.20.0)
set(KCONFIG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/Kconfig)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(codex_lighting_tests)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/protocol
)

target_sources(app PRIVATE
  src/model_test.c
  ../../src/protocol/json_value.c
  ../../src/lighting/model.c
  ../../src/lighting/effects.c
)
