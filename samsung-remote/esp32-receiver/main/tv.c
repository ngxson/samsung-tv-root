#include "tv.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "led.h"
#include "voice.h"

static const char *TAG = "tv";

#if CONFIG_SRR_TV_RELAY

#define NVS_NS         "srr"
#define NVS_KEY_TV     "tv_addr"
#define NVS_KEY_PNP    "tv_pnp"
#define ADV_INSTANCE   0
#define MAX_REPORT_LEN 200

/* Report Map read from the Smart Control 2016 (REMOTE_PROTO.md §4.1) */
static const uint8_t REPORT_MAP[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01, 0x75, 0x01, 0x95, 0x08, 0x05, 0x07, 0x19, 0xe0,
    0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x03, 0x95, 0x05,
    0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x03,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xc0, 0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19,
    0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02, 0x75, 0x05, 0x95,
    0x01, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95,
    0x02, 0x81, 0x06, 0xc0, 0xc0, 0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0xf6, 0x95, 0xb4, 0x75,
    0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0x81, 0x00, 0xc0, 0x06, 0xf0, 0xff, 0x09, 0x01, 0xa1, 0x01,
    0x85, 0xf7, 0x09, 0x2f, 0x75, 0x08, 0x95, 0x06, 0x15, 0x00, 0x26, 0xff, 0x00, 0x81, 0x06, 0xc0,
    0x06, 0xf0, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x85, 0xf8, 0x09, 0x2f, 0x75, 0x08, 0x95, 0x06, 0x15,
    0x00, 0x26, 0xff, 0x00, 0x81, 0x06, 0xc0, 0x06, 0xf0, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x85, 0xf9,
    0x09, 0x2f, 0x75, 0x08, 0x95, 0x12, 0x15, 0x00, 0x26, 0xff, 0x00, 0x81, 0x06, 0xc0, 0x09, 0x01,
    0xa1, 0x01, 0x85, 0xfb, 0x95, 0xc0, 0x75, 0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0x81, 0x00, 0x95,
    0x0b, 0x75, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00, 0x81, 0x00, 0xc0, 0x06, 0x01, 0xff, 0x09, 0x01,
    0xa1, 0x01, 0xa1, 0x02, 0x85, 0x07, 0x09, 0x14, 0x15, 0x80, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x14,
    0x81, 0x22, 0xc0, 0xc0, 0x06, 0x01, 0xff, 0x09, 0x01, 0xa1, 0x01, 0xa1, 0x02, 0x85, 0x08, 0x09,
    0x14, 0x15, 0x80, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x14, 0x91, 0x22, 0xc0, 0xc0,
};

/* HID Information as read from the remote: bcdHID 1.11, country 0, flags 0 */
static const uint8_t HID_INFO[] = {0x11, 0x01, 0x00, 0x00};

/* The remote's pairing-mode advert (REMOTE_PROTO.md §2.4): flags, peripheral
 * connection interval 0x0010..0x0020, appearance 0x0180, uuid16 1812/180f/180a */
static uint8_t s_adv[] = {
    0x02, 0x01, 0x05,
    0x05, 0x12, 0x10, 0x00, 0x20, 0x00,
    0x03, 0x19, 0x80, 0x01,
    0x07, 0x03, 0x12, 0x18, 0x0f, 0x18, 0x0a, 0x18,
};
#define ADV_FLAGS_IDX 2
#define FLAGS_PAIRING 0x05 /* LE limited discoverable, no BR/EDR */
#define FLAGS_BONDED  0x04 /* not discoverable: only the bonded TV connects */

typedef enum {
    ATTR_REPORT_MAP,
    ATTR_HID_INFO,
    ATTR_CONTROL_POINT,
    ATTR_PROTOCOL_MODE,
    ATTR_BATTERY,
    ATTR_PNP_ID,
} attr_t;

/* A read-only value copied from the remote */
typedef struct {
    const char *name;
    const void *data;
    uint16_t len;
} const_attr_t;
#define CONST_BIN(n, ...) ((void *)&(const_attr_t){n, (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__})})
#define CONST_STR(n, s)   ((void *)&(const_attr_t){n, s, sizeof(s) - 1})


/* The report characteristics of the remote, in its handle order (REMOTE_PROTO.md §4.0) */
typedef struct {
    uint8_t id;
    uint8_t type; /* 1 input, 2 output, 3 feature */
    uint16_t handle;
    bool notify;
    uint16_t len;
    uint8_t value[MAX_REPORT_LEN];
} tv_report_t;

static tv_report_t s_reports[] = {
    {.id = 0xf7, .type = 1},
    {.id = 0xf6, .type = 1},
    {.id = VOICE_REPORT_ID, .type = 3},
    {.id = VOICE_REPORT_ID, .type = 1},
};
#define N_REPORTS (sizeof(s_reports) / sizeof(s_reports[0]))

