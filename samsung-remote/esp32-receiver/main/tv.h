#pragma once

/*
 * TV relay: the board also acts as a Smart Remote towards the TV
 * (remote --> ESP32 --> TV). It serves a copy of the remote's GATT layout
 * (HID service with the same Report Map and report characteristics,
 * REMOTE_PROTO.md §3/§4), forwards the remote's input reports to the TV and
 * the TV's 0xFB Feature writes (voice commands) to the remote. Every
 * forwarded command is logged, so this doubles as a sniffer of what the TV
 * sends.
 *
 * Pairing: a short press on the BOOT button advertises like a remote in
 * pairing mode for CONFIG_SRR_TV_PAIR_S seconds. Once bonded, the board
 * advertises whenever the TV is not connected so it can reconnect.
 *
 * tv_init() runs before the host starts; everything else on the NimBLE host
 * task, except tv_pairing_request() (any task).
 */

#include <stdbool.h>
#include <stdint.h>
#include "host/ble_hs.h"

void tv_init(void);

/* The host is synced: start advertising for reconnection if a TV is bonded */
void tv_start(void);

/* BOOT button short press */
void tv_pairing_request(void);

bool tv_connected(void);

/* The remote link is up (GATT setup done) or down. The relay advertises to
 * the TV only while it is up, when the board isn't scanning: the C3
 * controller asserts (sch_arb.c 517) when the TV connects during a scan */
void tv_set_remote_up(bool up);

/* Forget the bonded TV's address (the bond store itself is cleared by the caller) */
void tv_forget(void);

/* The bonded TV's identity address: the remote side must not use that bond */
bool tv_is_peer(const ble_addr_t *addr);

/* A report from the remote: forwarded if the TV subscribed to it */
void tv_relay_report(uint8_t report_id, uint8_t report_type, const uint8_t *data, uint16_t len);

void tv_relay_battery(uint8_t level);

/* The remote's PnP ID (0x2A50) was read: the TV gets it with product ID + 1
 * (kept in NVS), unless CONFIG_SRR_TV_PNP_ID fixes it */
void tv_set_remote_pnp(const uint8_t *pnp, uint16_t len);
