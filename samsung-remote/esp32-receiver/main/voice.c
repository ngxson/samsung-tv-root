#include "voice.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "led.h"
#include "tv.h"

static const char *TAG = "voice";

#define POLL_MS        500
#define END_SILENCE_MS 1000
/* After the stop command, wait this long for the frames still in flight */
#define STOP_DRAIN_MS  300
#define MAX_CMD        16
/* 0xFB inputs shorter than this are command replies (e.g. "33 17 ..."), not audio */
#define MIN_AUDIO_LEN  64

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_cmd_handle;
static struct ble_npl_callout s_co;

static struct {
    bool active;   /* voice key tapped: start sent, until stopped */
    bool relay;    /* started by the TV through the relay: the TV sends the commands */
    bool started;  /* first start command sent (after the pre command) */
    bool extended; /* extend command sent */
    bool stopping; /* voice key released, stop sent: draining the last frames */
    bool warned;
    uint8_t key;
    int variant;   /* index into CONFIG_SRR_VOICE_START_CMDS */
    uint32_t frames;
    uint32_t bytes;
    uint32_t replies;
    uint16_t len_min;
    uint16_t len_max;
    uint32_t t_start;
    uint32_t t_first;
    uint32_t t_last;
    uint32_t t_stop;
    bool restore_params;
    struct ble_gap_upd_params saved;
} s_v;

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

/* Entry `index` of a comma-separated hex list ("310117,fb310117"); returns its length, 0 if none */
static int parse_cmd(const char *list, int index, uint8_t *out, int max)
{
    int n = 0;
    int hi = -1;
    for (const char *p = list;; p++) {
        if (*p == ',' || *p == 0) {
            if (index == 0 || *p == 0) {
                return index == 0 ? n : 0;
            }
            index--;
            continue;
        }
        int v = hexval(*p);
        if (v < 0 || index != 0) {
            continue; /* spaces, or not our entry */
        }
        if (hi < 0) {
            hi = v;
        } else if (n < max) {
            out[n++] = (uint8_t)(hi << 4 | v);
            hi = -1;
        }
    }
}

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

static int on_cmd_written(uint16_t conn, const struct ble_gatt_error *err,
                          struct ble_gatt_attr *attr, void *arg)
{
    if (err->status == 0) {
        ESP_LOGI(TAG, "command written");
    } else {
        ESP_LOGW(TAG, "command write failed; status=0x%x", err->status);
    }
    return 0;
}

static void send_cmd(const char *what, const uint8_t *cmd, int len)
{
    ESP_LOGI(TAG, "%s -> h=0x%04x: %s", what, s_cmd_handle, hex_line(cmd, len));
    int rc = ble_gattc_write_flat(s_conn, s_cmd_handle, cmd, len, on_cmd_written, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "write failed to start; rc=%d", rc);
    }
}

static bool send_start(int variant)
{
    uint8_t cmd[MAX_CMD];
    int len = parse_cmd(CONFIG_SRR_VOICE_START_CMDS, variant, cmd, sizeof(cmd));
    if (len == 0) {
        return false;
    }
    char what[24];
    snprintf(what, sizeof(what), "START #%d", variant);
    send_cmd(what, cmd, len);
    return true;
}

/* Like the TV: no peripheral latency while streaming, so commands reach the remote at once */
static void latency_zero(void)
{
#if CONFIG_SRR_VOICE_LATENCY0
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(s_conn, &d) != 0 || d.conn_latency == 0) {
        return;
    }
    if (!s_v.restore_params) {
        s_v.saved = (struct ble_gap_upd_params){
            .itvl_min = d.conn_itvl,
            .itvl_max = d.conn_itvl,
            .latency = d.conn_latency,
            .supervision_timeout = d.supervision_timeout,
        };
    }
    struct ble_gap_upd_params p = s_v.saved;
    p.latency = 0;
    int rc = ble_gap_update_params(s_conn, &p);
    ESP_LOGI(TAG, "latency %u -> 0; rc=%d", d.conn_latency, rc);
    s_v.restore_params = s_v.restore_params || rc == 0;
#endif
}

static void latency_restore(void)
{
    if (!s_v.restore_params) {
        return;
    }
    int rc = ble_gap_update_params(s_conn, &s_v.saved);
    if (rc == BLE_HS_EALREADY) {
        /* The latency-0 update is still in progress: try again later */
        ble_npl_callout_reset(&s_co, ble_npl_time_ms_to_ticks32(POLL_MS));
        return;
    }
    s_v.restore_params = false;
    ESP_LOGI(TAG, "latency -> %u; rc=%d", s_v.saved.latency, rc);
}

