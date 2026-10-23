#include "usb_mic.h"
#include <gui/gui.h>
#include <input/input.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    UsbMic usb;
    FuriMessageQueue* keys;
    bool usb_ready;
} MicApp;

static void draw(Canvas* canvas, void* context) {
    MicApp* app = context;
    MicStatus status;
    usb_mic_snapshot(&app->usb, &status);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "Dummy USB Mic");
    canvas_set_font(canvas, FontSecondary);
    const char* state = !app->usb_ready    ? "USB busy - close and retry" :
                        status.suspended   ? "USB suspended / unplugged" :
                        !status.configured ? "Waiting for USB host" :
                        status.reading     ? "HOST READING" :
                        status.alt         ? "Open, waiting for reads" :
                                             "Idle - host not reading";
    canvas_draw_str(canvas, 0, 22, state);
    char line[64];
    snprintf(
        line,
        sizeof(line),
        "Read total: %lu.%03lu s",
        (unsigned long)(status.packets / 1000),
        (unsigned long)(status.packets % 1000));
    canvas_draw_str(canvas, 0, 33, line);
    snprintf(
        line,
        sizeof(line),
        "Session: %lu.%03lu s",
        (unsigned long)(status.session_packets / 1000),
        (unsigned long)(status.session_packets % 1000));
    canvas_draw_str(canvas, 0, 43, line);
    snprintf(
        line,
        sizeof(line),
        "16k mono | %s | %u",
        status.silence ? "silent" : "noise",
        status.level);
    canvas_draw_str(canvas, 0, 53, line);
    canvas_draw_str(canvas, 0, 63, "OK: reset    Back: exit");
}

static void input(InputEvent* event, void* context) {
    MicApp* app = context;
    furi_message_queue_put(app->keys, event, 0);
}

static bool command(MicApp* app, char* line) {
    MicStatus status;
    usb_mic_snapshot(&app->usb, &status);
    uint16_t level = status.level;
    char reply[512];
    switch(mic_parse_command(line, &level)) {
    case MicCommandHelp:
        usb_mic_send(
            &app->usb, "OK help ping status reset noise silence level 0..4096 reconnect exit\r\n");
        break;
    case MicCommandPing:
        usb_mic_send(&app->usb, "OK pong dummy_mic/0.1\r\n");
        break;
    case MicCommandStatus:
        snprintf(
            reply,
            sizeof(reply),
            "OK {\"configured\":%s,\"suspended\":%s,\"alt\":%u,\"reading\":%s,"
            "\"total_ms\":%lu,\"session_ms\":%lu,\"packets\":%lu,"
            "\"starts\":%lu,\"stops\":%lu,\"errors\":%lu,\"rx_overflows\":%lu,"
            "\"mode\":\"%s\",\"level\":%u,\"rate\":16000}\r\n",
            status.configured ? "true" : "false",
            status.suspended ? "true" : "false",
            status.alt,
            status.reading ? "true" : "false",
            (unsigned long)status.packets,
            (unsigned long)status.session_packets,
            (unsigned long)status.packets,
            (unsigned long)status.starts,
            (unsigned long)status.stops,
            (unsigned long)status.errors,
            (unsigned long)status.rx_overflows,
            status.silence ? "silence" : "noise",
            status.level);
        usb_mic_send(&app->usb, reply);
        break;
    case MicCommandReset:
        usb_mic_reset_stats(&app->usb);
        usb_mic_send(&app->usb, "OK counters reset\r\n");
        break;
    case MicCommandNoise:
        usb_mic_set_signal(&app->usb, false, level);
        usb_mic_send(&app->usb, "OK noise\r\n");
        break;
    case MicCommandSilence:
        usb_mic_set_signal(&app->usb, true, level);
        usb_mic_send(&app->usb, "OK silence\r\n");
        break;
    case MicCommandLevel:
        usb_mic_set_signal(&app->usb, status.silence, level);
        usb_mic_send(&app->usb, "OK level set\r\n");
        break;
    case MicCommandReconnect:
        usb_mic_send(&app->usb, "OK reconnecting\r\n");
        furi_delay_ms(50); // Let the host serial reader drain the acknowledgement.
        // Deinit/reinit clears all app callbacks and endpoint state.
        furi_check(furi_hal_usb_set_config(NULL, NULL));
        furi_check(furi_hal_usb_set_config(&usb_dummy_mic, &app->usb));
        break;
    case MicCommandExit:
        usb_mic_send(&app->usb, "OK exiting\r\n");
        furi_delay_ms(50);
        return false;
    default:
        usb_mic_send(&app->usb, "ERR unknown command or invalid argument; use help\r\n");
        break;
    }
    return true;
}

