zephyr_library_named(codex_micro)
zephyr_include_directories(include)
target_include_directories(codex_micro PRIVATE
  ${APPLICATION_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src/protocol
)
target_sources(codex_micro PRIVATE src/descriptors/usb_report_descriptor.c)

target_sources_ifdef(CONFIG_CODEX_MICRO codex_micro PRIVATE
  src/descriptors/usb_identity.c
  src/descriptors/ble_report_map.c
  src/protocol/framing.c
  src/protocol/json_value.c
  src/protocol/rpc.c
  src/protocol/device_status.c
  src/lighting/model.c
  src/lighting/effects.c
  src/lighting/renderer.c
  src/input/codex_keys.c
  src/input/encoder.c
  src/input/touch_control.c
  src/state/layers.c
  src/state/connection_mode.c
  src/state/indicators.c
  src/transport/ble_queue.c
  src/transport/ble_report_state.c
  src/transport/usb_transport.c
)

target_sources_ifdef(CONFIG_CODEX_ANALOG_STICK codex_micro PRIVATE
  src/input/activity_bridge.c
  src/input/analog_stick.c
)

# Pinned ZMK edf5c081 treats every Zephyr input event as pointing activity.
# Codex's ADC source must instead wait for one complete calibrated radial
# sample, while every unrelated pointing device keeps the upstream behavior.
# Patch only the audited activity source and fail closed on any dependency or
# source drift; this is the same replacement pattern used below for hog.c and
# the pinned analog driver.
if(CONFIG_CODEX_ANALOG_STICK AND CONFIG_ZMK_POINTING)
  if(NOT DEFINED CODEX_ZMK_ACTIVITY_SOURCE)
    set(CODEX_ZMK_ACTIVITY_SOURCE "${APPLICATION_SOURCE_DIR}/src/activity.c")
  endif()
  set(CODEX_ACTIVITY_PINNED_ZMK_COMMIT "edf5c0814fd3ea202e43aad2d68fd32e882a518c")
  set(CODEX_ACTIVITY_PINNED_SHA256 "b67ad17db3aeb10fdede62183dfb8b319019ff916051d8adf7c1bb80a86c911e")
  get_filename_component(CODEX_ACTIVITY_APP_DIR "${CODEX_ZMK_ACTIVITY_SOURCE}" DIRECTORY)
  get_filename_component(CODEX_ACTIVITY_ZMK_DIR "${CODEX_ACTIVITY_APP_DIR}/../.." ABSOLUTE)

  if(NOT EXISTS "${CODEX_ZMK_ACTIVITY_SOURCE}")
    message(FATAL_ERROR "Pinned ZMK activity source is missing: ${CODEX_ZMK_ACTIVITY_SOURCE}")
  endif()
  execute_process(
    COMMAND git -C "${CODEX_ACTIVITY_ZMK_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE CODEX_ACTIVITY_ACTUAL_ZMK_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE CODEX_ACTIVITY_ZMK_GIT_RESULT
  )
  file(SHA256 "${CODEX_ZMK_ACTIVITY_SOURCE}" CODEX_ACTIVITY_ACTUAL_SHA256)
  if(NOT CODEX_ACTIVITY_ZMK_GIT_RESULT EQUAL 0 OR
     NOT CODEX_ACTIVITY_ACTUAL_ZMK_COMMIT STREQUAL CODEX_ACTIVITY_PINNED_ZMK_COMMIT OR
     NOT CODEX_ACTIVITY_ACTUAL_SHA256 STREQUAL CODEX_ACTIVITY_PINNED_SHA256)
    message(FATAL_ERROR
      "Codex activity override requires audited ZMK ${CODEX_ACTIVITY_PINNED_ZMK_COMMIT} activity.c")
  endif()

  file(READ "${CODEX_ZMK_ACTIVITY_SOURCE}" CODEX_ACTIVITY_PATCHED_SOURCE)
  set(CODEX_ACTIVITY_ORIGINAL_SOURCE "${CODEX_ACTIVITY_PATCHED_SOURCE}")
  set(CODEX_ACTIVITY_INCLUDE_ANCHOR "#include <zmk/activity.h>")
  set(CODEX_ACTIVITY_INCLUDE_PATCH
      "#include <zmk/activity.h>\n#include <codex/activity.h>")
  set(CODEX_ACTIVITY_NOTE_ANCHOR "static int note_activity(void)")
  set(CODEX_ACTIVITY_LISTENER_ANCHOR
      "static void activity_input_listener(struct input_event *ev) { k_work_submit(&note_activity_work); }")
  set(CODEX_ACTIVITY_LISTENER_PATCH
      "static void activity_input_listener(struct input_event *ev) {\n    if (!codex_activity_defer_input_device(ev->dev)) {\n        k_work_submit(&note_activity_work);\n    }\n}")
  foreach(CODEX_ACTIVITY_REQUIRED_ANCHOR
          CODEX_ACTIVITY_INCLUDE_ANCHOR CODEX_ACTIVITY_NOTE_ANCHOR
          CODEX_ACTIVITY_LISTENER_ANCHOR)
    string(FIND "${CODEX_ACTIVITY_PATCHED_SOURCE}"
           "${${CODEX_ACTIVITY_REQUIRED_ANCHOR}}" CODEX_ACTIVITY_ANCHOR_INDEX)
    if(CODEX_ACTIVITY_ANCHOR_INDEX EQUAL -1)
      message(FATAL_ERROR
        "Pinned ZMK activity API/source anchor changed: ${CODEX_ACTIVITY_REQUIRED_ANCHOR}")
    endif()
  endforeach()
  string(REPLACE "${CODEX_ACTIVITY_INCLUDE_ANCHOR}" "${CODEX_ACTIVITY_INCLUDE_PATCH}"
    CODEX_ACTIVITY_PATCHED_SOURCE "${CODEX_ACTIVITY_PATCHED_SOURCE}")
  string(REPLACE "${CODEX_ACTIVITY_NOTE_ANCHOR}" "int codex_zmk_activity_note(void)"
    CODEX_ACTIVITY_PATCHED_SOURCE "${CODEX_ACTIVITY_PATCHED_SOURCE}")
  string(REPLACE "note_activity();" "codex_zmk_activity_note();"
    CODEX_ACTIVITY_PATCHED_SOURCE "${CODEX_ACTIVITY_PATCHED_SOURCE}")
  string(REPLACE "${CODEX_ACTIVITY_LISTENER_ANCHOR}" "${CODEX_ACTIVITY_LISTENER_PATCH}"
    CODEX_ACTIVITY_PATCHED_SOURCE "${CODEX_ACTIVITY_PATCHED_SOURCE}")
  if(CODEX_ACTIVITY_PATCHED_SOURCE STREQUAL CODEX_ACTIVITY_ORIGINAL_SOURCE)
    message(FATAL_ERROR "Audited ZMK activity source was not patched")
  endif()

  set(CODEX_ACTIVITY_PATCHED_SOURCE_PATH
      "${CMAKE_CURRENT_BINARY_DIR}/codex_zmk_activity_edf5c081.c")
  file(WRITE "${CODEX_ACTIVITY_PATCHED_SOURCE_PATH}" "${CODEX_ACTIVITY_PATCHED_SOURCE}")
  set(CODEX_ACTIVITY_OVERRIDE_SOURCE "${CODEX_ZMK_ACTIVITY_SOURCE}"
      CACHE INTERNAL "Audited ZMK activity source")
  set(CODEX_ACTIVITY_OVERRIDE_PATCHED_SOURCE "${CODEX_ACTIVITY_PATCHED_SOURCE_PATH}"
      CACHE INTERNAL "Generated Codex-aware ZMK activity source")

  function(codex_replace_pinned_zmk_activity)
    get_target_property(CODEX_ACTIVITY_APP_SOURCES app SOURCES)
    list(FIND CODEX_ACTIVITY_APP_SOURCES "src/activity.c" CODEX_ACTIVITY_RELATIVE_INDEX)
    list(FIND CODEX_ACTIVITY_APP_SOURCES "${CODEX_ACTIVITY_OVERRIDE_SOURCE}"
         CODEX_ACTIVITY_ABSOLUTE_INDEX)
    if(CODEX_ACTIVITY_RELATIVE_INDEX EQUAL -1 AND CODEX_ACTIVITY_ABSOLUTE_INDEX EQUAL -1)
      message(FATAL_ERROR "Pinned ZMK app target no longer contains activity.c")
    endif()
    list(REMOVE_ITEM CODEX_ACTIVITY_APP_SOURCES
         "src/activity.c" "${CODEX_ACTIVITY_OVERRIDE_SOURCE}")
    list(FIND CODEX_ACTIVITY_APP_SOURCES "src/activity.c" CODEX_ACTIVITY_RELATIVE_REMAINS)
    list(FIND CODEX_ACTIVITY_APP_SOURCES "${CODEX_ACTIVITY_OVERRIDE_SOURCE}"
         CODEX_ACTIVITY_ABSOLUTE_REMAINS)
    if(NOT CODEX_ACTIVITY_RELATIVE_REMAINS EQUAL -1 OR
       NOT CODEX_ACTIVITY_ABSOLUTE_REMAINS EQUAL -1)
      message(FATAL_ERROR "Failed to remove pinned ZMK app/src/activity.c")
    endif()
    set_property(TARGET app PROPERTY SOURCES "${CODEX_ACTIVITY_APP_SOURCES}")
    target_sources(app PRIVATE "${CODEX_ACTIVITY_OVERRIDE_PATCHED_SOURCE}")
  endfunction()
  cmake_language(DEFER DIRECTORY "${APPLICATION_SOURCE_DIR}"
    CALL codex_replace_pinned_zmk_activity)
endif()

# badjeff 2684f22 has an unsigned reverse loop which walks past index zero when
# every report-on-change-only channel is unchanged. Keep west pinned to that
# audited commit, verify both commit and source hash, then generate the one-line
# MIT-preserving correction in the build tree and replace only that library source.
if(CONFIG_ANALOG_INPUT)
  set(CODEX_ANALOG_UPSTREAM_DIR "${ZEPHYR_ZMK_ANALOG_INPUT_DRIVER_MODULE_DIR}")
  set(CODEX_ANALOG_UPSTREAM_SOURCE "${CODEX_ANALOG_UPSTREAM_DIR}/src/analog_input.c")
  set(CODEX_ANALOG_UPSTREAM_COMMIT "2684f22ee7e2168d4393f7e63676912210a796fc")
  set(CODEX_ANALOG_UPSTREAM_SHA256 "a506f92c2df688b90f34189bcc7f9fba954214248c12afcc01ac8d64944131af")
  execute_process(COMMAND git -C "${CODEX_ANALOG_UPSTREAM_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE CODEX_ANALOG_ACTUAL_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE CODEX_ANALOG_GIT_RESULT)
  file(SHA256 "${CODEX_ANALOG_UPSTREAM_SOURCE}" CODEX_ANALOG_ACTUAL_SHA256)
  if(NOT CODEX_ANALOG_GIT_RESULT EQUAL 0 OR
     NOT CODEX_ANALOG_ACTUAL_COMMIT STREQUAL CODEX_ANALOG_UPSTREAM_COMMIT OR
     NOT CODEX_ANALOG_ACTUAL_SHA256 STREQUAL CODEX_ANALOG_UPSTREAM_SHA256)
    message(FATAL_ERROR "Codex analog override requires audited badjeff 2684f22 source")
  endif()
  file(READ "${CODEX_ANALOG_UPSTREAM_SOURCE}" CODEX_ANALOG_PATCHED_SOURCE)
  set(CODEX_ANALOG_ORIGINAL_SOURCE "${CODEX_ANALOG_PATCHED_SOURCE}")
  set(CODEX_ANALOG_BAD_LOOP "for (uint8_t i = config->io_channels_len - 1; i >= 0; i--)")
  set(CODEX_ANALOG_GOOD_LOOP "for (int i = (int)config->io_channels_len - 1; i >= 0; i--)")
  string(REPLACE "${CODEX_ANALOG_BAD_LOOP}" "${CODEX_ANALOG_GOOD_LOOP}"
    CODEX_ANALOG_PATCHED_SOURCE "${CODEX_ANALOG_PATCHED_SOURCE}")
  if(CODEX_ANALOG_PATCHED_SOURCE STREQUAL CODEX_ANALOG_ORIGINAL_SOURCE)
    message(FATAL_ERROR "Audited analog loop was not found for patching")
  endif()
  set(CODEX_ANALOG_PATCHED_SOURCE_PATH "${CMAKE_CURRENT_BINARY_DIR}/codex_analog_input_2684f22.c")
  file(WRITE "${CODEX_ANALOG_PATCHED_SOURCE_PATH}" "${CODEX_ANALOG_PATCHED_SOURCE}")
  # zephyr_library() names a library from its directory relative to
  # ZEPHYR_BASE. Derive the name from the audited module path instead of
  # assuming where a consuming application places the module.
  file(RELATIVE_PATH CODEX_ANALOG_TARGET_RELATIVE_PATH "${ZEPHYR_BASE}"
       "${CODEX_ANALOG_UPSTREAM_DIR}")
  string(REPLACE "/" "__" CODEX_ANALOG_UPSTREAM_TARGET
         "${CODEX_ANALOG_TARGET_RELATIVE_PATH}")
  string(REPLACE ":" "__" CODEX_ANALOG_UPSTREAM_TARGET
         "${CODEX_ANALOG_UPSTREAM_TARGET}")
  # The substitution runs deferred in APPLICATION_SOURCE_DIR; preserve its
  # exact arguments beyond this module directory's CMake scope.
  set(CODEX_ANALOG_OVERRIDE_TARGET "${CODEX_ANALOG_UPSTREAM_TARGET}"
      CACHE INTERNAL "Audited analog library target")
  set(CODEX_ANALOG_OVERRIDE_SOURCE "${CODEX_ANALOG_UPSTREAM_SOURCE}"
      CACHE INTERNAL "Audited analog source")
  set(CODEX_ANALOG_OVERRIDE_PATCHED_SOURCE "${CODEX_ANALOG_PATCHED_SOURCE_PATH}"
      CACHE INTERNAL "Generated corrected analog source")
  function(codex_replace_badjeff_analog_source)
    if(NOT TARGET "${CODEX_ANALOG_OVERRIDE_TARGET}")
      message(FATAL_ERROR "Pinned badjeff Zephyr library target is unavailable: ${CODEX_ANALOG_OVERRIDE_TARGET}")
    endif()
    get_target_property(CODEX_ANALOG_SOURCES "${CODEX_ANALOG_OVERRIDE_TARGET}" SOURCES)
    # zephyr_library_sources_ifdef retains the path as written in the
    # upstream CMakeLists (relative), while source inspection is absolute.
    # Remove both representations and fail closed if the audited original
    # still remains, so a build can never compile both implementations.
    list(REMOVE_ITEM CODEX_ANALOG_SOURCES
         "${CODEX_ANALOG_OVERRIDE_SOURCE}" "src/analog_input.c")
    list(FIND CODEX_ANALOG_SOURCES "${CODEX_ANALOG_OVERRIDE_SOURCE}"
         CODEX_ANALOG_ABSOLUTE_REMAINS)
    list(FIND CODEX_ANALOG_SOURCES "src/analog_input.c"
         CODEX_ANALOG_RELATIVE_REMAINS)
    if(NOT CODEX_ANALOG_ABSOLUTE_REMAINS EQUAL -1 OR
       NOT CODEX_ANALOG_RELATIVE_REMAINS EQUAL -1)
      message(FATAL_ERROR "Failed to remove audited badjeff analog_input.c")
    endif()
    set_property(TARGET "${CODEX_ANALOG_OVERRIDE_TARGET}" PROPERTY SOURCES "${CODEX_ANALOG_SOURCES}")
    target_sources("${CODEX_ANALOG_OVERRIDE_TARGET}" PRIVATE "${CODEX_ANALOG_OVERRIDE_PATCHED_SOURCE}")
    if(DEFINED ZMK_SOURCE_DIR)
      target_include_directories("${CODEX_ANALOG_OVERRIDE_TARGET}" PRIVATE
        "${ZMK_SOURCE_DIR}/app/include")
    endif()
  endfunction()
  cmake_language(DEFER DIRECTORY "${APPLICATION_SOURCE_DIR}" CALL codex_replace_badjeff_analog_source)
endif()

target_sources_ifdef(CONFIG_CODEX_REPORT_ROUTER codex_micro PRIVATE
  src/transport/report_router.c
)

if(CONFIG_CODEX_REPORT_ROUTER AND CONFIG_ZMK_POINTING)
  zephyr_ld_options(-Wl,--wrap=zmk_hid_mouse_clear)
endif()
if(CONFIG_CODEX_REPORT_ROUTER AND CONFIG_ZMK_USB)
  zephyr_ld_options(-Wl,--wrap=usb_enable)
endif()

if(CONFIG_CODEX_MICRO)
  zephyr_ld_options(
    -Wl,--wrap=usb_get_device_descriptor
    -Wl,--wrap=usb_hid_register_device
    -Wl,--wrap=hid_int_ep_write
    -Wl,--wrap=zmk_usb_hid_send_mouse_report
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
