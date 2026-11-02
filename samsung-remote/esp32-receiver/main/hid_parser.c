#include "hid_parser.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "hid_usages.h"

static const char *TAG = "hid";

#define MAX_REPORT_IDS 32
#define GLOBAL_STACK   4

typedef struct {
    uint16_t usage_page;
    int32_t logical_min;
    int32_t logical_max;
    uint8_t report_size;
    uint8_t report_count;
    uint8_t report_id;
} hid_globals_t;

typedef struct {
    uint8_t n_usages;
    uint32_t usages[HID_MAX_LOCAL_USAGES];
    uint32_t usage_min;
    uint32_t usage_max;
    bool has_min;
    bool has_max;
} hid_locals_t;

typedef struct {
    uint8_t report_id;
    uint16_t bits;
} hid_offset_t;

static uint32_t item_uval(const uint8_t *p, int size)
{
    switch (size) {
    case 1: return p[0];
    case 2: return p[0] | (p[1] << 8);
    case 4: return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
    default: return 0;
    }
}

static int32_t item_sval(const uint8_t *p, int size)
{
    switch (size) {
    case 1: return (int8_t)p[0];
    case 2: return (int16_t)(p[0] | (p[1] << 8));
    case 4: return (int32_t)item_uval(p, 4);
    default: return 0;
    }
}

/* Local usages without an explicit page take the page current at the main item */
static uint32_t extend_usage(uint32_t u, uint16_t page)
{
    return (u >> 16) ? u : HID_USAGE(page, u);
}

static uint16_t *offset_for(hid_offset_t *offs, int *n_offs, uint8_t report_id)
{
    for (int i = 0; i < *n_offs; i++) {
        if (offs[i].report_id == report_id) {
            return &offs[i].bits;
        }
    }
    if (*n_offs >= MAX_REPORT_IDS) {
        return NULL;
    }
    offs[*n_offs].report_id = report_id;
    offs[*n_offs].bits = 0;
    return &offs[(*n_offs)++].bits;
}

int hid_parse(hid_map_t *map, const uint8_t *desc, size_t len)
{
    hid_globals_t g = {0};
    hid_globals_t stack[GLOBAL_STACK];
    int sp = 0;
    hid_locals_t l = {0};
    hid_offset_t offs[MAX_REPORT_IDS];
    int n_offs = 0;
    int lmax_size = 0;
    size_t i = 0;

    memset(map, 0, sizeof(*map));

    while (i < len) {
        uint8_t prefix = desc[i];

        if (prefix == 0xfe) { /* long item: skip */
            if (i + 1 >= len) {
                break;
            }
            i += 3 + desc[i + 1];
            continue;
        }

        int size = prefix & 0x03;
        if (size == 3) {
            size = 4;
        }
        int type = (prefix >> 2) & 0x03;
        int tag = prefix >> 4;
        if (i + 1 + size > len) {
            ESP_LOGW(TAG, "truncated item at offset %u", (unsigned)i);
            break;
        }
        const uint8_t *data = &desc[i + 1];
        uint32_t uval = item_uval(data, size);

        if (type == 0) { /* main */
            if (tag == 0x8) { /* Input */
                uint16_t *off = offset_for(offs, &n_offs, g.report_id);
                if (off && g.report_size && g.report_count && map->n_fields < HID_MAX_FIELDS) {
                    hid_field_t *f = &map->fields[map->n_fields++];
                    f->report_id = g.report_id;
                    f->report_size = g.report_size;
                    f->report_count = g.report_count;
                    f->flags = (uint8_t)uval;
                    f->usage_page = g.usage_page;
                    f->bit_offset = *off;
                    f->logical_min = g.logical_min;
                    f->logical_max = g.logical_max;
                    /* Common descriptor bug: 0x25 0xFF meaning 255, not -1 */
                    if (g.logical_min >= 0 && g.logical_max < 0 && lmax_size < 4) {
                        f->logical_max = (int32_t)(g.logical_max & ((1u << (lmax_size * 8)) - 1));
                    }
                    f->usage_min = l.has_min ? extend_usage(l.usage_min, g.usage_page) : 0;
                    f->usage_max = l.has_max ? extend_usage(l.usage_max, g.usage_page) : 0;
                    f->n_usages = l.n_usages;
                    for (int k = 0; k < l.n_usages; k++) {
                        f->usages[k] = extend_usage(l.usages[k], g.usage_page);
                    }
                }
                if (off) {
                    *off += g.report_size * g.report_count;
                }
            } else if (tag == 0x9 || tag == 0xb) { /* Output / Feature: tracked separately by the device, ignore */
            }
            memset(&l, 0, sizeof(l));
        } else if (type == 1) { /* global */
            switch (tag) {
            case 0x0: g.usage_page = (uint16_t)uval; break;
            case 0x1: g.logical_min = item_sval(data, size); break;
            case 0x2: g.logical_max = item_sval(data, size); lmax_size = size; break;
            case 0x7: g.report_size = (uint8_t)uval; break;
            case 0x8: g.report_id = (uint8_t)uval; break;
            case 0x9: g.report_count = (uint8_t)uval; break;
            case 0xa: if (sp < GLOBAL_STACK) stack[sp++] = g; break;
            case 0xb: if (sp > 0) g = stack[--sp]; break;
            default: break;
            }
        } else if (type == 2) { /* local */
            uint32_t u = (size == 4) ? uval : (uval & 0xffff);
            switch (tag) {
            case 0x0:
                if (l.n_usages < HID_MAX_LOCAL_USAGES) {
                    l.usages[l.n_usages++] = u;
                }
                break;
            case 0x1: l.usage_min = u; l.has_min = true; break;
            case 0x2: l.usage_max = u; l.has_max = true; break;
            default: break;
            }
        }
        i += 1 + size;
    }

    map->valid = true;
    return 0;
}

