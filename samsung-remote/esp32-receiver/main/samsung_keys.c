#include "samsung_keys.h"

#include <stddef.h>
#include "esp_log.h"
#include "remote_events.h"
#include "samsung_keymap.h"

static const char *TAG = "skey";

static struct {
    bool down;
    uint8_t code;
    uint8_t sum;
    uint16_t reports;
    uint32_t t_down;
} s_key;

const char *samsung_key_name(uint8_t code)
{
    for (size_t i = 0; i < sizeof(SAMSUNG_KEYMAP) / sizeof(SAMSUNG_KEYMAP[0]); i++) {
        if (SAMSUNG_KEYMAP[i].code == code) {
            return SAMSUNG_KEYMAP[i].name;
        }
    }
    return NULL;
}

uint8_t samsung_key_down_code(void)
{
    return s_key.down ? s_key.code : 0;
}

void samsung_key_reset(void)
{
    s_key.down = false;
}

bool samsung_key_report(const uint8_t *d, uint16_t len)
{
    uint32_t now = esp_log_timestamp();

    if (len < 6) {
        return false;
    }

    if (d[0] != 0) {
        if (s_key.down && d[0] == s_key.code) {
            s_key.reports++;
            s_key.sum += d[0];
            remote_on_samsung_key(d[0], REMOTE_KEY_REPEAT, s_key.reports, now - s_key.t_down, true);
            return true;
        }
        if (s_key.down) {
            /* A different key without a release in between: never observed */
            ESP_LOGW(TAG, "key 0x%02x pressed while 0x%02x still down", d[0], s_key.code);
            remote_on_samsung_key(s_key.code, REMOTE_KEY_UP, s_key.reports, now - s_key.t_down, false);
        }
        s_key.down = true;
        s_key.code = d[0];
        s_key.sum = d[0];
        s_key.reports = 1;
        s_key.t_down = now;
        remote_on_samsung_key(d[0], REMOTE_KEY_DOWN, 1, 0, true);
        return true;
    }

    if (d[3] == 0x01) {
        if (!s_key.down) {
            ESP_LOGW(TAG, "release without press (checksum 0x%02x)", d[5]);
            return true;
        }
        bool ok = d[5] == s_key.sum;
        if (!ok) {
            ESP_LOGW(TAG, "checksum mismatch: remote 0x%02x, ours 0x%02x after %u reports (lost notifications?)",
                     d[5], s_key.sum, s_key.reports);
        }
        remote_on_samsung_key(s_key.code, REMOTE_KEY_UP, s_key.reports, now - s_key.t_down, ok);
        s_key.down = false;
        return true;
    }

    return false;
}
