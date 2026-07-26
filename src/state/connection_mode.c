#include <errno.h>

#include <zmk/ble.h>
#include <zmk/endpoints.h>

#include "state_internal.h"

static enum codex_connection_choice active_profile_choice(void)
{
    int profile = zmk_ble_active_profile_index();

    return profile >= 0 && profile <= CODEX_CONNECTION_BLE_3
               ? (enum codex_connection_choice)profile
               : CODEX_CONNECTION_BLE_1;
}

enum codex_connection_choice codex_connection_initial_choice(void)
{
    struct zmk_endpoint_instance selected = zmk_endpoints_selected();

    return selected.transport == ZMK_TRANSPORT_USB ? CODEX_CONNECTION_USB
                                                    : active_profile_choice();
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
    *actual = err == 0 ? requested : active_profile_choice();
    return err;
}

int codex_connection_clear_choice(enum codex_connection_choice choice)
{
    enum codex_connection_choice actual = choice;
    int err;

    if ((unsigned int)choice >= CODEX_CONNECTION_CHOICE_COUNT) {
        return -EINVAL;
    }
    if (choice == CODEX_CONNECTION_USB) {
        return 0;
    }

    err = codex_connection_apply_choice(choice, &actual);
    if (err != 0 || actual != choice) {
        return err != 0 ? err : -EIO;
    }
    zmk_ble_clear_bonds();
    return 0;
}
