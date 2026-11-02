#include "remote_events.h"

#include "esp_log.h"
#include "hid_parser.h"
#include "hid_usages.h"
#include "samsung_keys.h"
#include "voice.h"
#include "led.h"

static const char *TAG = "remote";

void remote_on_connection(bool connected)
{
    ESP_LOGI(TAG, "%s", connected ? "CONNECTED - press keys on the remote" : "DISCONNECTED");
}

void remote_on_key(uint32_t usage, bool down)
{
    const char *name = hid_usage_name(usage);
    ESP_LOGI(TAG, "KEY %-4s %-16s (%s:0x%04x)", down ? "DOWN" : "UP",
             name ? name : "?", hid_page_name(HID_USAGE_PAGE(usage)), HID_USAGE_ID(usage));
}

void remote_on_axis(uint32_t usage, int32_t value)
{
    const char *name = hid_usage_name(usage);
    ESP_LOGI(TAG, "AXIS %s (%s:0x%04x) = %ld", name ? name : "?",
             hid_page_name(HID_USAGE_PAGE(usage)), HID_USAGE_ID(usage), (long)value);
}

/* Whole payload as one hex line, so host scripts can grep frames back out */
static const char *hex_line(const uint8_t *data, uint16_t len)
{
    static char buf[2 * 256 + 1];
    static const char digits[] = "0123456789abcdef";
    if (len > 256) {
        len = 256;
    }
    for (int i = 0; i < len; i++) {
        buf[2 * i] = digits[data[i] >> 4];
        buf[2 * i + 1] = digits[data[i] & 0x0f];
    }
    buf[2 * len] = 0;
    return buf;
}

void remote_on_vendor_report(uint16_t handle, uint8_t report_id, const uint8_t *data, uint16_t len)
{
    ESP_LOGI(TAG, "VENDOR rid=0x%02x h=0x%04x len=%u %s", report_id, handle, len, hex_line(data, len));
}

void remote_on_other_notify(uint16_t handle, const char *uuid, const uint8_t *data, uint16_t len)
{
    ESP_LOGI(TAG, "NOTIFY h=0x%04x uuid=%s len=%u %s", handle, uuid, len, hex_line(data, len));
}

void remote_on_battery(uint8_t percent)
{
    ESP_LOGI(TAG, "BATTERY %u%%", percent);
}

void remote_on_samsung_key(uint8_t code, remote_key_event_t ev, uint16_t reports,
                           uint32_t held_ms, bool checksum_ok)
{
    const char *name = samsung_key_name(code);
    switch (ev) {
    case REMOTE_KEY_DOWN:
        ESP_LOGI(TAG, "SKEY DOWN   0x%02x %s", code, name ? name : "?");
        led_blink();
        voice_on_key(code, true);
        break;
    case REMOTE_KEY_REPEAT:
        ESP_LOGI(TAG, "SKEY REPEAT 0x%02x %s x%u", code, name ? name : "?", reports);
        break;
    case REMOTE_KEY_UP:
        ESP_LOGI(TAG, "SKEY UP     0x%02x %s held=%lums reports=%u checksum=%s", code,
                 name ? name : "?", (unsigned long)held_ms, reports, checksum_ok ? "ok" : "BAD");
        voice_on_key(code, false);
        break;
    }
}
