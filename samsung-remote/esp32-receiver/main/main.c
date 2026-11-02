/*
 * Samsung Smart Remote receiver: the ESP32 takes the TV's place as BLE
 * central, bonds with the remote (Just Works), dumps its GATT table and HID
 * Report Map, subscribes to every notifying characteristic and decodes key
 * presses. It also poses as a remote to the TV and relays both ways (tv.c).
 * See ../README.md and ../../../REMOTE_PROTO.md.
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "esp_central.h"
#include "hid_parser.h"
#include "remote_events.h"
#include "samsung_keys.h"
#include "voice.h"
#include "led.h"
#include "tv.h"

static const char *TAG = "srr";

#define SAMSUNG_COMPANY_ID    0x0075
#define SAMSUNG_REMOTE_NAME   "Smart Control"

#define UUID_SVC_HID          0x1812
#define UUID_CHR_REPORT       0x2a4d
#define UUID_CHR_REPORT_MAP   0x2a4b
#define UUID_CHR_PROTO_MODE   0x2a4e
#define UUID_CHR_BOOT_KBD_IN  0x2a22
#define UUID_CHR_BOOT_MOUSE   0x2a33
#define UUID_CHR_BATTERY      0x2a19
#define UUID_DSC_CCCD         0x2902
#define UUID_DSC_REPORT_REF   0x2908

#define MAX_HID_SVCS          2
#define MAX_SLOTS             32
#define MAX_OPS               128
#define MAX_SEEN              64
#define MAX_BONDS             4
#define TOP_N                 5

void ble_store_config_init(void);

/* A characteristic we receive notifications from */
typedef struct {
    uint16_t val_handle;
    uint16_t cccd_handle;
    uint16_t uuid16;
    ble_uuid_any_t uuid;
    uint8_t props;
    int8_t hid_idx;      /* index into s_hid, -1 if not in a HID service */
    uint8_t report_id;   /* from the Report Reference descriptor */
    uint8_t report_type; /* 1 input, 2 output, 3 feature */
    uint8_t n_prev;
    uint32_t prev[HID_MAX_ACTIVE];
} notify_slot_t;

typedef enum {
    OP_READ_VALUE,
    OP_READ_REPORT_MAP,
    OP_READ_REPORT_REF,
    OP_SET_REPORT_PROTOCOL,
    OP_ENABLE_CCCD,
} op_kind_t;

typedef struct {
    op_kind_t kind;
    uint16_t handle;
    int16_t idx; /* slot index or HID service index, depending on kind */
    uint16_t val_handle; /* OP_READ_REPORT_REF: the Report characteristic it describes */
    ble_uuid_any_t uuid;
} gatt_op_t;

/* What we have seen of an advertiser so far (adv + scan response) */
typedef struct {
    bool used;
    bool logged;
    bool samsung;
    bool hid_hint;
    bool discoverable;
    bool dumped_adv;
    bool dumped_rsp;
    uint32_t win;
    ble_addr_t addr;
} adv_seen_t;

/* One advertising report, normalised from legacy and extended scan events */
typedef struct {
    ble_addr_t addr;
    ble_addr_t direct_addr;
    int8_t rssi;
    bool connectable;
    bool directed;
    bool scan_rsp;
    bool legacy;
    bool complete;
    uint8_t prim_phy;
    uint8_t sec_phy;
    uint8_t sid;
    uint16_t len;
    const uint8_t *data;
} adv_report_t;

/* Strongest advertisers of the current heartbeat window */
typedef struct {
    ble_addr_t addr;
    int8_t rssi;
    uint16_t reports;
    bool connectable;
    bool extended;
    uint16_t company;
    uint16_t appearance;
    char name[20];
} top_dev_t;

static uint8_t s_own_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool s_connecting;
static bool s_gatt_started;
static bool s_ready; /* GATT setup finished: report IDs known */

/* Notifications that arrive before setup is done (the remote keeps our
 * subscriptions in its bond and sends the wake-up key press immediately) */
typedef struct {
    uint16_t handle;
    uint8_t len;
    uint8_t data[32];
} pending_notify_t;
static pending_notify_t s_pending[48];
static int s_n_pending;
static int s_n_dropped;

/* The remote's power-saving connection parameters, applied after setup */
static struct ble_gap_upd_params s_deferred_params;
static bool s_have_deferred_params;

static ble_addr_t s_bonds[MAX_BONDS];
static int s_n_bonds;

static adv_seen_t s_seen[MAX_SEEN];
static int s_seen_next;

static struct {
    uint32_t win;
    uint32_t reports;
    uint32_t unique;
    uint32_t connectable;
    uint32_t extended;
    uint32_t samsung;
    int n_top;
    top_dev_t top[TOP_N];
} s_stats = {.win = 1}; /* seen-cache entries start at win 0 */
static struct ble_npl_callout s_hb_co;

static hid_map_t s_hid[MAX_HID_SVCS];
static int s_n_hid;
static notify_slot_t s_slots[MAX_SLOTS];
static int s_n_slots;

static gatt_op_t s_ops[MAX_OPS];
static int s_n_ops;
static int s_op_idx;
static uint8_t s_rd_buf[1024];
static uint16_t s_rd_len;

static int gap_event(struct ble_gap_event *event, void *arg);
static void ops_next(void);
static void on_notify_data(uint16_t handle, const uint8_t *data, uint16_t len);

/* ------------------------------------------------------------------------ */
/* helpers                                                                  */
/* ------------------------------------------------------------------------ */

static const char *fmt_addr(const ble_addr_t *a)
{
    static char buf[2][28];
    static int which;
    static const char *types[] = {"pub", "rnd", "pub-id", "rnd-id"};
    char *s = buf[which ^= 1];
    snprintf(s, sizeof(buf[0]), "%02x:%02x:%02x:%02x:%02x:%02x (%s)",
             a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0],
             a->type < 4 ? types[a->type] : "?");
    return s;
}

static uint16_t uuid16_of(const ble_uuid_t *u)
{
    return u->type == BLE_UUID_TYPE_16 ? BLE_UUID16(u)->value : 0;
}

static const char *uuid_str(const ble_uuid_t *u)
{
    static char buf[2][BLE_UUID_STR_LEN];
    static int which;
    return ble_uuid_to_str(u, buf[which ^= 1]);
}

static void log_value(const char *label, const uint8_t *d, uint16_t len)
{
    char ascii[65];
    int n = 0;
    bool printable = len > 0;
    for (int i = 0; i < len && n < (int)sizeof(ascii) - 1; i++) {
        if (d[i] == 0 && i == len - 1) {
            break; /* trailing NUL */
        }
        if (d[i] < 0x20 || d[i] > 0x7e) {
            printable = false;
            break;
        }
        ascii[n++] = (char)d[i];
    }
    ascii[n] = 0;
    if (printable) {
        ESP_LOGI(TAG, "  %s = \"%s\"", label, ascii);
    } else {
        ESP_LOGI(TAG, "  %s (%u bytes):", label, len);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, d, len, ESP_LOG_INFO);
    }
}