void hid_dump(const hid_map_t *map)
{
    ESP_LOGI(TAG, "Report Map: %d input fields", map->n_fields);
    for (int i = 0; i < map->n_fields; i++) {
        const hid_field_t *f = &map->fields[i];
        const char *kind = (f->flags & 0x01) ? "const" : (f->flags & 0x02) ? "var  " : "array";
        if (f->flags & 0x01) {
            ESP_LOGI(TAG, "  rid=%-3u off=%-4u %ux%-2u %s (padding)",
                     f->report_id, f->bit_offset, f->report_count, f->report_size, kind);
            continue;
        }
        char usages[96] = "";
        if (f->n_usages) {
            int n = 0;
            for (int k = 0; k < f->n_usages && n < (int)sizeof(usages) - 8; k++) {
                n += snprintf(usages + n, sizeof(usages) - n, "%s0x%04x", k ? "," : "",
                              HID_USAGE_ID(f->usages[k]));
            }
        } else {
            snprintf(usages, sizeof(usages), "0x%04x..0x%04x",
                     HID_USAGE_ID(f->usage_min), HID_USAGE_ID(f->usage_max));
        }
        ESP_LOGI(TAG, "  rid=%-3u off=%-4u %ux%-2u %s page=0x%04x(%s) usages=%s logical=%ld..%ld",
                 f->report_id, f->bit_offset, f->report_count, f->report_size, kind,
                 f->usage_page, hid_page_name(f->usage_page), usages,
                 (long)f->logical_min, (long)f->logical_max);
    }
}

static uint32_t get_bits(const uint8_t *data, size_t len, uint32_t off, uint8_t n)
{
    uint32_t v = 0;
    for (uint8_t b = 0; b < n && b < 32; b++) {
        uint32_t bit = off + b;
        if (bit / 8 >= len) {
            break;
        }
        if ((data[bit / 8] >> (bit % 8)) & 1) {
            v |= 1u << b;
        }
    }
    return v;
}

static int32_t sign_extend(uint32_t v, uint8_t bits)
{
    if (bits == 0 || bits >= 32) {
        return (int32_t)v;
    }
    uint32_t m = 1u << (bits - 1);
    return (int32_t)((v ^ m) - m);
}

static uint32_t usage_at(const hid_field_t *f, uint32_t idx)
{
    if (f->n_usages) {
        return f->usages[idx < f->n_usages ? idx : f->n_usages - 1];
    }
    uint32_t u = f->usage_min + idx;
    if (f->usage_max && u > f->usage_max) {
        u = f->usage_max;
    }
    return u;
}

static void add_key(hid_decoded_t *out, uint32_t usage)
{
    for (int i = 0; i < out->n_keys; i++) {
        if (out->keys[i] == usage) {
            return;
        }
    }
    if (out->n_keys < HID_MAX_ACTIVE) {
        out->keys[out->n_keys++] = usage;
    }
}

void hid_decode(const hid_map_t *map, uint8_t report_id,
                const uint8_t *data, size_t len, hid_decoded_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!map->valid) {
        return;
    }

    for (int i = 0; i < map->n_fields; i++) {
        const hid_field_t *f = &map->fields[i];
        if (f->report_id != report_id) {
            continue;
        }
        out->matched = true;
        if (f->flags & 0x01) {
            continue;
        }
        if (f->usage_page >= HID_PAGE_VENDOR_MIN) {
            out->has_vendor = true;
            continue;
        }
        bool is_signed = f->logical_min < 0;

        for (uint32_t e = 0; e < f->report_count; e++) {
            uint32_t raw = get_bits(data, len, f->bit_offset + e * f->report_size, f->report_size);
            int32_t v = is_signed ? sign_extend(raw, f->report_size) : (int32_t)raw;

            if (f->flags & 0x02) { /* variable */
                uint32_t usage = usage_at(f, e);
                if (f->report_size == 1) {
                    if (v) {
                        add_key(out, usage);
                    }
                } else if (v && out->n_axes < HID_MAX_AXES) {
                    out->axes[out->n_axes].usage = usage;
                    out->axes[out->n_axes].value = v;
                    out->n_axes++;
                }
            } else { /* array: each element is an index of a pressed usage */
                if (v < f->logical_min || v > f->logical_max) {
                    continue;
                }
                uint32_t idx = (uint32_t)(v - f->logical_min);
                uint32_t usage = usage_at(f, idx);
                if (HID_USAGE_ID(usage) != 0) {
                    add_key(out, usage);
                }
            }
        }
    }
}
