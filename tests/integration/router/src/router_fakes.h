#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include <codex/framing.h>

void router_fakes_reset(void);
void router_fake_set_usb_powered(bool powered);
void router_fake_set_send_error(int error);
void router_fake_fail_ble_after(size_t successful_packets, int error);
void router_fake_switch_ble_connection_on_notify(size_t invocation);
size_t router_fake_ble_notify_count(uint8_t connection_id);
size_t router_fake_ble_disconnect_count(uint8_t connection_id);
size_t router_fake_ble_peak_references(uint8_t connection_id);
size_t router_fake_ble_references(uint8_t connection_id);
void router_fake_fail_usb_after(size_t successful_packets, int error);
void router_fake_clear_usb_failure(void);
int router_fake_wait_usb_attempts(size_t attempts, k_timeout_t timeout);
size_t router_fake_usb_session(void);
size_t router_fake_usb_packet_session(size_t index);
size_t router_fake_usb_disable_count(void);
size_t router_fake_usb_enable_count(void);
int router_fake_wait_usb_disables(size_t count, k_timeout_t timeout);
int router_fake_wait_usb_enables(size_t count, k_timeout_t timeout);
void router_fake_fail_usb_enable(size_t failures, int error);
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
size_t router_fake_ble_abort_count(void);

void router_fake_set_status(uint8_t profile, uint8_t layer, uint8_t battery);