static const char *gap_event_name(uint8_t t)
{
    static const char *names[] = {
        "CONNECT", "DISCONNECT", "?", "CONN_UPDATE", "CONN_UPDATE_REQ", "L2CAP_UPDATE_REQ",
        "TERM_FAILURE", "DISC", "DISC_COMPLETE", "ADV_COMPLETE", "ENC_CHANGE", "PASSKEY_ACTION",
        "NOTIFY_RX", "NOTIFY_TX", "SUBSCRIBE", "MTU", "IDENTITY_RESOLVED", "REPEAT_PAIRING",
        "PHY_UPDATE_COMPLETE", "EXT_DISC", "PERIODIC_SYNC", "PERIODIC_REPORT", "PERIODIC_SYNC_LOST",
        "SCAN_REQ_RCVD", "PERIODIC_TRANSFER", "PATHLOSS_THRESHOLD", "TRANSMIT_POWER",
        "PAIRING_COMPLETE", "SUBRATE_CHANGE", "VS_HCI", "BIGINFO_REPORT", "REATTEMPT_COUNT",
        "AUTHORIZE", "TEST_UPDATE", "DATA_LEN_CHG",
    };
    return t < sizeof(names) / sizeof(names[0]) ? names[t] : "?";
}

/* Decode the NimBLE status codes seen in connect / disconnect / security events */
static const char *err_name(int status)
{
    switch (status) {
    case 0: return "ok";
    case BLE_HS_ETIMEOUT: return "timeout (remote stopped advertising?)";
    case BLE_HS_ENOTCONN: return "not connected";
    case BLE_HS_EAUTHEN: return "authentication failed";
    case BLE_HS_EENCRYPT: return "encryption failed";
    case BLE_HS_HCI_ERR(BLE_ERR_CONN_SPVN_TMO): return "supervision timeout (out of range / remote slept)";
    case BLE_HS_HCI_ERR(BLE_ERR_REM_USER_CONN_TERM): return "remote terminated";
    case BLE_HS_HCI_ERR(BLE_ERR_CONN_TERM_LOCAL): return "terminated by us";
    case BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING): return "remote has no key for us";
    case BLE_HS_HCI_ERR(BLE_ERR_CONN_ESTABLISHMENT): return "failed to establish";
    case BLE_HS_HCI_ERR(BLE_ERR_AUTH_FAIL): return "HCI authentication failure";
    case BLE_HS_HCI_ERR(BLE_ERR_LMP_LL_RSP_TMO): return "LL response timeout";
    case BLE_HS_HCI_ERR(BLE_ERR_INSTANT_PASSED): return "instant passed";
    case BLE_HS_HCI_ERR(BLE_ERR_UNSUPP_REM_FEATURE): return "unsupported remote feature";
    case BLE_HS_HCI_ERR(BLE_ERR_DIFF_TRANS_COLL): return "transaction collision";
    case BLE_HS_SM_US_ERR(BLE_SM_ERR_PAIR_NOT_SUPP): return "SM(us): pairing not supported";
    case BLE_HS_SM_US_ERR(BLE_SM_ERR_UNSPECIFIED): return "SM(us): unspecified";
    case BLE_HS_SM_US_ERR(BLE_SM_ERR_AUTHREQ): return "SM(us): auth requirements";
    case BLE_HS_SM_US_ERR(BLE_SM_ERR_CONFIRM_MISMATCH): return "SM(us): confirm mismatch";
    case BLE_HS_SM_PEER_ERR(BLE_SM_ERR_PAIR_NOT_SUPP): return "SM(remote): pairing not supported";
    case BLE_HS_SM_PEER_ERR(BLE_SM_ERR_UNSPECIFIED): return "SM(remote): unspecified";
    case BLE_HS_SM_PEER_ERR(BLE_SM_ERR_AUTHREQ): return "SM(remote): auth requirements";
    case BLE_HS_SM_PEER_ERR(BLE_SM_ERR_ENC_KEY_SZ): return "SM(remote): key size";
    case BLE_HS_SM_PEER_ERR(BLE_SM_ERR_REPEATED): return "SM(remote): repeated attempts";
    default:
        if (status >= BLE_HS_ERR_HCI_BASE && status < BLE_HS_ERR_HCI_BASE + 0x100) return "HCI error";
        if (status >= BLE_HS_ERR_SM_US_BASE && status < BLE_HS_ERR_SM_US_BASE + 0x100) return "SM error (us)";
        if (status >= BLE_HS_ERR_SM_PEER_BASE && status < BLE_HS_ERR_SM_PEER_BASE + 0x100) return "SM error (remote)";
        if (status >= BLE_HS_ERR_ATT_BASE && status < BLE_HS_ERR_ATT_BASE + 0x100) return "ATT error";
        return "host error";
    }
}

static void refresh_bonds(void)
{
    s_n_bonds = 0;
    int rc = ble_store_util_bonded_peers(s_bonds, &s_n_bonds, MAX_BONDS);
    if (rc != 0) {
        s_n_bonds = 0;
    }
}