static void session_end(const char *why)
{
    uint32_t span = s_v.t_last - s_v.t_first;
    ESP_LOGI(TAG, "SESSION END (%s): %lu ms, START #%d, %lu audio frames, %lu bytes, len %u..%u, "
             "%lu replies, %lu ms of frames (%lu.%lu frames/s)",
             why, (unsigned long)(esp_log_timestamp() - s_v.t_start), s_v.variant,
             (unsigned long)s_v.frames, (unsigned long)s_v.bytes,
             s_v.frames ? s_v.len_min : 0, s_v.len_max, (unsigned long)s_v.replies, (unsigned long)span,
             (unsigned long)(span ? s_v.frames * 1000 / span : 0),
             (unsigned long)(span ? s_v.frames * 10000 / span % 10 : 0));
    s_v.active = false;
    led_set_mode(LED_IDLE);
    ble_npl_callout_stop(&s_co);
    latency_restore();
}

/* Voice key released: stop the stream, end the session once the last frames are in */
static void session_release(void)
{
    uint8_t cmd[MAX_CMD];
    int len = parse_cmd(CONFIG_SRR_VOICE_STOP_CMD, 0, cmd, sizeof(cmd));
    if (len) {
        send_cmd("STOP", cmd, len);
    }
    len = parse_cmd(CONFIG_SRR_VOICE_POST_CMD, 0, cmd, sizeof(cmd));
    if (len) {
        send_cmd("POST", cmd, len);
    }
    s_v.stopping = true;
    s_v.t_stop = esp_log_timestamp();
}

static void session_stop(const char *why)
{
    uint8_t cmd[MAX_CMD];
    int len = parse_cmd(CONFIG_SRR_VOICE_STOP_CMD, 0, cmd, sizeof(cmd));
    if (len) {
        send_cmd("STOP", cmd, len);
    }
    len = parse_cmd(CONFIG_SRR_VOICE_POST_CMD, 0, cmd, sizeof(cmd));
    if (len) {
        send_cmd("POST", cmd, len);
    }
    session_end(why);
}

static void poll(struct ble_npl_event *ev)
{
    uint32_t now = esp_log_timestamp();

    if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        if (s_v.active) {
            session_end("remote not connected");
        }
        return;
    }
    if (!s_v.active) {
        latency_restore();
        return;
    }
    if (s_v.stopping) {
        uint32_t last = s_v.t_last > s_v.t_stop ? s_v.t_last : s_v.t_stop;
        if (now - last >= STOP_DRAIN_MS) {
            session_end("key released");
            return;
        }
    } else if (now - s_v.t_start >= CONFIG_SRR_VOICE_MAX_MS) {
        s_v.relay ? session_end("time limit") : session_stop("time limit");
        return;
    }
    if (!s_v.stopping && s_v.frames && now - s_v.t_last >= END_SILENCE_MS) {
        s_v.relay ? session_end("stream stopped") : session_stop("stream stopped");
        return;
    }
    if (s_v.relay || s_v.stopping) {
        /* the TV drives the session / waiting for the last frames */
    } else if (!s_v.extended) {
        /* Like the TV: one extend right after the start */
        uint8_t cmd[MAX_CMD];
        int len = parse_cmd(CONFIG_SRR_VOICE_EXTEND_CMD, 0, cmd, sizeof(cmd));
        if (len) {
            send_cmd("EXTEND", cmd, len);
        }
        s_v.extended = true;
    } else if (s_v.frames == 0 && !s_v.warned) {
        ESP_LOGW(TAG, "no audio yet: was the voice key still held when the start arrived?");
        s_v.warned = true;
    }
    ble_npl_callout_reset(&s_co, ble_npl_time_ms_to_ticks32(POLL_MS));
}

static void session_begin(uint8_t key, bool relay)
{
    /* Keep a pending latency restore across sessions */
    bool restore = s_v.restore_params;
    struct ble_gap_upd_params saved = s_v.saved;
    memset(&s_v, 0, sizeof(s_v));
    s_v.restore_params = restore;
    s_v.saved = saved;

    s_v.active = true;
    s_v.relay = relay;
    s_v.key = key;
    s_v.t_start = esp_log_timestamp();
    if (relay) {
        ESP_LOGI(TAG, "SESSION START by the TV");
    } else {
        ESP_LOGI(TAG, "SESSION START on key 0x%02x", key);
    }
    led_set_mode(LED_SESSION);
    ble_npl_callout_reset(&s_co, ble_npl_time_ms_to_ticks32(POLL_MS));
}

