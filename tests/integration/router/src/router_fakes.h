#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include <codex/framing.h>

void router_fakes_reset(void);
void router_fake_set_usb_powered(bool powered);
void router_fake_set_send_error(int error);
void router_fake_block_first_usb_send(bool block);
int router_fake_wait_usb_send_entered(k_timeout_t timeout);
void router_fake_release_usb_send(void);
size_t router_fake_usb_count(void);
size_t router_fake_ble_count(void);
const uint8_t *router_fake_usb_packet(size_t index);
const uint8_t *router_fake_ble_packet(size_t index);
uint8_t router_fake_selected_profile(void);
enum codex_transport router_fake_standard_transport(void);
const char *router_fake_clear_log(void);
enum codex_transport router_fake_mouse_release_transport(void);
uint32_t router_fake_ble_generation(void);
size_t router_fake_ble_purge_count(void);

void router_fake_set_status(uint8_t profile, uint8_t layer, uint8_t battery);