static bool is_bonded(const ble_addr_t *addr)
{
    ble_addr_t id = *addr;
    if (id.type == BLE_ADDR_PUBLIC_ID || id.type == BLE_ADDR_RANDOM_ID) {
        id.type -= 2; /* controller resolved an RPA to this identity address */
    }
    for (int i = 0; i < s_n_bonds; i++) {
        if (ble_addr_cmp(&id, &s_bonds[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* The bond of the remote: the store also holds the TV's when the relay is paired */
static const ble_addr_t *remote_bond(void)
{
    for (int i = 0; i < s_n_bonds; i++) {
        if (!tv_is_peer(&s_bonds[i])) {
            return &s_bonds[i];
        }
    }
    return NULL;
}

/* Keep only the bond with `keep` (and the TV's): the remote itself also holds just one */
static void drop_other_bonds(const ble_addr_t *keep)
{
    refresh_bonds();
    for (int i = 0; i < s_n_bonds; i++) {
        if (ble_addr_cmp(&s_bonds[i], keep) != 0 && !tv_is_peer(&s_bonds[i])) {
            ESP_LOGI(TAG, "Forgetting old bond %s", fmt_addr(&s_bonds[i]));
            ble_store_util_delete_peer(&s_bonds[i]);
        }
    }
    refresh_bonds();
}

static void reset_conn_state(void)
{
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_connecting = false;
    s_gatt_started = false;
    s_ready = false;
    s_n_pending = 0;
    s_n_dropped = 0;
    s_have_deferred_params = false;
    s_n_hid = 0;
    s_n_slots = 0;
    s_n_ops = 0;
    s_op_idx = 0;
    memset(s_hid, 0, sizeof(s_hid));
    memset(s_slots, 0, sizeof(s_slots));
}

/* ------------------------------------------------------------------------ */
/* scanning and connecting                                                  */
/* ------------------------------------------------------------------------ */

static void heartbeat_arm(void)
{
#if CONFIG_SRR_HEARTBEAT_S > 0
    ble_npl_callout_reset(&s_hb_co, ble_npl_time_ms_to_ticks32(CONFIG_SRR_HEARTBEAT_S * 1000));
#endif
}

static void scan_start(void)
{
    struct ble_gap_disc_params p = {0};

#if CONFIG_SRR_SKIP_REMOTE
    return; /* testing the TV side alone */
#endif

    /* Active scan for scan responses; no duplicate filtering because the
     * controller may otherwise drop the scan response of a seen device. */
    p.passive = 0;
    p.filter_duplicates = 0;
    /* 50% duty, to leave air time for the TV relay (tv.c), which advertises
     * and holds a second link at the same time */
    p.itvl = BLE_GAP_SCAN_ITVL_MS(100);
    p.window = BLE_GAP_SCAN_WIN_MS(50);

    memset(s_seen, 0, sizeof(s_seen));
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_disc failed; rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "Scan started: active, %s, no duplicate filter, debug dump >= %d dBm",
             MYNEWT_VAL(BLE_EXT_ADV) ? "legacy + extended adverts" : "legacy adverts only",
             CONFIG_SRR_DEBUG_ADV_RSSI);
    if (remote_bond()) {
        ESP_LOGI(TAG, "Scanning: waiting for bonded remote %s (press any key on it), "
                 "or any remote in pairing mode", fmt_addr(remote_bond()));
    } else {
        ESP_LOGI(TAG, "Scanning: hold Back + Play/Pause on the remote for 3 s to pair");
    }
    heartbeat_arm();
}

static void connect_to(const ble_addr_t *addr, const char *why)
{
    if (ble_gap_disc_active()) {
        ble_gap_disc_cancel();
    }
    ESP_LOGI(TAG, "Connecting to %s (%s)", fmt_addr(addr), why);
    int rc = ble_gap_connect(s_own_addr_type, addr, 10000, NULL, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect failed; rc=%d", rc);
        scan_start();
        return;
    }
    s_connecting = true;
}

static adv_seen_t *seen_entry(const ble_addr_t *addr)
{
    for (int i = 0; i < MAX_SEEN; i++) {
        if (s_seen[i].used && ble_addr_cmp(&s_seen[i].addr, addr) == 0) {
            return &s_seen[i];
        }
    }
    adv_seen_t *e = &s_seen[s_seen_next];
    s_seen_next = (s_seen_next + 1) % MAX_SEEN;
    memset(e, 0, sizeof(*e));
    e->used = true;
    e->addr = *addr;
    return e;
}

static bool appearance_is_hid(uint16_t a)
{
    uint16_t category = a >> 6;
    return category == 0x0f /* HID */ || category == 0x06 /* remote control */;
}

static const char *company_name(uint16_t id)
{
    switch (id) {
    case 0x0006: return "Microsoft";
    case 0x004c: return "Apple";
    case 0x0059: return "Nordic";
    case 0x005d: return "Realtek";
    case 0x0075: return "Samsung";
    case 0x00e0: return "Google";
    case 0x02e5: return "Espressif";
    case 0x038f: return "Xiaomi";
    case 0xffff: return "none";
    default:     return "?";
    }
}

static void debug_dump_adv(const adv_report_t *r, const struct ble_hs_adv_fields *f, bool parsed,
                           const char *why)
{
    ESP_LOGI(TAG, "ADV[%s] %s rssi=%d %s%s%s%s phy=%u/%u sid=%u len=%u%s", why, fmt_addr(&r->addr),
             r->rssi, r->connectable ? "conn " : "nonconn ", r->directed ? "directed " : "",
             r->scan_rsp ? "scan-rsp " : "", r->legacy ? "legacy" : "EXTENDED",
             r->prim_phy, r->sec_phy, r->sid, r->len, r->complete ? "" : " (data incomplete)");
    if (r->directed) {
        ESP_LOGI(TAG, "    directed to %s", fmt_addr(&r->direct_addr));
    }
    if (parsed) {
        char name[32] = "";
        char uuids[64] = "";
        int n = 0;
        if (f->name_len) {
            snprintf(name, sizeof(name), "%.*s", f->name_len, (const char *)f->name);
        }
        for (int i = 0; i < f->num_uuids16 && n < (int)sizeof(uuids) - 8; i++) {
            n += snprintf(uuids + n, sizeof(uuids) - n, "%s%04x", i ? "," : "", f->uuids16[i].value);
        }
        uint16_t company = f->mfg_data_len >= 2 ? get_le16(f->mfg_data) : 0xffff;
        ESP_LOGI(TAG, "    flags=0x%02x name=\"%s\" appearance=%s0x%04x uuid16=[%s] uuid32=%u uuid128=%u "
                 "company=0x%04x(%s) mfg_len=%u svc_data16_len=%u tx_pwr=%s%d",
                 f->flags, name, f->appearance_is_present ? "" : "(none)", f->appearance, uuids,
                 f->num_uuids32, f->num_uuids128, company, company_name(company), f->mfg_data_len,
                 f->svc_data_uuid16_len, f->tx_pwr_lvl_is_present ? "" : "(none)", f->tx_pwr_lvl);
    } else if (r->len) {
        ESP_LOGI(TAG, "    (payload did not parse as AD structures)");
    }
    if (r->len) {
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, r->data, r->len, ESP_LOG_INFO);
    }
}

static void stats_update(const adv_report_t *r, const struct ble_hs_adv_fields *f, bool parsed)
{
    s_stats.reports++;
    s_stats.connectable += r->connectable;
    s_stats.extended += !r->legacy;

    int i;
    for (i = 0; i < s_stats.n_top; i++) {
        if (ble_addr_cmp(&s_stats.top[i].addr, &r->addr) == 0) {
            break;
        }
    }
    if (i == s_stats.n_top) {
        if (s_stats.n_top < TOP_N) {
            i = s_stats.n_top++;
        } else {
            int weakest = 0;
            for (int k = 1; k < TOP_N; k++) {
                if (s_stats.top[k].rssi < s_stats.top[weakest].rssi) {
                    weakest = k;
                }
            }
            if (r->rssi <= s_stats.top[weakest].rssi) {
                return;
            }
            i = weakest;
        }
        memset(&s_stats.top[i], 0, sizeof(s_stats.top[i]));
        s_stats.top[i].addr = r->addr;
        s_stats.top[i].rssi = INT8_MIN;
        s_stats.top[i].company = 0xffff;
    }

    top_dev_t *t = &s_stats.top[i];
    if (r->rssi > t->rssi) {
        t->rssi = r->rssi;
    }
    t->reports++;
    t->connectable |= r->connectable;
    t->extended |= !r->legacy;
    if (parsed) {
        if (f->name_len) {
            snprintf(t->name, sizeof(t->name), "%.*s", f->name_len, (const char *)f->name);
        }
        if (f->mfg_data_len >= 2) {
            t->company = get_le16(f->mfg_data);
        }
        if (f->appearance_is_present) {
            t->appearance = f->appearance;
        }
    }
}

static void heartbeat(struct ble_npl_event *ev)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE && !s_connecting) {
        ESP_LOGI(TAG, "scan[%ds]: %lu reports from ~%lu devices (%lu connectable, %lu extended, "
                 "%lu Samsung)%s", CONFIG_SRR_HEARTBEAT_S, (unsigned long)s_stats.reports,
                 (unsigned long)s_stats.unique, (unsigned long)s_stats.connectable,
                 (unsigned long)s_stats.extended, (unsigned long)s_stats.samsung,
                 s_stats.reports ? ", strongest:" : "");
        if (!s_stats.reports) {
            ESP_LOGW(TAG, "scan: no advert received at all (disc active=%d)", ble_gap_disc_active());
        }
        /* sort strongest first */
        for (int a = 0; a < s_stats.n_top; a++) {
            for (int b = a + 1; b < s_stats.n_top; b++) {
                if (s_stats.top[b].rssi > s_stats.top[a].rssi) {
                    top_dev_t tmp = s_stats.top[a];
                    s_stats.top[a] = s_stats.top[b];
                    s_stats.top[b] = tmp;
                }
            }
        }
        for (int k = 0; k < s_stats.n_top; k++) {
            const top_dev_t *t = &s_stats.top[k];
            ESP_LOGI(TAG, "  %d. %s rssi=%d n=%u %s%s company=0x%04x(%s) appearance=0x%04x name=\"%s\"",
                     k + 1, fmt_addr(&t->addr), t->rssi, t->reports,
                     t->connectable ? "conn" : "nonconn", t->extended ? " EXT" : "",
                     t->company, company_name(t->company), t->appearance, t->name);
        }
    }
    uint32_t win = s_stats.win + 1;
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.win = win;
    heartbeat_arm();
}

static void on_adv(const adv_report_t *r)
{
    struct ble_hs_adv_fields f;
    bool parsed = r->complete && r->len && ble_hs_adv_parse_fields(&f, r->data, r->len) == 0;
    if (!parsed) {
        memset(&f, 0, sizeof(f));
    }

    stats_update(r, &f, parsed);
    adv_seen_t *e = seen_entry(&r->addr);
    if (e->win != s_stats.win) {
        e->win = s_stats.win;
        s_stats.unique++;
    }

#if CONFIG_SRR_DEBUG_ADV_ALL
    debug_dump_adv(r, &f, parsed, "all");
#else
    bool *dumped = r->scan_rsp ? &e->dumped_rsp : &e->dumped_adv;
    if (!*dumped && (r->rssi >= CONFIG_SRR_DEBUG_ADV_RSSI || r->directed || !r->legacy)) {
        *dumped = true;
        debug_dump_adv(r, &f, parsed, r->rssi >= CONFIG_SRR_DEBUG_ADV_RSSI ? "near" :
                       r->directed ? "directed" : "extended");
    }
#endif

    if (s_conn != BLE_HS_CONN_HANDLE_NONE || s_connecting) {
        return;
    }

    /* Reconnection of an already-bonded remote */
    if (r->connectable && is_bonded(&r->addr) && !tv_is_peer(&r->addr)) {
        connect_to(&r->addr, "bonded remote");
        return;
    }
    /* Directed advertising only reaches us if it targets our address, so it
     * comes from something that bonded with us even if resolution failed */
    if (r->directed && remote_bond()) {
        /* The C3 controller reports the remote's directed adverts with
         * address 00:00:00:00:00:00; connecting to that never succeeds.
         * Connect to the bonded identity address instead (we keep 1 remote bond). */
        const ble_addr_t *bond = remote_bond();
        ESP_LOGI(TAG, "Directed adv from %s (type %u), using bond %s", fmt_addr(&r->addr),
                 r->addr.type, fmt_addr(bond));
        connect_to(bond, "directed advertising to us");
        return;
    }

    if (!parsed) {
        return;
    }

    /* Observed pairing-mode advert (Smart Control 2016): flags 0x05 (limited
     * discoverable), appearance 0x0180, uuid16 1812/180f/180a, name in the
     * scan response, no manufacturer data */
    if (f.mfg_data_len >= 2 && get_le16(f.mfg_data) == SAMSUNG_COMPANY_ID) {
        e->samsung = true;
    }
    if (f.name_len >= strlen(SAMSUNG_REMOTE_NAME) &&
        memcmp(f.name, SAMSUNG_REMOTE_NAME, strlen(SAMSUNG_REMOTE_NAME)) == 0) {
        e->samsung = true;
    }
    for (int i = 0; i < f.num_uuids16; i++) {
        if (f.uuids16[i].value == UUID_SVC_HID) {
            e->hid_hint = true;
        }
    }
    if (f.appearance_is_present && appearance_is_hid(f.appearance)) {
        e->hid_hint = true;
    }
    if (f.flags & (BLE_HS_ADV_F_DISC_LTD | BLE_HS_ADV_F_DISC_GEN)) {
        e->discoverable = true;
    }

    if (!e->samsung && !e->hid_hint) {
        return;
    }
    s_stats.samsung += e->samsung;

    /* The C3 controller sometimes reports the remote's adverts with address
     * 00:00:00:00:00:00 (seen after it dropped our link and went back to
     * pairing mode); connecting to that never succeeds, use the bond instead */
    static const uint8_t zero[6];
    if (memcmp(r->addr.val, zero, sizeof(zero)) == 0) {
        const ble_addr_t *bond = remote_bond();
        if (r->connectable && bond && e->samsung && e->hid_hint) {
            ESP_LOGI(TAG, "Remote advert from 00:00:00:00:00:00, using bond %s", fmt_addr(bond));
            connect_to(bond, "bonded remote, address not reported");
        }
        return;
    }

    const char *skip = NULL;
    if (!r->connectable) {
        skip = "not connectable";
    } else if (r->rssi < CONFIG_SRR_MIN_RSSI) {
        skip = "RSSI below threshold, bring it closer";
    } else if (!e->discoverable) {
        skip = "not discoverable (not in pairing mode)";
    } else if (!e->hid_hint) {
        skip = "no HID UUID/appearance";
#if CONFIG_SRR_STRICT_FILTER
    } else if (!e->samsung) {
        skip = "strict filter: not a Samsung remote (no \"" SAMSUNG_REMOTE_NAME "\" name or 0x0075 data yet)";
#endif
    }

    if (!e->logged || !skip) {
        char name[32] = "";
        if (f.name_len) {
            snprintf(name, sizeof(name), "%.*s", f.name_len, (const char *)f.name);
        }
        ESP_LOGI(TAG, "Candidate %s rssi=%d %s flags=0x%02x name=\"%s\" appearance=%s0x%04x "
                 "hid=%d samsung=%d%s%s", fmt_addr(&r->addr), r->rssi, r->legacy ? "legacy" : "EXT", f.flags, name,
                 f.appearance_is_present ? "" : "(none)", f.appearance, e->hid_hint, e->samsung,
                 skip ? " -> skip: " : "", skip ? skip : "");
        e->logged = true;
    }
    if (!skip) {
        connect_to(&r->addr, "remote in pairing mode");
    }
}

/* ------------------------------------------------------------------------ */
/* GATT: dump, read, subscribe                                              */
/* ------------------------------------------------------------------------ */

static gatt_op_t *push_op(op_kind_t kind, uint16_t handle, int idx, const ble_uuid_t *uuid)
{
    if (s_n_ops >= MAX_OPS) {
        ESP_LOGW(TAG, "op queue full");
        return NULL;
    }
    gatt_op_t *op = &s_ops[s_n_ops++];
    memset(op, 0, sizeof(*op));
    op->kind = kind;
    op->handle = handle;
    op->idx = idx;
    if (uuid) {
        ble_uuid_copy(&op->uuid, uuid);
    }
    return op;
}

static const char *props_str(uint8_t p)
{
    static char buf[8];
    snprintf(buf, sizeof(buf), "%c%c%c%c%c",
             (p & BLE_GATT_CHR_PROP_READ) ? 'R' : '-',
             (p & BLE_GATT_CHR_PROP_WRITE) ? 'W' : '-',
             (p & BLE_GATT_CHR_PROP_WRITE_NO_RSP) ? 'w' : '-',
             (p & BLE_GATT_CHR_PROP_NOTIFY) ? 'N' : '-',
             (p & BLE_GATT_CHR_PROP_INDICATE) ? 'I' : '-');
    return buf;
}

static void build_ops(const struct peer *peer)
{
    const struct peer_svc *svc;
    const struct peer_chr *chr;
    const struct peer_dsc *dsc;

    ESP_LOGI(TAG, "==== GATT table ====");
    SLIST_FOREACH(svc, &peer->svcs, next) {
        bool is_hid = uuid16_of(&svc->svc.uuid.u) == UUID_SVC_HID;
        int hid_idx = -1;
        if (is_hid && s_n_hid < MAX_HID_SVCS) {
            hid_idx = s_n_hid++;
        }
        ESP_LOGI(TAG, "SVC %s [0x%04x..0x%04x]", uuid_str(&svc->svc.uuid.u),
                 svc->svc.start_handle, svc->svc.end_handle);

        SLIST_FOREACH(chr, &svc->chrs, next) {
            const ble_uuid_t *cu = &chr->chr.uuid.u;
            uint16_t u16 = uuid16_of(cu);
            uint8_t props = chr->chr.properties;
            uint16_t vh = chr->chr.val_handle;

            ESP_LOGI(TAG, "  CHR %s h=0x%04x %s", uuid_str(cu), vh, props_str(props));
            SLIST_FOREACH(dsc, &chr->dscs, next) {
                ESP_LOGI(TAG, "    DSC %s h=0x%04x", uuid_str(&dsc->dsc.uuid.u), dsc->dsc.handle);
            }

            if (hid_idx >= 0 && u16 == UUID_CHR_REPORT_MAP) {
                push_op(OP_READ_REPORT_MAP, vh, hid_idx, cu);
            } else if (hid_idx >= 0 && u16 == UUID_CHR_PROTO_MODE &&
                       (props & BLE_GATT_CHR_PROP_WRITE_NO_RSP)) {
                push_op(OP_SET_REPORT_PROTOCOL, vh, hid_idx, cu);
            } else if ((props & BLE_GATT_CHR_PROP_READ) && u16 != UUID_CHR_REPORT &&
                       (CONFIG_SRR_DUMP_READABLE || u16 == UUID_CHR_BATTERY ||
                        (u16 >= 0x2a23 && u16 <= 0x2a2a) || u16 == 0x2a50 || u16 == 0x2a4a)) {
                push_op(OP_READ_VALUE, vh, -1, cu);
            }

            if (!(props & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE))) {
                /* Output/feature reports: log their report ID (host -> remote channel) */
                if (hid_idx >= 0 && u16 == UUID_CHR_REPORT) {
                    if (props & BLE_GATT_CHR_PROP_READ) {
                        push_op(OP_READ_VALUE, vh, -1, cu); /* GET_REPORT, read-only */
                    }
                    SLIST_FOREACH(dsc, &chr->dscs, next) {
                        if (uuid16_of(&dsc->dsc.uuid.u) == UUID_DSC_REPORT_REF) {
                            gatt_op_t *op = push_op(OP_READ_REPORT_REF, dsc->dsc.handle, -1, cu);
                            if (op) {
                                op->val_handle = vh;
                            }
                        }
                    }
                }
                continue;
            }
            /* We run in Report Protocol mode, boot reports would only duplicate */
            if (is_hid && (u16 == UUID_CHR_BOOT_KBD_IN || u16 == UUID_CHR_BOOT_MOUSE)) {
                continue;
            }
            if (s_n_slots >= MAX_SLOTS) {
                ESP_LOGW(TAG, "too many notifying characteristics");
                continue;
            }
            int si = s_n_slots++;
            notify_slot_t *slot = &s_slots[si];
            slot->val_handle = vh;
            slot->uuid16 = u16;
            ble_uuid_copy(&slot->uuid, cu);
            slot->props = props;
            slot->hid_idx = hid_idx;
            SLIST_FOREACH(dsc, &chr->dscs, next) {
                uint16_t du = uuid16_of(&dsc->dsc.uuid.u);
                if (du == UUID_DSC_CCCD) {
                    slot->cccd_handle = dsc->dsc.handle;
                } else if (du == UUID_DSC_REPORT_REF && hid_idx >= 0) {
                    push_op(OP_READ_REPORT_REF, dsc->dsc.handle, si, &dsc->dsc.uuid.u);
                }
            }
        }
    }
    ESP_LOGI(TAG, "==== end of GATT table ====");

    /* Subscribe last, once report IDs are known */
    for (int i = 0; i < s_n_slots; i++) {
        if (s_slots[i].cccd_handle) {
            push_op(OP_ENABLE_CCCD, s_slots[i].cccd_handle, i, &s_slots[i].uuid.u);
        }
    }
}

