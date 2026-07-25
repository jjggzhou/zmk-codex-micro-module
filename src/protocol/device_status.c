#include <codex/rpc.h>

#include <limits.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/keymap.h>
#include <zmk/usb.h>

struct codex_device_status codex_device_status_snapshot(void)
{
    int profile = zmk_ble_active_profile_index();
    uint8_t battery = zmk_battery_state_of_charge();

    return (struct codex_device_status){
        .version = CODEX_FIRMWARE_VERSION,
        .profile_index =
            profile >= 0 && profile <= UINT8_MAX ? (uint8_t)profile : 0U,
        .layer_index = zmk_keymap_highest_layer_active(),
        .battery_percent = battery <= 100U ? battery : 100U,
        .is_charging = zmk_usb_is_powered(),
    };
}