static uint16_t s_battery_handle;
static bool s_battery_notify;
static uint16_t s_sc_handle;    /* Service Changed (0x2A05) */
/* value handles that only exist to check the layout against the remote's */
static uint16_t s_gap_car_handle, s_hid_info_handle, s_report_map_handle, s_vendor_a002_handle;
static uint8_t s_battery = 100;
static uint8_t s_protocol_mode = 1; /* report protocol */
/* Remote's PnP ID with product ID + 1, until the remote is read: Realtek 0x005D:0x0002 */
static uint8_t s_pnp[7] = {0x01, 0x5d, 0x00, 0x02, 0x00, 0x03, 0x00};
static bool s_pnp_fixed; /* set by CONFIG_SRR_TV_PNP_ID */

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;
static bool s_synced;
static bool s_pairing;
static bool s_remote_up;
static bool s_forget_on_disconnect;
static bool s_have_tv;
static ble_addr_t s_tv;
static bool s_warned_mtu;
static bool s_ready;        /* link to the bonded TV is encrypted: reports may flow */
static bool s_logged_relay; /* first relayed report of this link logged */
static struct ble_npl_event s_pair_ev;

static int gap_event(struct ble_gap_event *event, void *arg);

static const char *fmt_addr(const ble_addr_t *a)
{
    static char buf[24];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", a->val[5], a->val[4], a->val[3], a->val[2],
             a->val[1], a->val[0]);
    return buf;
}

static const char *hex_line(const uint8_t *data, uint16_t len)
{
    static char buf[2 * MAX_REPORT_LEN + 1];
    static const char digits[] = "0123456789abcdef";
    if (len > MAX_REPORT_LEN) {
        len = MAX_REPORT_LEN;
    }
    for (int i = 0; i < len; i++) {
        buf[2 * i] = digits[data[i] >> 4];
        buf[2 * i + 1] = digits[data[i] & 0x0f];
    }
    buf[2 * len] = 0;
    return buf;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* ------------------------------------------------------------------------ */
/* the bonded TV                                                            */
/* ------------------------------------------------------------------------ */

static ble_addr_t identity(const ble_addr_t *a)
{
    ble_addr_t id = *a;
    if (id.type == BLE_ADDR_PUBLIC_ID || id.type == BLE_ADDR_RANDOM_ID) {
        id.type -= 2; /* controller resolved an RPA to this identity address */
    }
    return id;
}

static void tv_save(const ble_addr_t *addr)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (addr) {
        nvs_set_blob(h, NVS_KEY_TV, addr, sizeof(*addr));
    } else {
        nvs_erase_key(h, NVS_KEY_TV);
    }
    nvs_commit(h);
    nvs_close(h);
}

static void tv_load(void)
{
    nvs_handle_t h;
    size_t len = sizeof(s_tv);
    s_have_tv = nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK &&
                nvs_get_blob(h, NVS_KEY_TV, &s_tv, &len) == ESP_OK && len == sizeof(s_tv);
    if (s_have_tv) {
        nvs_close(h);
    }
}

/* The TV address is only kept while its bond is (BOOT held 3 s clears the store) */
static bool tv_bonded(void)
{
    struct ble_store_key_sec key = {.peer_addr = s_tv};
    struct ble_store_value_sec value;
    return s_have_tv && ble_store_read_peer_sec(&key, &value) == 0;
}

/* The remote's bond went missing once after the TV paired: list the store when it changes */
static void log_bonds(const char *when)
{
    ble_addr_t peers[4];
    int n = 0;
    ble_store_util_bonded_peers(peers, &n, 4);
    ESP_LOGI(TAG, "bonds %s: %d", when, n);
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "  %s%s", fmt_addr(&peers[i]), ble_addr_cmp(&peers[i], &s_tv) == 0 ? " (TV)" : "");
    }
}

bool tv_is_peer(const ble_addr_t *addr)
{
    ble_addr_t id = identity(addr);
    return s_have_tv && ble_addr_cmp(&id, &s_tv) == 0;
}

bool tv_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

void tv_forget(void)
{
    tv_save(NULL);
    s_have_tv = false;
}

/* ------------------------------------------------------------------------ */
/* PnP ID                                                                   */
/* ------------------------------------------------------------------------ */

static void pnp_load(void)
{
    nvs_handle_t h;
    size_t len = sizeof(s_pnp);
    uint8_t pnp[sizeof(s_pnp)];
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    if (nvs_get_blob(h, NVS_KEY_PNP, pnp, &len) == ESP_OK && len == sizeof(pnp)) {
        memcpy(s_pnp, pnp, sizeof(s_pnp));
    }
    nvs_close(h);
}