static void ops_advance(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    s_op_idx++;
    ops_next();
}

static void read_done(const gatt_op_t *op)
{
    switch (op->kind) {
    case OP_READ_REPORT_MAP: {
        ESP_LOGI(TAG, "HID[%d] Report Map (%u bytes):", op->idx, s_rd_len);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, s_rd_buf, s_rd_len, ESP_LOG_INFO);
        hid_parse(&s_hid[op->idx], s_rd_buf, s_rd_len);
        hid_dump(&s_hid[op->idx]);
        break;
    }
    case OP_READ_REPORT_REF: {
        if (op->idx < 0) {
            ESP_LOGI(TAG, "Report (no notify) ref h=0x%04x -> rid=0x%02x type=%u", op->handle,
                     s_rd_len >= 1 ? s_rd_buf[0] : 0, s_rd_len >= 2 ? s_rd_buf[1] : 0);
            if (s_rd_len >= 2 && s_rd_buf[0] == VOICE_REPORT_ID && s_rd_buf[1] == 3) {
                voice_set_cmd_handle(s_conn, op->val_handle);
            }
            break;
        }
        notify_slot_t *slot = &s_slots[op->idx];
        if (s_rd_len >= 2) {
            slot->report_id = s_rd_buf[0];
            slot->report_type = s_rd_buf[1];
        }
        ESP_LOGI(TAG, "Report h=0x%04x -> HID[%d] rid=0x%02x type=%u", slot->val_handle,
                 slot->hid_idx, slot->report_id, slot->report_type);
        break;
    }
    case OP_READ_VALUE: {
        char label[64];
        snprintf(label, sizeof(label), "%s h=0x%04x", uuid_str(&op->uuid.u), op->handle);
        log_value(label, s_rd_buf, s_rd_len);
        if (uuid16_of(&op->uuid.u) == UUID_CHR_BATTERY && s_rd_len) {
            remote_on_battery(s_rd_buf[0]);
        }
        if (uuid16_of(&op->uuid.u) == 0x2a50) {
            tv_set_remote_pnp(s_rd_buf, s_rd_len);
        }
        break;
    }
    default:
        break;
    }
}

