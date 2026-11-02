#pragma once

/*
 * Status LED (CONFIG_SRR_LED_GPIO): a short blink per key press, solid on
 * while a voice session waits for audio, rapid blinking while audio is
 * recorded, slow blinking while in TV pairing mode. Safe to call from any
 * task.
 */

#include <stdbool.h>

typedef enum {
    LED_IDLE,
    LED_SESSION,   /* voice session started, no audio yet */
    LED_RECORDING, /* audio frames arriving */
} led_mode_t;

void led_init(void);
void led_blink(void);
void led_set_mode(led_mode_t mode);
void led_set_pairing(bool on);
