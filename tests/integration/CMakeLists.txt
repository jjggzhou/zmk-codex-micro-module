cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(codex_usb_integration)

target_sources(app PRIVATE
  src/usb_fakes.c
  src/usb_identity_test.c
)