static int on_read(uint16_t conn, const struct ble_gatt_error *err,
                   struct ble_gatt_attr *attr, void *arg)
{
    const gatt_op_t *op = &s_ops[s_op_idx];
    if (err->status == 0 && attr) {
        s_rd_len = OS_MBUF_PKTLEN(attr->om);
        if (s_rd_len > sizeof(s_rd_buf)) {
            s_rd_len = sizeof(s_rd_buf);
        }
        os_mbuf_copydata(attr->om, 0, s_rd_len, s_rd_buf);
        read_done(op);
    } else {
        ESP_LOGW(TAG, "  read h=0x%04x failed; status=0x%x", op->handle, err->status);
    }
    ops_advance();
    return 0;
}

static int on_read_long(uint16_t conn, const struct ble_gatt_error *err,
                        struct ble_gatt_attr *attr, void *arg)
{
    const gatt_op_t *op = &s_ops[s_op_idx];
    if (err->status == 0 && attr) {
        uint16_t n = OS_MBUF_PKTLEN(attr->om);
        if (s_rd_len + n > sizeof(s_rd_buf)) {
            n = sizeof(s_rd_buf) - s_rd_len;
        }
        os_mbuf_copydata(attr->om, 0, n, s_rd_buf + s_rd_len);
        s_rd_len += n;
        return 0; /* more chunks follow, then BLE_HS_EDONE */
    }
    if (err->status == BLE_HS_EDONE) {
        read_done(op);
    } else {
        ESP_LOGW(TAG, "  long read h=0x%04x failed; status=0x%x", op->handle, err->status);
    }
    ops_advance();
    return 0;
}

