#include "usb_mic.h"
#include "usb_descriptors.h"
#include <string.h>

static UsbMic* active;
static const struct usb_string_descriptor manufacturer = USB_STRING_DESC("TV USB experiments");
static const struct usb_string_descriptor product = USB_STRING_DESC("Flipper Dummy Mic");
static const struct usb_string_descriptor serial = USB_STRING_DESC("dummy_mic_001");
static struct usb_device_descriptor device_descriptor = {
    .bLength = sizeof(struct usb_device_descriptor),
    .bDescriptorType = USB_DTYPE_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0xef,
    .bDeviceSubClass = 2,
    .bDeviceProtocol = 1,
    .bMaxPacketSize0 = 8, // Updated from the running HAL in init().
    // Local experimental identity, not an allocated product ID. See README.
    .idVendor = 0x1209,
    .idProduct = 0x0001,
    .bcdDevice = 0x0001,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static void queue_audio(usbd_device* dev) {
    int16_t samples[MIC_SAMPLES_PER_PACKET];
    mic_generate(samples, &active->seed, active->status.level, active->status.silence);
    if(usbd_ep_write(dev, MIC_AUDIO_EP, samples, sizeof(samples)) != sizeof(samples)) {
        active->status.errors++;
    }
}

static void audio_event(usbd_device* dev, uint8_t event, uint8_t ep) {
    UNUSED(ep);
    if(!active || event != usbd_evt_eptx || !active->status.alt) return;
    // STM32WB's ISO driver fills the inactive double buffer. The initial
    // active bank is empty, so its first completion carries no audio.
    if(active->skip_first_packet) {
        active->skip_first_packet = false;
    } else {
        active->status.packets++;
        active->status.session_packets++;
        active->status.last_packet_tick = furi_get_tick();
    }
    queue_audio(dev);
}

static void cdc_event(usbd_device* dev, uint8_t event, uint8_t ep) {
    if(!active) return;
    if(event == usbd_evt_eptx && ep == MIC_CDC_TX_EP) {
        active->tx_busy = false;
    } else if(event == usbd_evt_eprx && ep == MIC_CDC_RX_EP) {
        uint8_t bytes[64];
        int32_t count = usbd_ep_read(dev, ep, bytes, sizeof(bytes));
        if(count > 0) {
            if(furi_stream_buffer_spaces_available(active->rx) < (size_t)count) {
                active->rx_overflow = true;
                active->status.rx_overflows++;
            } else {
                furi_stream_buffer_send(active->rx, bytes, count, 0);
            }
        }
    }
}

static void stop_audio(usbd_device* dev) {
    if(active->status.alt) active->status.stops++;
    active->status.alt = 0;
    usbd_reg_endpoint(dev, MIC_AUDIO_EP, NULL);
    usbd_ep_deconfig(dev, MIC_AUDIO_EP);
}

static void clear_endpoints(usbd_device* dev) {
    stop_audio(dev);
    usbd_reg_endpoint(dev, MIC_CDC_RX_EP, NULL);
    usbd_reg_endpoint(dev, MIC_CDC_TX_EP, NULL);
    usbd_reg_endpoint(dev, MIC_CDC_NOTIFY_EP, NULL);
    usbd_ep_deconfig(dev, MIC_CDC_RX_EP);
    usbd_ep_deconfig(dev, MIC_CDC_TX_EP);
    usbd_ep_deconfig(dev, MIC_CDC_NOTIFY_EP);
    active->status.configured = false;
    active->tx_busy = false;
}

static usbd_respond configure(usbd_device* dev, uint8_t config) {
    if(config > 1) return usbd_fail;
    clear_endpoints(dev);
    if(!config) return usbd_ack;
    if(!usbd_ep_config(dev, MIC_CDC_RX_EP, USB_EPTYPE_BULK, 64) ||
       !usbd_ep_config(dev, MIC_CDC_TX_EP, USB_EPTYPE_BULK, 64) ||
       !usbd_ep_config(dev, MIC_CDC_NOTIFY_EP, USB_EPTYPE_INTERRUPT, 8)) {
        active->status.errors++;
        clear_endpoints(dev);
        return usbd_fail;
    }
    usbd_reg_endpoint(dev, MIC_CDC_RX_EP, cdc_event);
    usbd_reg_endpoint(dev, MIC_CDC_TX_EP, cdc_event);
    active->status.configured = true;
    active->status.suspended = false;
    return usbd_ack;
}

static usbd_respond control(usbd_device* dev, usbd_ctlreq* req, usbd_rqc_callback* callback) {
    UNUSED(callback);
    if(!active) return usbd_fail;
    if(req->bmRequestType == 0x81 && req->bRequest == USB_STD_GET_INTERFACE && req->wIndex < 4 &&
       req->wValue == 0 && req->wLength == 1) {
        req->data[0] = req->wIndex == 1 ? active->status.alt : 0;
        dev->status.data_ptr = req->data;
        dev->status.data_count = 1;
        return usbd_ack;
    }
    if(req->bmRequestType == 0x01 && req->bRequest == USB_STD_SET_INTERFACE && req->wLength == 0 &&
       req->wIndex < 4 && active->status.configured) {
        if(req->wIndex != 1) return req->wValue == 0 ? usbd_ack : usbd_fail;
        if(req->wValue > 1) return usbd_fail;
        stop_audio(dev);
        if(req->wValue == 1) {
            if(!usbd_ep_config(dev, MIC_AUDIO_EP, USB_EPTYPE_ISOCHRONUS, MIC_PACKET_BYTES)) {
                active->status.errors++;
                return usbd_fail;
            }
            active->status.alt = 1;
            active->status.session_packets = 0;
            active->status.starts++;
            active->skip_first_packet = true;
            usbd_reg_endpoint(dev, MIC_AUDIO_EP, audio_event);
            queue_audio(dev);
        }
        return usbd_ack;
    }
    // The format is fixed; tolerate hosts which still query/set the one rate.
    if(req->wIndex == MIC_AUDIO_EP && req->wValue == 0x0100 && req->wLength == 3) {
        if(req->bmRequestType == 0xa2 && req->bRequest == 0x81) {
            req->data[0] = MIC_RATE & 0xff;
            req->data[1] = (MIC_RATE >> 8) & 0xff;
            req->data[2] = (MIC_RATE >> 16) & 0xff;
            dev->status.data_ptr = req->data;
            dev->status.data_count = 3;
            return usbd_ack;
        }
        if(req->bmRequestType == 0x22 && req->bRequest == 1) {
            uint32_t rate = req->data[0] | (req->data[1] << 8) | (req->data[2] << 16);
            return rate == MIC_RATE ? usbd_ack : usbd_fail;
        }
    }
    if(req->wIndex == 2) {
        if(req->bmRequestType == 0x21 && req->bRequest == 0x20 && req->wLength == 7) {
            memcpy(active->line_coding, req->data, 7);
            return usbd_ack;
        }
        if(req->bmRequestType == 0xa1 && req->bRequest == 0x21 && req->wLength == 7) {
            dev->status.data_ptr = active->line_coding;
            dev->status.data_count = 7;
            return usbd_ack;
        }
        if(req->bmRequestType == 0x21 && req->bRequest == 0x22 && req->wLength == 0) {
            return usbd_ack;
        }
    }
    return usbd_fail;
}

static void init(usbd_device* dev, FuriHalUsbInterface* interface, void* context) {
    UNUSED(interface);
    device_descriptor.bMaxPacketSize0 = dev->status.ep0size;
    active = context;
    active->dev = dev;
    active->status.configured = false;
    active->status.alt = 0;
    active->status.suspended = false;
    active->tx_busy = false;
    usbd_reg_config(dev, configure);
    usbd_reg_control(dev, control);
    usbd_connect(dev, true);
}

static void deinit(usbd_device* dev) {
    FURI_CRITICAL_ENTER();
    clear_endpoints(dev);
    usbd_reg_config(dev, NULL);
    usbd_reg_control(dev, NULL);
    dev->complete_callback = NULL;
    active->dev = NULL;
    active = NULL;
    FURI_CRITICAL_EXIT();
}

static void wakeup(usbd_device* dev) {
    UNUSED(dev);
    if(active) active->status.suspended = false;
}

static void suspend(usbd_device* dev) {
    UNUSED(dev);
    if(active) active->status.suspended = true;
}

FuriHalUsbInterface usb_dummy_mic = {
    .init = init,
    .deinit = deinit,
    .wakeup = wakeup,
    .suspend = suspend,
    .dev_descr = (struct usb_device_descriptor*)&device_descriptor,
    .str_manuf_descr = (void*)&manufacturer,
    .str_prod_descr = (void*)&product,
    .str_serial_descr = (void*)&serial,
    .cfg_descr = (void*)mic_config_descriptor,
};

void usb_mic_snapshot(UsbMic* mic, MicStatus* status) {
    FURI_CRITICAL_ENTER();
    *status = mic->status;
    status->configured = mic->dev && mic->dev->status.device_state == usbd_state_configured;
    FURI_CRITICAL_EXIT();
    status->reading = status->configured && !status->suspended && status->alt &&
                      status->session_packets &&
                      (furi_get_tick() - status->last_packet_tick < furi_ms_to_ticks(250));
}

void usb_mic_reset_stats(UsbMic* mic) {
    FURI_CRITICAL_ENTER();
    mic->status.packets = 0;
    mic->status.session_packets = 0;
    mic->status.starts = 0;
    mic->status.stops = 0;
    mic->status.errors = 0;
    mic->status.rx_overflows = 0;
    FURI_CRITICAL_EXIT();
}

void usb_mic_set_signal(UsbMic* mic, bool silence, uint16_t level) {
    FURI_CRITICAL_ENTER();
    mic->status.silence = silence;
    mic->status.level = level;
    FURI_CRITICAL_EXIT();
}

bool usb_mic_take_overflow(UsbMic* mic) {
    FURI_CRITICAL_ENTER();
    bool overflow = mic->rx_overflow;
    if(overflow) {
        mic->rx_overflow = false;
        furi_stream_buffer_reset(mic->rx);
    }
    FURI_CRITICAL_EXIT();
    return overflow;
}

static bool send_packet(UsbMic* mic, const uint8_t* bytes, size_t length) {
    uint32_t start = furi_get_tick();
    while(furi_get_tick() - start < furi_ms_to_ticks(300)) {
        bool sent = false;
        FURI_CRITICAL_ENTER();
        if(mic->dev && mic->dev->status.device_state == usbd_state_configured &&
           !mic->status.suspended && !mic->tx_busy) {
            mic->tx_busy = true;
            sent = usbd_ep_write(mic->dev, MIC_CDC_TX_EP, bytes, length) == (int32_t)length;
            if(!sent) mic->tx_busy = false;
        }
        FURI_CRITICAL_EXIT();
        if(sent) return true;
        furi_delay_ms(1);
    }
    return false;
}

bool usb_mic_send(UsbMic* mic, const char* text) {
    size_t length = strlen(text);
    size_t total = length;
    while(length) {
        size_t count = length > 64 ? 64 : length;
        if(!send_packet(mic, (const uint8_t*)text, count)) return false;
        text += count;
        length -= count;
    }
    // Terminate a USB bulk transfer whose last packet was exactly full.
    if(total && total % 64 == 0 && !send_packet(mic, NULL, 0)) return false;
    uint32_t start = furi_get_tick();
    while(furi_get_tick() - start < furi_ms_to_ticks(300)) {
        FURI_CRITICAL_ENTER();
        bool busy = mic->tx_busy;
        FURI_CRITICAL_EXIT();
        if(!busy) return true;
        furi_delay_ms(1);
    }
    return false;
}
