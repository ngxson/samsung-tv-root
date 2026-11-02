#pragma once

/*
 * Decoder for the Samsung key report (HID input report ID 0xF7, 6 bytes).
 *
 *   press / auto-repeat:  CC 00 00 00 00 00    CC = key code, resent every
 *                                              ~100-120 ms while held
 *   release:              00 00 00 01 00 SS    SS = 8-bit sum of the CC bytes
 *                                              of every press report sent
 *                                              since the key went down
 */

#include <stdbool.h>
#include <stdint.h>

#define SAMSUNG_KEY_REPORT_ID 0xf7

/* Returns true if the report was a Samsung key report and has been handled */
bool samsung_key_report(const uint8_t *data, uint16_t len);

/* Forget any key in progress (call on disconnect) */
/* Code of the key currently held, 0 if none */
uint8_t samsung_key_down_code(void);

void samsung_key_reset(void);

/* Button name from samsung_keymap.h, or NULL if not mapped yet */
const char *samsung_key_name(uint8_t code);
