#pragma once

/*
 * Application hooks. main.c calls these from the NimBLE host task; the
 * default implementations in remote_events.c only log. Put your own logic
 * (UART, MQTT, GPIO, IR blaster, ...) there.
 */

#include <stdbool.h>
#include <stdint.h>

void remote_on_connection(bool connected);

/* A decoded key went down / up. usage = (page << 16) | id, see hid_parser.h */
void remote_on_key(uint32_t usage, bool down);

/* A non-zero multi-bit value (touchpad / pointer / sensor axis) */
void remote_on_axis(uint32_t usage, int32_t value);

/* A HID report containing vendor-page data, or not described by the Report Map */
void remote_on_vendor_report(uint16_t handle, uint8_t report_id, const uint8_t *data, uint16_t len);

/* A notification from a non-HID characteristic (e.g. Samsung vendor service) */
void remote_on_other_notify(uint16_t handle, const char *uuid, const uint8_t *data, uint16_t len);

void remote_on_battery(uint8_t percent);

typedef enum {
    REMOTE_KEY_DOWN,
    REMOTE_KEY_REPEAT,
    REMOTE_KEY_UP,
} remote_key_event_t;

/*
 * A Samsung remote button (vendor report 0xF7). reports = press reports so
 * far (1 on DOWN, +1 per auto-repeat), held_ms = time since DOWN,
 * checksum_ok is only meaningful on UP.
 */
void remote_on_samsung_key(uint8_t code, remote_key_event_t ev, uint16_t reports,
                           uint32_t held_ms, bool checksum_ok);