void tv_set_remote_pnp(const uint8_t *pnp, uint16_t len)
{
    if (s_pnp_fixed || len != sizeof(s_pnp)) {
        return;
    }
    /* Same as the remote but product ID + 1, so the TV can tell the two apart */
    uint8_t ours[sizeof(s_pnp)];
    memcpy(ours, pnp, sizeof(ours));
    uint16_t pid = (ours[3] | ours[4] << 8) + 1;
    ours[3] = pid & 0xff;
    ours[4] = pid >> 8;
    if (memcmp(ours, s_pnp, sizeof(ours)) == 0) {
        return;
    }
    memcpy(s_pnp, ours, sizeof(s_pnp));
    ESP_LOGI(TAG, "PnP ID for the TV: vendor 0x%04x product 0x%04x (remote's + 1)", s_pnp[1] | s_pnp[2] << 8, pid);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY_PNP, s_pnp, sizeof(s_pnp));
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ------------------------------------------------------------------------ */
/* advertising                                                              */
/* ------------------------------------------------------------------------ */

static void adv_stop(void)
{
#if MYNEWT_VAL(BLE_EXT_ADV)
    if (ble_gap_ext_adv_active(ADV_INSTANCE)) {
        ble_gap_ext_adv_stop(ADV_INSTANCE);
    }
#else
    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }
#endif
}

static void adv_start(void)
{
    if (!s_synced || !s_remote_up || tv_connected() || !(s_pairing || tv_bonded())) {
        return;
    }
    adv_stop();
    s_adv[ADV_FLAGS_IDX] = s_pairing ? FLAGS_PAIRING : FLAGS_BONDED;
    uint8_t rsp[2 + 31];
    int name_len = strlen(CONFIG_SRR_TV_NAME);
    if (name_len > 29) {
        name_len = 29;
    }
    rsp[0] = name_len + 1;
    rsp[1] = BLE_HS_ADV_TYPE_COMP_NAME;
    memcpy(rsp + 2, CONFIG_SRR_TV_NAME, name_len);
    /* 30 ms in both modes: with 100 ms for reconnection the C3 controller
     * asserted (sch_arb.c 517) within 1 s of boot while scanning */
    uint32_t itvl = BLE_GAP_ADV_ITVL_MS(30);
    int rc;

#if MYNEWT_VAL(BLE_EXT_ADV)
    struct ble_gap_ext_adv_params p = {
        .connectable = 1,
        .scannable = 1,
        .legacy_pdu = 1,
        .itvl_min = itvl,
        .itvl_max = itvl + itvl / 4,
        .own_addr_type = s_own_addr_type,
        .primary_phy = BLE_HCI_LE_PHY_1M,
        .secondary_phy = BLE_HCI_LE_PHY_1M,
        .tx_power = 127,
    };
    ble_gap_ext_adv_remove(ADV_INSTANCE);
    rc = ble_gap_ext_adv_configure(ADV_INSTANCE, &p, NULL, gap_event, NULL);
    if (rc == 0) {
        rc = ble_gap_ext_adv_set_data(ADV_INSTANCE, ble_hs_mbuf_from_flat(s_adv, sizeof(s_adv)));
    }
    if (rc == 0) {
        rc = ble_gap_ext_adv_rsp_set_data(ADV_INSTANCE, ble_hs_mbuf_from_flat(rsp, name_len + 2));
    }
    if (rc == 0) {
        /* duration in 10 ms units, 0 = forever */
        rc = ble_gap_ext_adv_start(ADV_INSTANCE, s_pairing ? CONFIG_SRR_TV_PAIR_S * 100 : 0, 0);
    }
#else
    struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = s_pairing ? BLE_GAP_DISC_MODE_LTD : BLE_GAP_DISC_MODE_NON,
        .itvl_min = itvl,
        .itvl_max = itvl + itvl / 4,
    };
    rc = ble_gap_adv_set_data(s_adv, sizeof(s_adv));
    if (rc == 0) {
        rc = ble_gap_adv_rsp_set_data(rsp, name_len + 2);
    }
    if (rc == 0) {
        rc = ble_gap_adv_start(s_own_addr_type, NULL, s_pairing ? CONFIG_SRR_TV_PAIR_S * 1000 : BLE_HS_FOREVER,
                               &p, gap_event, NULL);
    }
#endif
    if (rc != 0) {
        ESP_LOGE(TAG, "advertising failed; rc=%d", rc);
        return;
    }
    if (s_pairing) {
        ESP_LOGI(TAG, "PAIRING MODE for %d s: advertising as \"%s\". The TV only connects while its "
                 "remote pairing search runs (XF86BTPairing key, REMOTE_PROTO.md §2.5)",
                 CONFIG_SRR_TV_PAIR_S, CONFIG_SRR_TV_NAME);
    } else {
        ESP_LOGI(TAG, "advertising for the bonded TV %s to reconnect", fmt_addr(&s_tv));
    }
}

static void pairing_end(void)
{
    s_pairing = false;
    led_set_pairing(false);
}

/* Pairing mode drops the TV's keys so it pairs from scratch: a bonded TV
 * reuses its cached PnP ID and GATT layout and never reads them again */
static void forget_tv_bond(void)
{
    if (!s_have_tv) {
        return;
    }
    ESP_LOGI(TAG, "forgetting the bond with TV %s so it pairs from scratch", fmt_addr(&s_tv));
    ble_store_util_delete_peer(&s_tv);
    tv_forget();
}

