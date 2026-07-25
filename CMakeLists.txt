zephyr_library_named(codex_micro)
zephyr_include_directories(include)
target_include_directories(codex_micro PRIVATE ${APPLICATION_SOURCE_DIR}/include)
target_sources(codex_micro PRIVATE src/descriptors/usb_report_descriptor.c)

target_sources_ifdef(CONFIG_CODEX_MICRO codex_micro PRIVATE
  src/descriptors/usb_identity.c
  src/protocol/framing.c
  src/protocol/json_value.c
  src/protocol/rpc.c
  src/protocol/device_status.c
  src/transport/usb_transport.c
)

if(CONFIG_CODEX_MICRO)
  zephyr_ld_options(
    -Wl,--wrap=usb_get_device_descriptor
    -Wl,--wrap=usb_hid_register_device
    -Wl,--wrap=hid_int_ep_write
  )
endif()