static int on_write(uint16_t conn, const struct ble_gatt_error *err,
                    struct ble_gatt_attr *attr, void *arg)
{
    const gatt_op_t *op = &s_ops[s_op_idx];
    if (err->status != 0) {
        ESP_LOGW(TAG, "  subscribe h=0x%04x failed; status=0x%x", op->handle, err->status);
    }
    ops_advance();
    return 0;
}

static void ops_next(void)
{
    while (s_op_idx < s_n_ops) {
        const gatt_op_t *op = &s_ops[s_op_idx];
        int rc;

        s_rd_len = 0;
        switch (op->kind) {
        case OP_READ_VALUE:
        case OP_READ_REPORT_REF:
            rc = ble_gattc_read(s_conn, op->handle, on_read, NULL);
            break;
        case OP_READ_REPORT_MAP:
            rc = ble_gattc_read_long(s_conn, op->handle, 0, on_read_long, NULL);
            break;
        case OP_SET_REPORT_PROTOCOL: {
            uint8_t mode = 1; /* Report Protocol */
            ble_gattc_write_no_rsp_flat(s_conn, op->handle, &mode, 1);
            s_op_idx++;
            continue;
        }
        case OP_ENABLE_CCCD: {
            const notify_slot_t *slot = &s_slots[op->idx];
            uint16_t v = htole16((slot->props & BLE_GATT_CHR_PROP_NOTIFY) ? 0x0001 : 0x0002);
            rc = ble_gattc_write_flat(s_conn, op->handle, &v, sizeof(v), on_write, NULL);
            break;
        }
        default:
            rc = BLE_HS_EINVAL;
            break;
        }
        if (rc == 0) {
            return; /* the callback advances */
        }
        ESP_LOGW(TAG, "GATT op %d on h=0x%04x failed to start; rc=%d", op->kind, op->handle, rc);
        s_op_idx++;
    }

    if (s_n_ops) {
        s_n_ops = 0;
        ESP_LOGI(TAG, "Subscribed to %d characteristics", s_n_slots);
        s_ready = true;
        remote_on_connection(true);
        tv_set_remote_up(true);

        if (s_n_pending || s_n_dropped) {
            ESP_LOGI(TAG, "Replaying %d notifications received during setup (%d too large, dropped)",
                     s_n_pending, s_n_dropped);
        }
        for (int i = 0; i < s_n_pending; i++) {
            on_notify_data(s_pending[i].handle, s_pending[i].data, s_pending[i].len);
        }
        s_n_pending = 0;

        if (s_have_deferred_params) {
            s_have_deferred_params = false;
            int rc = ble_gap_update_params(s_conn, &s_deferred_params);
            ESP_LOGI(TAG, "Applying the remote's connection params now (latency=%u); rc=%d",
                     s_deferred_params.latency, rc);
        }
    }
}

