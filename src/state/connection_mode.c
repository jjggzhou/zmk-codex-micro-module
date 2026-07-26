#include <errno.h>

#include <zmk/ble.h>
#include <zmk/endpoints.h>

#include "state_internal.h"

static int active_profile_choice(enum codex_connection_choice *choice)
{
    int profile = zmk_ble_active_profile_index();

    if (choice == NULL || profile < 0 || profile > CODEX_CONNECTION_BLE_3) {
        return -EINVAL;
    }
    *choice = (enum codex_connection_choice)profile;
    return 0;
}

int codex_connection_initial_choice(enum codex_connection_choice *choice)
{
    struct zmk_endpoint_instance selected = zmk_endpoints_selected();
    int err;

    if (choice == NULL) {
        return -EINVAL;
    }
    if (selected.transport == ZMK_TRANSPORT_USB) {
        *choice = CODEX_CONNECTION_USB;
        return 0;
    }
    err = active_profile_choice(choice);
    if (err == 0) {
        return 0;
    }
    err = zmk_ble_prof_select(CODEX_CONNECTION_BLE_1);
    if (err == 0) {
        *choice = CODEX_CONNECTION_BLE_1;
    }
    return err;
}

int codex_connection_apply_choice(enum codex_connection_choice requested,
                                  enum codex_connection_choice *actual)
{
    int err;

    if (actual == NULL || (unsigned int)requested >= CODEX_CONNECTION_CHOICE_COUNT) {
        return -EINVAL;
    }

    if (requested == CODEX_CONNECTION_USB) {
        err = zmk_endpoints_select_transport(ZMK_TRANSPORT_USB);
        if (err == 0) {
            *actual = requested;
        }
        return err;
    }

    err = zmk_endpoints_select_transport(ZMK_TRANSPORT_BLE);
    if (err != 0) {
        return err;
    }
    err = zmk_ble_prof_select((uint8_t)requested);
    if (err == 0) {
        *actual = requested;
    } else if (active_profile_choice(actual) != 0) {
        *actual = CODEX_CONNECTION_CHOICE_COUNT;
    }
    return err;
}

int codex_connection_clear_choice(enum codex_connection_choice choice,
                                  enum codex_connection_choice *actual)
{
    int err;

    if (actual == NULL || (unsigned int)choice >= CODEX_CONNECTION_CHOICE_COUNT) {
        return -EINVAL;
    }
    *actual = choice;
    if (choice == CODEX_CONNECTION_USB) {
        return 0;
    }

    err = codex_connection_apply_choice(choice, actual);
    if (err != 0 || *actual != choice) {
        return err != 0 ? err : -EIO;
    }
    zmk_ble_clear_bonds();
    return 0;
}