static void on_pair_request(struct ble_npl_event *ev)
{
    if (s_pairing) {
        ESP_LOGI(TAG, "already in pairing mode");
        return;
    }
    if (!s_remote_up) {
        ESP_LOGW(TAG, "connect the remote first (press a key on it), then press BOOT again");
        return;
    }
    s_pairing = true;
    led_set_pairing(true);
    if (tv_connected()) {
        ESP_LOGI(TAG, "pairing requested: dropping the current TV link");
        s_forget_on_disconnect = true;
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        return; /* the disconnect forgets the bond and restarts advertising */
    }
    forget_tv_bond();
    adv_start();
}

void tv_set_remote_up(bool up)
{
    s_remote_up = up;
    if (up) {
        adv_start();
        return;
    }
    adv_stop();
    if (s_pairing) {
        ESP_LOGW(TAG, "remote disconnected: pairing mode stopped");
        pairing_end();
    }
}

void tv_pairing_request(void)
{
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_pair_ev);
}

/* ------------------------------------------------------------------------ */
/* GATT server                                                              */
/* ------------------------------------------------------------------------ */

static tv_report_t *report_by_handle(uint16_t handle)
{
    for (int i = 0; i < N_REPORTS; i++) {
        if (s_reports[i].handle == handle) {
            return &s_reports[i];
        }
    }
    return NULL;
}