static void on_disc_complete(const struct peer *peer, int status, void *arg)
{
    if (status != 0) {
        ESP_LOGE(TAG, "Service discovery failed; status=%d", status);
        ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    build_ops(peer);
    s_op_idx = 0;
    ops_next();
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    if (err->status == 0) {
        ESP_LOGI(TAG, "MTU %u", mtu);
    }
    int rc = peer_disc_all(conn, on_disc_complete, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "peer_disc_all failed; rc=%d", rc);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* notifications                                                            */
/* ------------------------------------------------------------------------ */

static notify_slot_t *slot_by_handle(uint16_t handle)
{
    for (int i = 0; i < s_n_slots; i++) {
        if (s_slots[i].val_handle == handle) {
            return &s_slots[i];
        }
    }
    return NULL;
}

static bool contains(const uint32_t *set, int n, uint32_t v)
{
    for (int i = 0; i < n; i++) {
        if (set[i] == v) {
            return true;
        }
    }
    return false;
}

/* With CONFIG_SRR_VOICE_LOCAL the TV never sees the voice key or the audio:
 * the board runs the voice session itself */
static bool relay_to_tv(const notify_slot_t *slot, const uint8_t *data, uint16_t len)
{
#if CONFIG_SRR_VOICE_LOCAL
    if (slot->report_type != 1) {
        return true;
    }
    if (slot->report_id == VOICE_AUDIO_REPORT_ID) {
        return false;
    }
    if (slot->report_id == SAMSUNG_KEY_REPORT_ID && len >= 6 && CONFIG_SRR_VOICE_KEY != 0) {
        /* press, or release of a held voice key (checked before samsung_key_report) */
        return data[0] != CONFIG_SRR_VOICE_KEY &&
               !(data[0] == 0 && data[3] == 0x01 && samsung_key_down_code() == CONFIG_SRR_VOICE_KEY);
    }
#endif
    return true;
}

static void on_hid_report(notify_slot_t *slot, const uint8_t *data, uint16_t len)
{
    hid_decoded_t d;

    if (relay_to_tv(slot, data, len)) {
        tv_relay_report(slot->report_id, slot->report_type, data, len);
    }

    if ((slot->report_id == VOICE_REPORT_ID || slot->report_id == VOICE_AUDIO_REPORT_ID) &&
        slot->report_type == 1) {
        voice_on_frame(data, len);
        return;
    }

#if CONFIG_SRR_LOG_RAW
    /* Large (voice?) reports are printed in full by remote_on_vendor_report */
    if (len <= 32) {
        char hex[3 * 32 + 1];
        int n = 0;
        for (int i = 0; i < len; i++) {
            n += snprintf(hex + n, sizeof(hex) - n, "%02x ", data[i]);
        }
        hex[n] = 0;
        ESP_LOGI(TAG, "RAW rid=0x%02x h=0x%04x: %s", slot->report_id, slot->val_handle, hex);
    }
#endif

    if (slot->report_id == SAMSUNG_KEY_REPORT_ID && samsung_key_report(data, len)) {
        return;
    }

    hid_decode(&s_hid[slot->hid_idx], slot->report_id, data, len, &d);

    for (int i = 0; i < d.n_keys; i++) {
        if (!contains(slot->prev, slot->n_prev, d.keys[i])) {
            remote_on_key(d.keys[i], true);
        }
    }
    for (int i = 0; i < slot->n_prev; i++) {
        if (!contains(d.keys, d.n_keys, slot->prev[i])) {
            remote_on_key(slot->prev[i], false);
        }
    }
    memcpy(slot->prev, d.keys, d.n_keys * sizeof(d.keys[0]));
    slot->n_prev = d.n_keys;

    for (int i = 0; i < d.n_axes; i++) {
        remote_on_axis(d.axes[i].usage, d.axes[i].value);
    }
    if (d.has_vendor || !d.matched) {
        remote_on_vendor_report(slot->val_handle, slot->report_id, data, len);
    }
}

static void on_notify(uint16_t handle, struct os_mbuf *om)
{
    static uint8_t buf[256];
    uint16_t len = OS_MBUF_PKTLEN(om);
    if (len > sizeof(buf)) {
        len = sizeof(buf);
    }
    os_mbuf_copydata(om, 0, len, buf);

    if (!s_ready) {
        if (len <= sizeof(s_pending[0].data) && s_n_pending < (int)(sizeof(s_pending) / sizeof(s_pending[0]))) {
            pending_notify_t *p = &s_pending[s_n_pending++];
            p->handle = handle;
            p->len = len;
            memcpy(p->data, buf, len);
        } else {
            s_n_dropped++;
        }
        return;
    }
    on_notify_data(handle, buf, len);
}

static void on_notify_data(uint16_t handle, const uint8_t *buf, uint16_t len)
{
    notify_slot_t *slot = slot_by_handle(handle);
    if (!slot) {
        remote_on_other_notify(handle, "?", buf, len);
    } else if (slot->hid_idx >= 0 && slot->uuid16 == UUID_CHR_REPORT) {
        on_hid_report(slot, buf, len);
    } else if (slot->uuid16 == UUID_CHR_BATTERY && len) {
        remote_on_battery(buf[0]);
        tv_relay_battery(buf[0]);
    } else {
        remote_on_other_notify(handle, uuid_str(&slot->uuid.u), buf, len);
    }
}

/* ------------------------------------------------------------------------ */
/* GAP events                                                               */
/* ------------------------------------------------------------------------ */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;

    if (event->type != BLE_GAP_EVENT_DISC && event->type != BLE_GAP_EVENT_EXT_DISC &&
        event->type != BLE_GAP_EVENT_NOTIFY_RX) {
        ESP_LOGI(TAG, "gap event %d (%s)", event->type, gap_event_name(event->type));
    }

    switch (event->type) {
#if MYNEWT_VAL(BLE_EXT_ADV)
    case BLE_GAP_EVENT_EXT_DISC: {
        const struct ble_gap_ext_disc_desc *d = &event->ext_disc;
        adv_report_t r = {
            .addr = d->addr,
            .direct_addr = d->direct_addr,
            .rssi = d->rssi,
            .connectable = d->props & BLE_HCI_ADV_CONN_MASK,
            .directed = d->props & BLE_HCI_ADV_DIRECT_MASK,
            .scan_rsp = d->props & BLE_HCI_ADV_SCAN_RSP_MASK,
            .legacy = d->props & BLE_HCI_ADV_LEGACY_MASK,
            .complete = d->data_status == BLE_GAP_EXT_ADV_DATA_STATUS_COMPLETE,
            .prim_phy = d->prim_phy,
            .sec_phy = d->sec_phy,
            .sid = d->sid,
            .len = d->length_data,
            .data = d->data,
        };
        on_adv(&r);
        return 0;
    }
#else
    case BLE_GAP_EVENT_DISC: {
        const struct ble_gap_disc_desc *d = &event->disc;
        adv_report_t r = {
            .addr = d->addr,
            .direct_addr = d->direct_addr,
            .rssi = d->rssi,
            .connectable = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                           d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND,
            .directed = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND,
            .scan_rsp = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP,
            .legacy = true,
            .complete = true,
            .prim_phy = 1,
            .len = d->length_data,
            .data = d->data,
        };
        on_adv(&r);
        return 0;
    }
#endif

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGW(TAG, "Scan stopped; reason=%d", event->disc_complete.reason);
        return 0;

    case BLE_GAP_EVENT_PARING_COMPLETE:
        ESP_LOGI(TAG, "Pairing complete; status=0x%x (%s)", event->pairing_complete.status,
                 event->pairing_complete.status == 0 ? "ok" : err_name(event->pairing_complete.status));
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        ESP_LOGW(TAG, "Unexpected passkey request; action=%d (we are NO_IO / Just Works)",
                 event->passkey.params.action);
        return 0;

    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        ESP_LOGI(TAG, "PHY update; status=%d tx=%u rx=%u", event->phy_updated.status,
                 event->phy_updated.tx_phy, event->phy_updated.rx_phy);
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        s_connecting = false;
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "Connection failed; status=0x%x (%s)", event->connect.status,
                     err_name(event->connect.status));
            scan_start();
            return 0;
        }
        s_conn = event->connect.conn_handle;
        ble_gap_conn_find(s_conn, &desc);
        ESP_LOGI(TAG, "Connected to %s, itvl=%u latency=%u timeout=%u", fmt_addr(&desc.peer_id_addr),
                 desc.conn_itvl, desc.conn_latency, desc.supervision_timeout);
        peer_add(s_conn);
        /* Pairs (Just Works) on first contact, or encrypts with the stored LTK */
        rc = ble_gap_security_initiate(s_conn);
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGE(TAG, "ble_gap_security_initiate failed; rc=%d", rc);
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected; reason=0x%x (%s)", event->disconnect.reason,
                 err_name(event->disconnect.reason));
        peer_delete(event->disconnect.conn.conn_handle);
        if (s_n_slots) {
            remote_on_connection(false);
        }
        samsung_key_reset();
        voice_on_disconnect();
        reset_conn_state();
        refresh_bonds();
        tv_set_remote_up(false); /* stop advertising to the TV before scanning */
        scan_start();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        if (event->enc_change.status != 0) {
            ESP_LOGW(TAG, "Encryption failed; status=0x%x (%s)", event->enc_change.status,
                     err_name(event->enc_change.status));
            if (event->enc_change.status == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING) ||
                event->enc_change.status == BLE_HS_SM_US_ERR(BLE_SM_ERR_UNSPECIFIED)) {
                ESP_LOGW(TAG, "The remote no longer has our bond (re-paired to the TV?). "
                         "Put it in pairing mode (Back + Play/Pause 3 s) to pair again.");
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        ESP_LOGI(TAG, "Link encrypted (bonded=%d, key size=%u)", desc.sec_state.bonded,
                 desc.sec_state.key_size);
        if (desc.sec_state.bonded) {
            drop_other_bonds(&desc.peer_id_addr);
        }
        if (s_gatt_started) {
            return 0; /* key refresh etc. on an already set up link */
        }
        s_gatt_started = true;
        rc = ble_gattc_exchange_mtu(event->enc_change.conn_handle, on_mtu, NULL);
        if (rc != 0) {
            on_mtu(event->enc_change.conn_handle, &(struct ble_gatt_error){.status = rc}, 0, NULL);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* The remote dropped its key and wants to pair again: allow it */
        ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_IDENTITY_RESOLVED:
        ble_gap_conn_find(event->identity_resolved.conn_handle, &desc);
        ESP_LOGI(TAG, "Remote identity address: %s", fmt_addr(&desc.peer_id_addr));
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        on_notify(event->notify_rx.attr_handle, event->notify_rx.om);
        return 0;

    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ:
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        ESP_LOGI(TAG, "Remote requests itvl=%u..%u latency=%u timeout=%u%s",
                 event->conn_update_req.peer_params->itvl_min,
                 event->conn_update_req.peer_params->itvl_max,
                 event->conn_update_req.peer_params->latency,
                 event->conn_update_req.peer_params->supervision_timeout,
                 s_ready ? "" : " -> deferred until setup is done");
        if (!s_ready) {
            /* With latency 99 every GATT request waits up to ~2 s; finish the
             * setup first, then apply the remote's power-saving parameters */
            s_deferred_params = *event->conn_update_req.peer_params;
            s_have_deferred_params = true;
            return BLE_ERR_UNSPECIFIED;
        }
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "Connection params now itvl=%u latency=%u timeout=%u",
                     desc.conn_itvl, desc.conn_latency, desc.supervision_timeout);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        return 0;

    default:
        return 0;
    }
}

