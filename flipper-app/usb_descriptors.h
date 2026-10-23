#pragma once
#include "mic_core.h"

#define MIC_AUDIO_EP      0x81
#define MIC_CDC_RX_EP     0x02
#define MIC_CDC_TX_EP     0x83
#define MIC_CDC_NOTIFY_EP 0x84
#define MIC_CONFIG_BYTES  174

// USB Audio Class 1.0, fixed-rate PCM; interfaces 0/1 audio, 2/3 CDC ACM.
// Raw bytes make variable-length class descriptors independent of SDK structs.
// clang-format off
static const uint8_t mic_config_descriptor[] = {
    9, 2, MIC_CONFIG_BYTES, 0, 4, 1, 0, 0xc0, 250, // Battery powered, up to 500mA
    8, 11, 0, 2, 1, 0, 0, 0,                 // Audio IAD
    9, 4, 0, 0, 0, 1, 1, 0, 0,              // AudioControl
    9, 0x24, 1, 0x00, 0x01, 30, 0, 1, 1,    // AC header, 30 bytes
    12, 0x24, 2, 1, 0x01, 0x02, 0, 1, 0, 0, 0, 0, // Microphone input #1
    9, 0x24, 3, 2, 0x01, 0x01, 0, 1, 0,     // USB streaming output #2 <- #1
    9, 4, 1, 0, 0, 1, 2, 0, 0,              // AS idle: no endpoints
    9, 4, 1, 1, 1, 1, 2, 0, 0,              // AS active
    7, 0x24, 1, 2, 1, 1, 0,                  // Link #2, PCM
    11, 0x24, 2, 1, 1, 2, 16, 1,            // Type I, mono, 16-bit, one rate
    MIC_RATE & 0xff, (MIC_RATE >> 8) & 0xff, (MIC_RATE >> 16) & 0xff,
    9, 5, MIC_AUDIO_EP, 0x0d, MIC_PACKET_BYTES, 0, 1, 0, 0, // Sync iso IN, 1ms
    7, 0x25, 1, 0, 0, 0, 0,                 // Fixed rate; no endpoint controls
    8, 11, 2, 2, 2, 2, 0, 0,                // CDC IAD
    9, 4, 2, 0, 1, 2, 2, 0, 0,              // CDC control
    5, 0x24, 0, 0x10, 0x01,                 // CDC 1.10
    5, 0x24, 1, 0, 3,                       // Call management, data interface 3
    4, 0x24, 2, 0,                          // ACM, no notification capability
    5, 0x24, 6, 2, 3,                       // Union 2/3
    7, 5, MIC_CDC_NOTIFY_EP, 3, 8, 0, 16,
    9, 4, 3, 0, 2, 0x0a, 0, 0, 0,           // CDC data
    7, 5, MIC_CDC_RX_EP, 2, 64, 0, 0,
    7, 5, MIC_CDC_TX_EP, 2, 64, 0, 0,
};
// clang-format on
_Static_assert(sizeof(mic_config_descriptor) == MIC_CONFIG_BYTES, "USB descriptor length");
