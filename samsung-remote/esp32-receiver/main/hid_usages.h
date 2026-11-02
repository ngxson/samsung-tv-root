#pragma once

#include <stdint.h>

/* Short name of a usage page, never NULL */
const char *hid_page_name(uint16_t page);

/* Name of an extended usage ((page << 16) | id), or NULL if unknown */
const char *hid_usage_name(uint32_t usage);