int32_t dummy_mic_app(void* context) {
    // Optional development watchdog also recovers from enumeration mistakes,
    // where no serial command can reach the device. Normal launches stay open.
    bool development = context && strcmp((const char*)context, "dev") == 0;
    MicApp* app = malloc(sizeof(MicApp));
    memset(app, 0, sizeof(*app));
    app->keys = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->usb.rx = furi_stream_buffer_alloc(512, 1);
    app->usb.seed = 0x6d696321;
    app->usb.status.level = MIC_DEFAULT_LEVEL;
    const uint8_t coding[] = {0x00, 0xc2, 0x01, 0x00, 0, 0, 8}; // 115200 8N1
    memcpy(app->usb.line_coding, coding, sizeof(coding));

    FuriHalUsbInterface* previous = furi_hal_usb_get_config();
    // Only these firmware interfaces can be restored without a private context.
    if(previous == &usb_cdc_single || previous == &usb_cdc_dual) {
        app->usb_ready = furi_hal_usb_set_config(&usb_dummy_mic, &app->usb);
    }
    Gui* gui = furi_record_open(RECORD_GUI);
    ViewPort* view = view_port_alloc();
    view_port_draw_callback_set(view, draw, app);
    view_port_input_callback_set(view, input, app);
    gui_add_view_port(gui, view, GuiLayerFullscreen);

    char line[MIC_COMMAND_SIZE];
    size_t used = 0;
    bool discard = false;
    bool running = true;
    bool enumerated = false;
    uint32_t launched = furi_get_tick();
    while(running) {
        MicStatus current;
        usb_mic_snapshot(&app->usb, &current);
        enumerated |= current.configured;
        if(development && !enumerated && furi_get_tick() - launched > furi_ms_to_ticks(15000)) {
            break;
        }
        InputEvent event;
        if(furi_message_queue_get(app->keys, &event, furi_ms_to_ticks(20)) == FuriStatusOk &&
           event.type == InputTypeShort) {
            if(event.key == InputKeyBack) running = false;
            if(event.key == InputKeyOk) usb_mic_reset_stats(&app->usb);
        }
        if(usb_mic_take_overflow(&app->usb)) {
            used = 0;
            discard = true;
            usb_mic_send(&app->usb, "ERR input overflow; send newline then retry\r\n");
        }
        uint8_t ch;
        // Limit work per loop so flooding serial cannot starve buttons/UI.
        for(size_t n = 0;
            running && n < 128 && furi_stream_buffer_receive(app->usb.rx, &ch, 1, 0) == 1;
            n++) {
            if(ch == '\r' || ch == '\n') {
                if(discard) {
                    usb_mic_send(&app->usb, "ERR line discarded\r\n");
                } else if(used) {
                    line[used] = 0;
                    running = command(app, line);
                }
                used = 0;
                discard = false;
            } else if(ch == 8 || ch == 127) {
                if(used) used--;
            } else if(!discard) {
                if(ch < 32 || ch > 126 || used == sizeof(line) - 1) {
                    discard = true;
                } else {
                    line[used++] = ch;
                }
            }
        }
        view_port_update(view);
    }

    // Mode switch is synchronous: no callbacks may point into the unloaded FAP.
    if(app->usb_ready) furi_check(furi_hal_usb_set_config(previous, NULL));
    view_port_enabled_set(view, false);
    gui_remove_view_port(gui, view);
    view_port_free(view);
    furi_record_close(RECORD_GUI);
    furi_stream_buffer_free(app->usb.rx);
    furi_message_queue_free(app->keys);
    free(app);
    return 0;
}