void voice_init(void)
{
    ble_npl_callout_init(&s_co, nimble_port_get_dflt_eventq(), poll, NULL);
}

void voice_set_cmd_handle(uint16_t conn, uint16_t val_handle)
{
    s_conn = conn;
    s_cmd_handle = val_handle;
    ESP_LOGI(TAG, "command channel: 0xFB Feature report at h=0x%04x", val_handle);
}

void voice_on_disconnect(void)
{
    s_v.restore_params = false;
    if (s_v.active) {
        session_end("disconnected");
    }
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_cmd_handle = 0;
}

void voice_on_key(uint8_t code, bool down)
{
    if (CONFIG_SRR_VOICE_KEY != 0 && code != CONFIG_SRR_VOICE_KEY) {
        return;
    }

    /* Hold to talk: the remote only accepts the start while the key is down */
    if (!down) {
        if (s_v.active && !s_v.relay && !s_v.stopping) {
            session_release();
        }
        return;
    }
#if !CONFIG_SRR_VOICE_LOCAL
    if (tv_connected() || (s_v.active && s_v.relay)) {
        return; /* the TV gets the key through the relay and runs the session */
    }
#endif
    if (s_v.active) {
        session_end("voice key pressed again");
    }
    if (!s_cmd_handle) {
        ESP_LOGW(TAG, "key 0x%02x: no 0xFB Feature report found, cannot start voice", code);
        return;
    }
    session_begin(code, false);
    latency_zero();
    /* The remote only accepts the start while the voice key is down (window opened on
     * press, closed on release), so it goes out at once */
    uint8_t cmd[MAX_CMD];
    int len = parse_cmd(CONFIG_SRR_VOICE_PRE_CMD, 0, cmd, sizeof(cmd));
    if (len) {
        send_cmd("PRE", cmd, len);
    }
    s_v.started = true;
    if (!send_start(0)) {
        ESP_LOGW(TAG, "CONFIG_SRR_VOICE_START_CMDS is empty");
    }
}

void voice_relay_cmd(const uint8_t *cmd, uint16_t len)
{
    bool start = len >= 2 && cmd[0] == 0x31 && cmd[1] == 0x04;
    bool stop = len >= 2 && cmd[0] == 0x31 && cmd[1] == 0x02;

#if CONFIG_SRR_VOICE_LOCAL
    if (len >= 2 && cmd[0] == 0x31 && cmd[1] >= 0x02 && cmd[1] <= 0x06) {
        ESP_LOGW(TAG, "TV voice command ignored, voice stays on the board: %s", hex_line(cmd, len));
        return;
    }
#endif

    if (!s_cmd_handle) {
        /* No session either: nothing could end it (LED stuck on) */
        ESP_LOGW(TAG, "TV command dropped, the remote is not connected: %s", hex_line(cmd, len));
        return;
    }
    if (start && !s_v.active) {
        session_begin(0, true);
    }
    send_cmd("TV->RC", cmd, len);
    if (stop && s_v.active && s_v.relay) {
        session_end("TV sent stop");
    }
}

void voice_on_frame(const uint8_t *data, uint16_t len)
{
    uint32_t now = esp_log_timestamp();

    if (!CONFIG_SRR_VOICE_LOCAL && len >= MIN_AUDIO_LEN && !s_v.active && tv_connected()) {
        session_begin(0, true); /* audio the TV asked for in a way we did not spot */
    }
    /* Byte 0 is a sub-report ID (0x1F-0x34 on the TV side) */
    if (len < MIN_AUDIO_LEN || !s_v.active) {
        if (s_v.active) {
            s_v.replies++;
        }
        ESP_LOGI(TAG, "FB %s sub=0x%02x len=%u %s", s_v.active ? "reply" : "idle", len ? data[0] : 0, len,
                 hex_line(data, len));
        return;
    }

    if (s_v.frames == 0) {
        ESP_LOGI(TAG, "first audio frame %lu ms after the tap, START #%d",
                 (unsigned long)(now - s_v.t_start), s_v.variant);
        s_v.t_first = now;
        s_v.len_min = len;
        led_set_mode(LED_RECORDING);
    }
    s_v.frames++;
    s_v.bytes += len;
    s_v.t_last = now;
    if (len < s_v.len_min) {
        s_v.len_min = len;
    }
    if (len > s_v.len_max) {
        s_v.len_max = len;
    }
    ESP_LOGI(TAG, "FRAME %lu len=%u %s", (unsigned long)s_v.frames, len, hex_line(data, len));
}
