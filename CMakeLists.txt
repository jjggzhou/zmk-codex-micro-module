zephyr_library_named(codex_micro)
zephyr_include_directories(include)
target_include_directories(codex_micro PRIVATE ${APPLICATION_SOURCE_DIR}/include)
target_sources(codex_micro PRIVATE src/descriptors/usb_report_descriptor.c)
target_sources(codex_micro PRIVATE src/descriptors/ble_report_map.c)
target_sources(codex_micro PRIVATE src/transport/ble_report_state.c)

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

if(CONFIG_CODEX_MICRO AND CONFIG_ZMK_BLE)
  set(CODEX_PINNED_ZMK_COMMIT "edf5c0814fd3ea202e43aad2d68fd32e882a518c")
  set(CODEX_PINNED_HOG_SHA256 "2e8c12bb4197173d238a6be1e33abf2db5dedbd4a6cf29a83d5e2f6c79d4919b")
  set(CODEX_ZMK_HOG_SOURCE "${APPLICATION_SOURCE_DIR}/src/hog.c")

  execute_process(
    COMMAND git -C "${APPLICATION_SOURCE_DIR}/.." rev-parse HEAD
    OUTPUT_VARIABLE CODEX_ACTUAL_ZMK_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE CODEX_ZMK_GIT_RESULT
  )
  if(NOT CODEX_ZMK_GIT_RESULT EQUAL 0 OR
     NOT CODEX_ACTUAL_ZMK_COMMIT STREQUAL CODEX_PINNED_ZMK_COMMIT)
    message(FATAL_ERROR
      "Codex HIDS replacement requires ZMK ${CODEX_PINNED_ZMK_COMMIT}; got ${CODEX_ACTUAL_ZMK_COMMIT}")
  endif()
  if(NOT EXISTS "${CODEX_ZMK_HOG_SOURCE}")
    message(FATAL_ERROR "Pinned ZMK hog.c is missing: ${CODEX_ZMK_HOG_SOURCE}")
  endif()
  file(SHA256 "${CODEX_ZMK_HOG_SOURCE}" CODEX_ACTUAL_HOG_SHA256)
  if(NOT CODEX_ACTUAL_HOG_SHA256 STREQUAL CODEX_PINNED_HOG_SHA256)
    message(FATAL_ERROR
      "Pinned ZMK hog.c hash changed: ${CODEX_ACTUAL_HOG_SHA256}")
  endif()

  # app/CMakeLists adds hog.c after module discovery. Defer the exact source
  # list substitution until the end of that directory, then fail closed if
  # the pinned relative entry is absent or survives removal.
  function(codex_replace_pinned_zmk_hog)
    get_target_property(CODEX_APP_SOURCES app SOURCES)
    list(FIND CODEX_APP_SOURCES "src/hog.c" CODEX_HOG_INDEX)
    if(CODEX_HOG_INDEX EQUAL -1)
      message(FATAL_ERROR "Pinned ZMK app target no longer contains src/hog.c")
    endif()
    list(REMOVE_ITEM CODEX_APP_SOURCES "src/hog.c")
    list(FIND CODEX_APP_SOURCES "src/hog.c" CODEX_HOG_REMAINS)
    if(NOT CODEX_HOG_REMAINS EQUAL -1)
      message(FATAL_ERROR "Failed to remove pinned ZMK app/src/hog.c")
    endif()
    set_property(TARGET app PROPERTY SOURCES "${CODEX_APP_SOURCES}")
  endfunction()
  cmake_language(DEFER DIRECTORY "${APPLICATION_SOURCE_DIR}"
    CALL codex_replace_pinned_zmk_hog)
  target_sources(app PRIVATE src/transport/ble_hids.c)
  target_compile_definitions(app PRIVATE
    CODEX_PINNED_ZMK_HOG_SOURCE="${CODEX_ZMK_HOG_SOURCE}"
  )
endif()