static int append(struct ble_gatt_access_ctxt *ctxt, const void *data, uint16_t len)
{
    return os_mbuf_append(ctxt->om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int report_access(uint16_t conn, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    tv_report_t *r = arg;
    uint8_t buf[MAX_REPORT_LEN];
    uint16_t len = 0;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_DSC: /* Report Reference */
        ESP_LOGI(TAG, "TV reads the Report Reference of report 0x%02x type %u", r->id, r->type);
        return append(ctxt, (uint8_t[]){r->id, r->type}, 2);
    case BLE_GATT_ACCESS_OP_READ_CHR:
        ESP_LOGI(TAG, "TV reads report 0x%02x type %u", r->id, r->type);
        return append(ctxt, r->value, r->len);
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
        ESP_LOGI(TAG, "TV->RC report 0x%02x type %u len=%u: %s", r->id, r->type, len, hex_line(buf, len));
        if (r->id == VOICE_REPORT_ID && r->type == 3) {
            voice_relay_cmd(buf, len);
        }
        return 0;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static int const_access(uint16_t conn, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const const_attr_t *a = arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    ESP_LOGI(TAG, "TV reads %s", a->name);
    return append(ctxt, a->data, a->len);
}

/* Service Changed is indicate-only: never read or written */
static int sc_access(uint16_t conn, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

/* The remote's two vendor services: the TV's writes are only logged */
static int vendor_access(uint16_t conn, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t buf[MAX_REPORT_LEN];
    uint16_t len = 0;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
        ESP_LOGW(TAG, "TV writes vendor char %s len=%u: %s (not forwarded)", (const char *)arg, len,
                 hex_line(buf, len));
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int attr_access(uint16_t conn, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    static const char *const names[] = {
        [ATTR_REPORT_MAP] = "Report Map", [ATTR_HID_INFO] = "HID Info", [ATTR_CONTROL_POINT] = "Control Point",
        [ATTR_PROTOCOL_MODE] = "Protocol Mode", [ATTR_BATTERY] = "Battery", [ATTR_PNP_ID] = "PnP ID",
    };
    uint8_t buf[8];
    uint16_t len = 0;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        ESP_LOGI(TAG, "TV reads %s", names[(attr_t)(intptr_t)arg]);
    }
    switch ((attr_t)(intptr_t)arg) {
    case ATTR_REPORT_MAP:
        return append(ctxt, REPORT_MAP, sizeof(REPORT_MAP));
    case ATTR_HID_INFO:
        return append(ctxt, HID_INFO, sizeof(HID_INFO));
    case ATTR_CONTROL_POINT:
        ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
        ESP_LOGI(TAG, "TV writes HID Control Point: %s", hex_line(buf, len));
        return 0;
    case ATTR_PROTOCOL_MODE:
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
            ESP_LOGI(TAG, "TV sets protocol mode: %s", hex_line(buf, len));
            if (len) {
                s_protocol_mode = buf[0];
            }
            return 0;
        }
        return append(ctxt, &s_protocol_mode, 1);
    case ATTR_BATTERY:
        return append(ctxt, &s_battery, 1);
    case ATTR_PNP_ID:
        return append(ctxt, s_pnp, sizeof(s_pnp));
    }
    return BLE_ATT_ERR_UNLIKELY;
}

#define ENC_R  (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC)
#define ENC_W  (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC)
#define ENC_WN (BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC)
#define ATTR(a) ((void *)(intptr_t)(a))

static struct ble_gatt_dsc_def s_report_refs[N_REPORTS][2];

static struct ble_gatt_chr_def s_hid_chrs[] = {
    {.uuid = BLE_UUID16_DECLARE(0x2a4a), .access_cb = attr_access, .arg = ATTR(ATTR_HID_INFO),
     .val_handle = &s_hid_info_handle, .flags = ENC_R},
    {.uuid = BLE_UUID16_DECLARE(0x2a4c), .access_cb = attr_access, .arg = ATTR(ATTR_CONTROL_POINT), .flags = ENC_WN},
    {.uuid = BLE_UUID16_DECLARE(0x2a4e), .access_cb = attr_access, .arg = ATTR(ATTR_PROTOCOL_MODE),
     .flags = ENC_R | ENC_WN},
    {.uuid = BLE_UUID16_DECLARE(0x2a4b), .access_cb = attr_access, .arg = ATTR(ATTR_REPORT_MAP),
     .val_handle = &s_report_map_handle, .flags = ENC_R},
    /* Report characteristics, filled in by tv_init() */
    {0}, {0}, {0}, {0},
    {0},
};
#define FIRST_REPORT_CHR 4

/* The remote's full GATT table, in its order, so that every handle up to
 * 0x0042 is the same as on the remote (REMOTE_PROTO.md §3). tv_init() checks
 * the handles. Only the 9e5d1e47 service differs: the remote puts it at
 * 0xff00, here it follows at 0x0043 */
static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1801),
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = BLE_UUID16_DECLARE(0x2a05), .access_cb = sc_access, .val_handle = &s_sc_handle,
             .flags = BLE_GATT_CHR_F_INDICATE},
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1800),
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = BLE_UUID16_DECLARE(0x2a00), .access_cb = const_access,
             .arg = CONST_STR("Device Name", CONFIG_SRR_TV_NAME), .flags = BLE_GATT_CHR_F_READ},
            {.uuid = BLE_UUID16_DECLARE(0x2a01), .access_cb = const_access,
             .arg = CONST_BIN("Appearance", 0x80, 0x01), .flags = BLE_GATT_CHR_F_READ},
            {.uuid = BLE_UUID16_DECLARE(0x2a04), .access_cb = const_access,
             .arg = CONST_BIN("Preferred Conn Params", 0x10, 0x00, 0x10, 0x00, 0x63, 0x00, 0x2a, 0x03),
             .flags = BLE_GATT_CHR_F_READ},
            /* Central Address Resolution: the remote returns an empty value */
            {.uuid = BLE_UUID16_DECLARE(0x2aa6), .access_cb = const_access, .arg = CONST_STR("Central Addr Res", ""),
             .val_handle = &s_gap_car_handle, .flags = BLE_GATT_CHR_F_READ},
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180a),
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = BLE_UUID16_DECLARE(0x2a29), .access_cb = const_access,
             .arg = CONST_STR("Manufacturer", "Realtek BT"), .flags = BLE_GATT_CHR_F_READ},
            {.uuid = BLE_UUID16_DECLARE(0x2a24), .access_cb = const_access,
             .arg = CONST_STR("Model", "Model Nbr 0.9"), .flags = BLE_GATT_CHR_F_READ},
            {.uuid = BLE_UUID16_DECLARE(0x2a25), .access_cb = const_access,
             .arg = CONST_STR("Serial", "RTKBeeSerialNum"), .flags = BLE_GATT_CHR_F_READ},
            {.uuid = BLE_UUID16_DECLARE(0x2a27), .access_cb = const_access,
             .arg = CONST_STR("Hardware Rev", "RTKBeeHardwareRev"), .flags = BLE_GATT_CHR_F_READ},
            {.uuid = BLE_UUID16_DECLARE(0x2a26), .access_cb = const_access,
             .arg = CONST_STR("Firmware Rev", "RTKBeeFirmwareRev"), .flags = BLE_GATT_CHR_F_READ},
            {.uuid = BLE_UUID16_DECLARE(0x2a28), .access_cb = const_access,
             .arg = CONST_STR("Software Rev", "RTKBeeSoftwareRev"), .flags = BLE_GATT_CHR_F_READ},
            {.uuid = BLE_UUID16_DECLARE(0x2a23), .access_cb = const_access,
             .arg = CONST_BIN("System ID", 0x00, 0x01, 0x02, 0x00, 0x00, 0x03, 0x04, 0x05),
             .flags = BLE_GATT_CHR_F_READ},
            {.uuid = BLE_UUID16_DECLARE(0x2a2a), .access_cb = const_access,
             .arg = CONST_STR("IEEE Cert", "RTKBeeIEEEDatalist"), .flags = BLE_GATT_CHR_F_READ},
            {.uuid = BLE_UUID16_DECLARE(0x2a50), .access_cb = attr_access, .arg = ATTR(ATTR_PNP_ID),
             .flags = BLE_GATT_CHR_F_READ},
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180f),
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = BLE_UUID16_DECLARE(0x2a19), .access_cb = attr_access, .arg = ATTR(ATTR_BATTERY),
             .val_handle = &s_battery_handle, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY},
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),
        .characteristics = s_hid_chrs,
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID128_DECLARE(0x12, 0xa2, 0x4d, 0x2e, 0xfe, 0x14, 0x48, 0x8e, 0x93, 0xd2, 0x17, 0x3c, 0xff,
                                    0xd1, 0x00, 0x00), /* 0000d1ff-3c17-d293-8e48-14fe2e4da212 */
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = BLE_UUID16_DECLARE(0xa001), .access_cb = vendor_access, .arg = "d1ff/A001",
             .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY},
            {.uuid = BLE_UUID16_DECLARE(0xa002), .access_cb = vendor_access, .arg = "d1ff/A002",
             .val_handle = &s_vendor_a002_handle, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID128_DECLARE(0x6f, 0x38, 0xa1, 0x38, 0xad, 0x82, 0x35, 0x86, 0xa0, 0x43, 0x13, 0x5c, 0x47,
                                    0x1e, 0x5d, 0x9e), /* 9e5d1e47-5c13-43a0-8635-82ad38a1386f */
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = BLE_UUID128_DECLARE(0x6b, 0x66, 0x6c, 0x08, 0x0a, 0x57, 0x8e, 0x83, 0x99, 0x4e, 0xa7, 0xf7, 0xbf,
                                         0x50, 0xdd, 0xe3), /* e3dd50bf-f7a7-4e99-838e-570a086c666b */
             .access_cb = vendor_access, .arg = "9e5d/e3dd50bf",
             .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE},
            {.uuid = BLE_UUID128_DECLARE(0x36, 0xfe, 0x2e, 0xe7, 0x09, 0x24, 0x4f, 0xb7, 0x91, 0x40, 0x61, 0xd9, 0x7a,
                                         0x6c, 0xe8, 0x92), /* 92e86c7a-d961-4091-b74f-2409e72efe36 */
             .access_cb = vendor_access, .arg = "9e5d/92e86c7a",
             .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
            {0},
        },
    },
    {0},
};

