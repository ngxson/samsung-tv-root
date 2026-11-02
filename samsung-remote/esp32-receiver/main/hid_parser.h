#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HID_MAX_FIELDS       96
#define HID_MAX_LOCAL_USAGES 16
#define HID_MAX_ACTIVE       32
#define HID_MAX_AXES         8

/* Extended usage = (usage_page << 16) | usage_id */
#define HID_USAGE(page, id) (((uint32_t)(page) << 16) | (uint16_t)(id))
#define HID_USAGE_PAGE(u)   ((uint16_t)((u) >> 16))
#define HID_USAGE_ID(u)     ((uint16_t)((u) & 0xffff))

#define HID_PAGE_VENDOR_MIN 0xff00

/* One Input main item of the report descriptor */
typedef struct {
    uint8_t report_id;
    uint8_t report_size;  /* bits per element */
    uint8_t report_count; /* number of elements */
    uint8_t flags;        /* main item data: bit0 constant, bit1 variable */
    uint16_t usage_page;
    uint16_t bit_offset;  /* within the report, excluding the report ID byte */
    int32_t logical_min;
    int32_t logical_max;
    uint32_t usage_min;
    uint32_t usage_max;
    uint8_t n_usages;
    uint32_t usages[HID_MAX_LOCAL_USAGES];
} hid_field_t;

typedef struct {
    bool valid;
    int n_fields;
    hid_field_t fields[HID_MAX_FIELDS];
} hid_map_t;

typedef struct {
    uint32_t usage;
    int32_t value;
} hid_axis_t;

typedef struct {
    bool matched;    /* at least one field belongs to this report ID */
    bool has_vendor; /* report contains vendor-page fields (left undecoded) */
    uint8_t n_keys;
    uint32_t keys[HID_MAX_ACTIVE]; /* usages currently pressed */
    uint8_t n_axes;
    hid_axis_t axes[HID_MAX_AXES]; /* non-zero multi-bit values */
} hid_decoded_t;

/* Parse a HID report descriptor (Report Map). Returns 0 on success. */
int hid_parse(hid_map_t *map, const uint8_t *desc, size_t len);

/* Log a human readable summary of all input fields. */
void hid_dump(const hid_map_t *map);

/* Decode one input report (without the report ID byte, as delivered by HOGP). */
void hid_decode(const hid_map_t *map, uint8_t report_id,
                const uint8_t *data, size_t len, hid_decoded_t *out);
