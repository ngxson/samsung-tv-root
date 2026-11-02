#include "hid_usages.h"

#include <stddef.h>
#include <stdio.h>
#include "hid_parser.h"

typedef struct {
    uint16_t id;
    const char *name;
} usage_name_t;

static const usage_name_t generic_desktop[] = {
    {0x30, "X"}, {0x31, "Y"}, {0x32, "Z"}, {0x33, "Rx"}, {0x34, "Ry"}, {0x35, "Rz"},
    {0x38, "Wheel"},
    {0x81, "SystemPowerDown"}, {0x82, "SystemSleep"}, {0x83, "SystemWakeUp"},
    {0x84, "SystemContextMenu"}, {0x85, "SystemMainMenu"}, {0x86, "SystemAppMenu"},
    {0x89, "SystemMenuSelect"}, {0x8a, "SystemMenuRight"}, {0x8b, "SystemMenuLeft"},
    {0x8c, "SystemMenuUp"}, {0x8d, "SystemMenuDown"},
    {0x90, "DpadUp"}, {0x91, "DpadDown"}, {0x92, "DpadRight"}, {0x93, "DpadLeft"},
};

static const usage_name_t keyboard[] = {
    {0x28, "Enter"}, {0x29, "Escape"}, {0x2a, "Backspace"}, {0x2b, "Tab"}, {0x2c, "Space"},
    {0x2d, "Minus"}, {0x2e, "Equal"}, {0x37, "Period"},
    {0x3a, "F1"}, {0x3b, "F2"}, {0x3c, "F3"}, {0x3d, "F4"}, {0x3e, "F5"}, {0x3f, "F6"},
    {0x40, "F7"}, {0x41, "F8"}, {0x42, "F9"}, {0x43, "F10"}, {0x44, "F11"}, {0x45, "F12"},
    {0x4a, "Home"}, {0x4b, "PageUp"}, {0x4c, "Delete"}, {0x4d, "End"}, {0x4e, "PageDown"},
    {0x4f, "Right"}, {0x50, "Left"}, {0x51, "Down"}, {0x52, "Up"},
    {0x65, "Application"}, {0x66, "Power"},
    {0x7f, "Mute"}, {0x80, "VolumeUp"}, {0x81, "VolumeDown"},
    {0xe0, "LeftCtrl"}, {0xe1, "LeftShift"}, {0xe2, "LeftAlt"}, {0xe3, "LeftGUI"},
    {0xe4, "RightCtrl"}, {0xe5, "RightShift"}, {0xe6, "RightAlt"}, {0xe7, "RightGUI"},
};

static const usage_name_t consumer[] = {
    {0x30, "Power"}, {0x31, "Reset"}, {0x32, "Sleep"}, {0x34, "SleepMode"},
    {0x40, "Menu"}, {0x41, "MenuPick"}, {0x42, "MenuUp"}, {0x43, "MenuDown"},
    {0x44, "MenuLeft"}, {0x45, "MenuRight"}, {0x46, "MenuEscape"},
    {0x60, "DataOnScreen"}, {0x61, "ClosedCaption"}, {0x64, "BroadcastMode"},
    {0x65, "Snapshot"}, {0x69, "Red"}, {0x6a, "Green"}, {0x6b, "Blue"}, {0x6c, "Yellow"},
    {0x6f, "BrightnessUp"}, {0x70, "BrightnessDown"},
    {0x81, "AssignSelection"}, {0x82, "ModeStep"}, {0x83, "RecallLast"},
    {0x88, "MediaSelectComputer"}, {0x89, "MediaSelectTV"}, {0x8d, "MediaSelectProgramGuide"},
    {0x94, "Quit"}, {0x9c, "ChannelUp"}, {0x9d, "ChannelDown"},
    {0xb0, "Play"}, {0xb1, "Pause"}, {0xb2, "Record"}, {0xb3, "FastForward"},
    {0xb4, "Rewind"}, {0xb5, "NextTrack"}, {0xb6, "PrevTrack"}, {0xb7, "Stop"},
    {0xb8, "Eject"}, {0xcd, "PlayPause"}, {0xcf, "VoiceCommand"},
    {0xe2, "Mute"}, {0xe9, "VolumeUp"}, {0xea, "VolumeDown"},
    {0x173, "AudioTrack"}, {0x174, "Subtitle"},
    {0x183, "AL_ConsumerControlConfig"}, {0x18a, "AL_Email"}, {0x192, "AL_Calculator"},
    {0x194, "AL_LocalBrowser"}, {0x1a2, "AL_SelectTaskApp"},
    {0x221, "AC_Search"}, {0x223, "AC_Home"}, {0x224, "AC_Back"}, {0x225, "AC_Forward"},
    {0x226, "AC_Stop"}, {0x227, "AC_Refresh"}, {0x22a, "AC_Bookmarks"},
    {0x232, "AC_ZoomIn"}, {0x233, "AC_ZoomOut"}, {0x23d, "AC_Edit"},
};

#define TABLE(t) t, sizeof(t) / sizeof(t[0])

static const char *lookup(const usage_name_t *t, size_t n, uint16_t id)
{
    for (size_t i = 0; i < n; i++) {
        if (t[i].id == id) {
            return t[i].name;
        }
    }
    return NULL;
}

const char *hid_page_name(uint16_t page)
{
    switch (page) {
    case 0x01: return "GenericDesktop";
    case 0x07: return "Keyboard";
    case 0x08: return "LED";
    case 0x09: return "Button";
    case 0x0c: return "Consumer";
    case 0x0d: return "Digitizer";
    case 0x20: return "Sensor";
    default:   return page >= HID_PAGE_VENDOR_MIN ? "Vendor" : "Other";
    }
}

const char *hid_usage_name(uint32_t usage)
{
    static char buf[16];
    uint16_t page = HID_USAGE_PAGE(usage);
    uint16_t id = HID_USAGE_ID(usage);

    switch (page) {
    case 0x01:
        return lookup(TABLE(generic_desktop), id);
    case 0x07:
        if (id >= 0x04 && id <= 0x1d) {
            snprintf(buf, sizeof(buf), "%c", 'A' + (id - 0x04));
            return buf;
        }
        if (id >= 0x1e && id <= 0x27) {
            snprintf(buf, sizeof(buf), "%d", (id - 0x1e + 1) % 10);
            return buf;
        }
        return lookup(TABLE(keyboard), id);
    case 0x09:
        snprintf(buf, sizeof(buf), "Button%u", id);
        return buf;
    case 0x0c:
        return lookup(TABLE(consumer), id);
    default:
        return NULL;
    }
}