/* Value handles on the remote (REMOTE_PROTO.md §3) */
static void check_layout(void)
{
    const struct {
        const char *name;
        uint16_t got, want;
    } h[] = {
        {"Service Changed", s_sc_handle, 0x0003},
        {"Central Addr Res", s_gap_car_handle, 0x000d},
        {"Battery", s_battery_handle, 0x0023},
        {"HID Info", s_hid_info_handle, 0x0027},
        {"Report Map", s_report_map_handle, 0x002d},
        {"Report F7 in", s_reports[0].handle, 0x002f},
        {"Report F6 in", s_reports[1].handle, 0x0033},
        {"Report FB feature", s_reports[2].handle, 0x0037},
        {"Report FB in", s_reports[3].handle, 0x003a},
        {"d1ff/A002", s_vendor_a002_handle, 0x0042},
    };
    int bad = 0;
    /* Lookups by UUID go through the same UUID pointers the ATT responses
     * are built from: a dangling one (stack compound literal) fails here */
    uint16_t chr_h = 0, dsc_h = 0;
    if (ble_gatts_find_chr(BLE_UUID16_DECLARE(0x1812), BLE_UUID16_DECLARE(0x2a4d), NULL, &chr_h) != 0 ||
        chr_h != 0x002f ||
        ble_gatts_find_dsc(BLE_UUID16_DECLARE(0x1812), BLE_UUID16_DECLARE(0x2a4d), BLE_UUID16_DECLARE(0x2908),
                           &dsc_h) != 0 ||
        dsc_h != 0x0031) {
        ESP_LOGE(TAG, "GATT layout: lookup by UUID gives Report h=0x%04x, Report Reference h=0x%04x "
                 "(want 0x002f, 0x0031): broken UUID pointers?", chr_h, dsc_h);
        bad++;
    }
    for (int i = 0; i < sizeof(h) / sizeof(h[0]); i++) {
        if (h[i].got != h[i].want) {
            ESP_LOGE(TAG, "GATT layout: %s at h=0x%04x, the remote has it at h=0x%04x", h[i].name, h[i].got,
                     h[i].want);
            bad++;
        }
    }
    if (!bad) {
        ESP_LOGI(TAG, "GATT layout matches the remote's handles (F7 report h=0x%04x)", s_reports[0].handle);
    }
}

/* ------------------------------------------------------------------------ */
/* relay                                                                    */
/* ------------------------------------------------------------------------ */

