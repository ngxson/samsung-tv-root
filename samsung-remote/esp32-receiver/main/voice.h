#pragma once

/*
 * Voice session control (REMOTE_PROTO.md §6). The mic is host-gated: the
 * remote only streams audio after the host writes a start command to the
 * 0xFB Feature report. Audio then arrives as 0xFB Input notifications
 * (ADPCM, ~192 bytes each), logged as "voice: FRAME" lines for
 * ../voice_decode.py.
 *
 * All functions run on the NimBLE host task.
 */

#include <stdbool.h>
#include <stdint.h>

#define VOICE_REPORT_ID 0xfb
/* The mic stream arrives on this Input report (180 bytes), seen live 2026-11-02 */
#define VOICE_AUDIO_REPORT_ID 0xf6

void voice_init(void);

/* GATT setup found the 0xFB Feature report characteristic (command channel) */
void voice_set_cmd_handle(uint16_t conn, uint16_t val_handle);

void voice_on_disconnect(void);

/* A Samsung key went down / up (auto-repeats are not passed in). Tapping
 * the voice key starts a session; it runs until the key is tapped again,
 * the audio stops for 1 s, or CONFIG_SRR_VOICE_MAX_MS. Ignored while a TV
 * is connected through the relay: the TV runs the session then. */
void voice_on_key(uint8_t code, bool down);

/* The TV wrote to the 0xFB Feature report through the relay (tv.c): forward
 * it to the remote. A start (31 04) also opens a session for logging */
void voice_relay_cmd(const uint8_t *cmd, uint16_t len);

/* A 0xFB Input report (audio frame) */
void voice_on_frame(const uint8_t *data, uint16_t len);