/* ------------------------------------------------------------------------ */
/* startup                                                                  */
/* ------------------------------------------------------------------------ */

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset; reason=%d", reason);
}

/* The TV keys its bond and its GATT cache on our BD address. Setting an own
 * address other than the burned-in one (CONFIG_SRR_OWN_ADDR) therefore gives
 * the board a clean identity: a fresh gatt_cache_<mac> file, no stale entries
 * from earlier GATT layouts. Empty = use the burned-in address. Changing the
 * address invalidates all bonds: re-pair the remote (Back+Play/Pause) and the
 * TV (BOOT + TV search, REMOTE_PROTO.md §2.5). */
static void apply_own_addr(void)
{
    const char *want = CONFIG_SRR_OWN_ADDR;
    if (*want == 0) {
        return;
    }
    /* Most significant byte first, as written: "0c4ea061b80b" = 0c:4e:a0:61:b8:0b */
    uint8_t mac[6];
    int n = 0, hi = -1;
    for (const char *p = want; *p && n < 6; p++) {
        int v = (*p >= '0' && *p <= '9') ? *p - '0'
               : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10
               : (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10 : -1;
        if (v < 0) {
            continue; /* separators like ':' */
        }
        if (hi < 0) {
            hi = v;
        } else {
            mac[n++] = (uint8_t)(hi << 4 | v);
            hi = -1;
        }
    }
    if (n != 6) {
        ESP_LOGE(TAG, "CONFIG_SRR_OWN_ADDR malformed (%d bytes), using the burned-in address", n);
        return;
    }
    /* The controller reads ESP_MAC_BT when it starts, so this must run before
     * nimble_port_init(). ble_hs_id_set_pub() would only change the host's
     * copy: the radio would still use the burned-in address and pairing
     * would compute its checks with the wrong one */
    esp_err_t err = esp_iface_mac_addr_set(mac, ESP_MAC_BT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_iface_mac_addr_set failed: %s, using the burned-in address", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "own address set to %02x:%02x:%02x:%02x:%02x:%02x (CONFIG_SRR_OWN_ADDR)", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
}

static void on_sync(void)
{
    uint8_t addr[6];

    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "Own address %02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    refresh_bonds();
    for (int i = 0; i < s_n_bonds; i++) {
        ESP_LOGI(TAG, "Stored bond: %s%s", fmt_addr(&s_bonds[i]), tv_is_peer(&s_bonds[i]) ? " (TV)" : "");
    }
    scan_start();
    tv_start();
#if CONFIG_SRR_SKIP_REMOTE
    ESP_LOGW(TAG, "SRR_SKIP_REMOTE: not scanning for the remote, the TV relay runs alone");
    tv_set_remote_up(true);
#endif
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

#if CONFIG_SRR_CLEAR_BOND_GPIO >= 0
static void forget_button_task(void *param)
{
    const gpio_num_t pin = CONFIG_SRR_CLEAR_BOND_GPIO;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    /* Short press: pair with the TV; hold 3 s: forget all bonds */
    int held = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(pin) != 0) {
            if (held >= 1 && held < 20) {
                tv_pairing_request();
            }
            held = 0;
            continue;
        }
        held++;
        if (held == 60) {
            ESP_LOGW(TAG, "Forget button held: erasing bonds and rebooting");
            ble_store_clear();
            tv_forget();
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
    }
}
#endif

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    apply_own_addr();
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", ret);
        return;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Headless bonding, same as the TV: Just Works, keep the keys */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

#if MYNEWT_VAL(BLE_INCL_SVC_DISCOVERY) || MYNEWT_VAL(BLE_GATT_CACHING_INCLUDE_SERVICES)
    int rc = peer_init(1, 24, 8, 96, 160);
#else
    int rc = peer_init(1, 24, 96, 160);
#endif
    assert(rc == 0);

    ble_store_config_init();
#if CONFIG_SRR_HEARTBEAT_S > 0
    ble_npl_callout_init(&s_hb_co, nimble_port_get_dflt_eventq(), heartbeat, NULL);
#endif
    voice_init();
    led_init();
    tv_init();
    nimble_port_freertos_init(host_task);

#if CONFIG_SRR_CLEAR_BOND_GPIO >= 0
    xTaskCreate(forget_button_task, "forget_btn", 2048, NULL, 1, NULL);
#endif
}