void tv_relay_report(uint8_t report_id, uint8_t report_type, const uint8_t *data, uint16_t len)
{
    for (int i = 0; i < N_REPORTS; i++) {
        tv_report_t *r = &s_reports[i];
        if (r->id != report_id || r->type != report_type) {
            continue;
        }
        r->len = len < MAX_REPORT_LEN ? len : MAX_REPORT_LEN;
        memcpy(r->value, data, r->len);
        /* The TV never writes the CCCDs (seen live), so notify once the link
         * is encrypted, subscribed or not */
        if (!tv_connected() || !s_ready) {
            return;
        }
        if (!s_logged_relay) {
            ESP_LOGI(TAG, "relaying reports to the TV (first: 0x%02x, TV %s subscribed)", report_id,
                     r->notify ? "is" : "is not");
            s_logged_relay = true;
        }
        uint16_t max = ble_att_mtu(s_conn) - 3;
        if (len > max && !s_warned_mtu) {
            ESP_LOGW(TAG, "report 0x%02x (%u bytes) is cut to the TV's MTU payload %u", report_id, len, max);
            s_warned_mtu = true;
        }
        int rc = ble_gatts_notify_custom(s_conn, r->handle, ble_hs_mbuf_from_flat(data, len < max ? len : max));
        if (rc != 0) {
            ESP_LOGW(TAG, "notify report 0x%02x failed; rc=%d", report_id, rc);
        }
        return;
    }
}

void tv_relay_battery(uint8_t level)
{
    s_battery = level;
    if (tv_connected() && s_ready && s_battery_notify) {
        ble_gatts_notify_custom(s_conn, s_battery_handle, ble_hs_mbuf_from_flat(&level, 1));
    }
}

/* ------------------------------------------------------------------------ */
/* GAP events of the TV link                                                */
/* ------------------------------------------------------------------------ */

static void on_subscribe(uint16_t handle, bool notify, bool indicate)
{
    /* Service Changed is never indicated: an indication during the TV's HID
     * setup may be what corrupts its GATT cache (REMOTE_PROTO.md §6.5). A
     * changed layout gets a new CONFIG_SRR_OWN_ADDR instead */
    if (handle == s_sc_handle) {
        return;
    }
    tv_report_t *r = report_by_handle(handle);
    if (r) {
        r->notify = notify;
        ESP_LOGI(TAG, "TV %s report 0x%02x", notify ? "subscribed to" : "unsubscribed from", r->id);
    } else if (handle == s_battery_handle) {
        s_battery_notify = notify;
    }
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *error, uint16_t mtu, void *arg)
{
    if (error->status != 0) {
        ESP_LOGW(TAG, "MTU exchange with the TV failed; status=0x%x", error->status);
    } else {
        ESP_LOGI(TAG, "MTU %u agreed with the TV (our request)", mtu);
    }
    return 0;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "TV connection failed; status=0x%x", event->connect.status);
            adv_start();
            return 0;
        }
        s_conn = event->connect.conn_handle;
        s_warned_mtu = false;
        s_ready = false;
        s_logged_relay = false;
        ble_gap_conn_find(s_conn, &desc);
        ESP_LOGI(TAG, "TV connected: %s, itvl=%u latency=%u timeout=%u", fmt_addr(&desc.peer_id_addr),
                 desc.conn_itvl, desc.conn_latency, desc.supervision_timeout);
        /* Ask for encryption: pairs on first contact, or uses the bond */
        ble_gap_security_initiate(s_conn);
        /* Exchange the MTU now, like the real remote. Otherwise the TV starts
         * its own exchange ~2 s after connecting, right after the Report Map
         * read at MTU 23, and its HID setup never reads a Report Reference
         * (its GATT client drops a command issued while another is pending) */
        rc = ble_gattc_exchange_mtu(s_conn, on_mtu, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "MTU exchange with the TV failed to start; rc=%d", rc);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "TV disconnected; reason=0x%x", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_ready = false;
        for (int i = 0; i < N_REPORTS; i++) {
            s_reports[i].notify = false;
        }
        s_battery_notify = false;
        if (s_forget_on_disconnect) {
            s_forget_on_disconnect = false;
            forget_tv_bond();
        }
        adv_start();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        /* Also reported (reason 0) just before BLE_GAP_EVENT_CONNECT when a TV connects */
        if (event->adv_complete.reason == BLE_HS_ETIMEOUT && s_pairing) {
            ESP_LOGI(TAG, "pairing mode timed out");
            pairing_end();
            adv_start();
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        if (event->enc_change.status != 0) {
            ESP_LOGW(TAG, "TV link encryption failed; status=0x%x", event->enc_change.status);
            return 0;
        }
        if (!desc.sec_state.bonded) {
            ESP_LOGW(TAG, "TV link encrypted without bonding");
            return 0;
        }
        if (tv_is_peer(&desc.peer_id_addr)) {
            ESP_LOGI(TAG, "TV link encrypted (bonded TV %s)", fmt_addr(&desc.peer_id_addr));
            s_ready = true;
        } else if (s_pairing) {
            ble_addr_t id = identity(&desc.peer_id_addr);
            if (s_have_tv) {
                ESP_LOGI(TAG, "forgetting the previous TV %s", fmt_addr(&s_tv));
                ble_store_util_delete_peer(&s_tv);
            }
            s_tv = id;
            s_have_tv = true;
            tv_save(&s_tv);
            ESP_LOGI(TAG, "PAIRED with TV %s", fmt_addr(&s_tv));
            s_ready = true;
            log_bonds("after TV pairing");
        } else {
            ESP_LOGW(TAG, "%s is not the bonded TV and pairing mode is off: dropping it",
                     fmt_addr(&desc.peer_id_addr));
            ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        pairing_end();
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* The TV lost its key for us and pairs again */
        ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        ESP_LOGW(TAG, "TV asks for a passkey (action %d); we are Just Works only", event->passkey.params.action);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "TV CCCD h=0x%04x notify=%d indicate=%d reason=%d", event->subscribe.attr_handle,
                 event->subscribe.cur_notify, event->subscribe.cur_indicate, event->subscribe.reason);
        on_subscribe(event->subscribe.attr_handle, event->subscribe.cur_notify, event->subscribe.cur_indicate);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "TV MTU %u", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "TV link params itvl=%u latency=%u timeout=%u", desc.conn_itvl, desc.conn_latency,
                     desc.supervision_timeout);
        }
        return 0;

    default:
        return 0;
    }
}

