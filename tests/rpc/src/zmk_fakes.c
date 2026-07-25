#include "zmk_fakes.h"

#include <zmk/usb.h>

static int fake_profile;
static uint8_t fake_layer;
static uint8_t fake_battery;
static enum zmk_usb_conn_state fake_usb_state;

void rpc_fake_status_set(int profile, uint8_t layer, uint8_t battery,
                         bool powered)
{
    fake_profile = profile;
    fake_layer = layer;
    fake_battery = battery;
    fake_usb_state = powered ? ZMK_USB_CONN_POWERED : ZMK_USB_CONN_NONE;
}

int zmk_ble_active_profile_index(void) { return fake_profile; }

uint8_t zmk_keymap_highest_layer_active(void) { return fake_layer; }

uint8_t zmk_battery_state_of_charge(void) { return fake_battery; }

enum zmk_usb_conn_state zmk_usb_get_conn_state(void) { return fake_usb_state; }
