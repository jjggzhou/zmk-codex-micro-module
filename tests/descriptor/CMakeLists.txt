cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(codex_descriptor)

target_include_directories(app PRIVATE ../../include)
target_sources(app PRIVATE
  src/main.c
  src/ble_map_test.c
  src/ble_queue_test.c
  ../../src/descriptors/ble_report_map.c
  ../../src/transport/ble_queue.c
  ../../src/transport/ble_report_state.c
)