/* ------------------------------------------------------------------------ */
/* startup                                                                  */
/* ------------------------------------------------------------------------ */

void tv_init(void)
{
    /* PnP ID: a fixed hex string (7 bytes), or derived from the remote's */
    const char *p = CONFIG_SRR_TV_PNP_ID;
    s_pnp_fixed = *p != 0;
    if (!s_pnp_fixed) {
        pnp_load();
    }
    for (int n = 0, hi = -1; *p && n < (int)sizeof(s_pnp); p++) {
        int v = hexval(*p);
        if (v < 0) {
            continue;
        }
        if (hi < 0) {
            hi = v;
        } else {
            s_pnp[n++] = (uint8_t)(hi << 4 | v);
            hi = -1;
        }
    }

    /* UUIDs used by the runtime-assigned report definitions below.
     * BLE_UUID16_DECLARE() in a runtime assignment creates a compound
     * literal on tv_init()'s stack; NimBLE's registration
     * (ble_gatts_start(), at host sync, after tv_init returned) stores the
     * POINTER, so the served ATT responses (Find Information, Read By
     * Type) would carry garbage — exactly the "format 0x02 / 128-bit
     * garbage" responses the TV cached, leaving its report table empty
     * ("no matching report"). Static objects fix the lifetime. */
    static ble_uuid16_t uuid_report_chr = BLE_UUID16_INIT(0x2a4d);
    static ble_uuid16_t uuid_report_ref = BLE_UUID16_INIT(0x2908);

    for (int i = 0; i < N_REPORTS; i++) {
        tv_report_t *r = &s_reports[i];
        s_report_refs[i][0] = (struct ble_gatt_dsc_def){
            .uuid = &uuid_report_ref.u,
            .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
            .access_cb = report_access,
            .arg = r,
        };
        s_hid_chrs[FIRST_REPORT_CHR + i] = (struct ble_gatt_chr_def){
            .uuid = &uuid_report_chr.u,
            .access_cb = report_access,
            .arg = r,
            .descriptors = s_report_refs[i],
            .val_handle = &r->handle,
            /* as on the remote: input reports are also writable */
            .flags = r->type == 3 ? ENC_R | ENC_W | ENC_WN : ENC_R | ENC_W | BLE_GATT_CHR_F_NOTIFY,
        };
    }

    /* No ble_svc_gap_init()/ble_svc_gatt_init(): s_svcs has its own 0x1801 and
     * 0x1800 in the remote's order (NimBLE's own put GAP first and add
     * characteristics the remote doesn't have) */
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_svcs);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT server setup failed; rc=%d", rc);
    }
    ble_npl_event_init(&s_pair_ev, on_pair_request, NULL);
    tv_load();
}

void tv_start(void)
{
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    s_synced = true;
    check_layout();
    if (tv_bonded()) {
        ESP_LOGI(TAG, "bonded TV: %s", fmt_addr(&s_tv));
    } else {
        ESP_LOGI(TAG, "no TV bonded yet: once the remote is connected, press BOOT to pair with the TV");
    }
    adv_start();
}

#else /* !CONFIG_SRR_TV_RELAY */

void tv_init(void) {}
void tv_start(void) {}
void tv_pairing_request(void)
{
    ESP_LOGI(TAG, "TV relay is disabled (CONFIG_SRR_TV_RELAY)");
}
bool tv_connected(void)
{
    return false;
}
void tv_forget(void) {}
void tv_set_remote_up(bool up) {}
bool tv_is_peer(const ble_addr_t *addr)
{
    return false;
}
void tv_relay_report(uint8_t report_id, uint8_t report_type, const uint8_t *data, uint16_t len) {}
void tv_relay_battery(uint8_t level) {}
void tv_set_remote_pnp(const uint8_t *pnp, uint16_t len) {}

#endif
