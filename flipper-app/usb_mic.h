#pragma once
#include "mic_core.h"
#include <furi.h>
#include <furi_hal_usb.h>

typedef struct {
    bool configured;
    bool suspended;
    bool reading;
    bool silence;
    uint8_t alt;
    uint16_t level;
    uint32_t packets;
    uint32_t session_packets;
    uint32_t starts;
    uint32_t stops;
    uint32_t errors;
    uint32_t rx_overflows;
    uint32_t last_packet_tick;
} MicStatus;

typedef struct {
    usbd_device* dev;
    FuriStreamBuffer* rx;
    MicStatus status;
    uint32_t seed;
    bool skip_first_packet;
    bool tx_busy;
    bool rx_overflow;
    uint8_t line_coding[7];
} UsbMic;

extern FuriHalUsbInterface usb_dummy_mic;
void usb_mic_snapshot(UsbMic* mic, MicStatus* status);
void usb_mic_reset_stats(UsbMic* mic);
void usb_mic_set_signal(UsbMic* mic, bool silence, uint16_t level);
bool usb_mic_send(UsbMic* mic, const char* text);
bool usb_mic_take_overflow(UsbMic* mic);
