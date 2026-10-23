// Targeted CoreAudio capture: no PortAudio, default device, or AudioUnit setup.
// Build: clang -std=c11 -Wall -Wextra -Werror tools/macos_capture.c \
//        -framework CoreAudio -framework CoreFoundation -o dist/macos_capture
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t interrupted;

typedef struct {
    AudioDeviceID device;
    bool floating;
    uint32_t target;
    _Atomic uint32_t frames;
    _Atomic bool invalid_buffer;
    double squares;
    double peak;
} Capture;

static void on_signal(int number) {
    (void)number;
    interrupted = 1;
}

static OSStatus property(AudioObjectID object, AudioObjectPropertySelector selector,
                         AudioObjectPropertyScope scope, void* value, UInt32* size) {
    AudioObjectPropertyAddress address = {selector, scope, kAudioObjectPropertyElementMain};
    return AudioObjectGetPropertyData(object, &address, 0, NULL, size, value);
}

static bool string_property(AudioObjectID object, AudioObjectPropertySelector selector,
                            char* output, size_t capacity) {
    CFStringRef value = NULL;
    UInt32 size = sizeof(value);
    if(property(object, selector, kAudioObjectPropertyScopeGlobal, &value, &size) || !value) {
        return false;
    }
    bool ok = CFStringGetCString(value, output, capacity, kCFStringEncodingUTF8);
    CFRelease(value);
    return ok;
}

static AudioDeviceID find_flipper(void) {
    AudioObjectPropertyAddress address = {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, NULL, &size)) return 0;
    AudioDeviceID* devices = calloc(1, size);
    if(!devices) return 0;
    if(AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, devices)) {
        free(devices);
        return 0;
    }
    AudioDeviceID found = 0;
    unsigned matches = 0;
    for(size_t i = 0; i < size / sizeof(*devices); i++) {
        UInt32 transport = 0, length = sizeof(transport);
        // Query only transport metadata on other devices. Never open their IO.
        if(property(devices[i], kAudioDevicePropertyTransportType,
                    kAudioObjectPropertyScopeGlobal, &transport, &length) ||
           transport != kAudioDeviceTransportTypeUSB) continue;
        char name[256];
        if(!string_property(devices[i], kAudioObjectPropertyName, name, sizeof(name)) ||
           strcmp(name, "Flipper Dummy Mic")) continue;
        found = devices[i];
        matches++;
    }
    free(devices);
    return matches == 1 ? found : 0;
}

static OSStatus receive(AudioObjectID device, const AudioTimeStamp* now,
                        const AudioBufferList* input, const AudioTimeStamp* input_time,
                        AudioBufferList* output, const AudioTimeStamp* output_time, void* context) {
    (void)now;
    (void)input_time;
    (void)output;
    (void)output_time;
    Capture* capture = context;
    uint32_t done = atomic_load_explicit(&capture->frames, memory_order_relaxed);
    if(done >= capture->target) return noErr;
    if(device != capture->device || !input || input->mNumberBuffers != 1 ||
       input->mBuffers[0].mNumberChannels != 1 || !input->mBuffers[0].mData) {
        atomic_store(&capture->invalid_buffer, true);
        return noErr;
    }
    const AudioBuffer* buffer = &input->mBuffers[0];
    size_t stride = capture->floating ? sizeof(float) : sizeof(int16_t);
    if(buffer->mDataByteSize % stride) {
        atomic_store(&capture->invalid_buffer, true);
        return noErr;
    }
    uint32_t count = buffer->mDataByteSize / stride;
    if(count > capture->target - done) count = capture->target - done;
    for(uint32_t i = 0; i < count; i++) {
        double value = capture->floating ? ((const float*)buffer->mData)[i] * 32768.0 :
                                          ((const int16_t*)buffer->mData)[i];
        if(!isfinite(value)) {
            atomic_store(&capture->invalid_buffer, true);
            return noErr;
        }
        capture->squares += value * value;
        if(fabs(value) > capture->peak) capture->peak = fabs(value);
    }
    atomic_store_explicit(&capture->frames, done + count, memory_order_release);
    return noErr;
}

int main(int argc, char** argv) {
    char* end = NULL;
    long seconds = argc == 2 ? strtol(argv[1], &end, 10) : 0;
    if(seconds < 1 || seconds > 60 || !end || *end) {
        fprintf(stderr, "Usage: %s SECONDS (1..60)\n", argv[0]);
        return 2;
    }
    AudioDeviceID device = find_flipper();
    if(!device) {
        fprintf(stderr, "Expected exactly one USB device named Flipper Dummy Mic; refusing capture\n");
        return 1;
    }
    char uid[256];
    if(!string_property(device, kAudioDevicePropertyDeviceUID, uid, sizeof(uid)) ||
       !strstr(uid, "dummy_mic_001")) {
        fprintf(stderr, "Flipper USB serial not present in audio UID; refusing capture\n");
        return 1;
    }
    AudioStreamID streams[2];
    UInt32 size = sizeof(streams);
    if(property(device, kAudioDevicePropertyStreams, kAudioDevicePropertyScopeInput,
                streams, &size) || size != sizeof(AudioStreamID)) {
        fprintf(stderr, "Expected one Flipper input stream\n");
        return 1;
    }
    AudioStreamBasicDescription format = {0};
    size = sizeof(format);
    if(property(streams[0], kAudioStreamPropertyVirtualFormat,
                kAudioObjectPropertyScopeGlobal, &format, &size)) {
        fprintf(stderr, "Cannot read Flipper stream format\n");
        return 1;
    }
    bool floating = (format.mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    bool signed_int = (format.mFormatFlags & kAudioFormatFlagIsSignedInteger) != 0;
    if(format.mFormatID != kAudioFormatLinearPCM || format.mSampleRate != 16000 ||
       format.mChannelsPerFrame != 1 || (format.mFormatFlags & kAudioFormatFlagIsBigEndian) ||
       !(format.mFormatFlags & kAudioFormatFlagIsPacked) ||
       (floating ? (format.mBitsPerChannel != 32 || format.mBytesPerFrame != 4) :
                   (!signed_int || format.mBitsPerChannel != 16 || format.mBytesPerFrame != 2))) {
        fprintf(stderr, "Unsupported Flipper format: %.0f Hz, %u channels, %u bits, flags=%u\n",
                format.mSampleRate, format.mChannelsPerFrame, format.mBitsPerChannel, format.mFormatFlags);
        return 1;
    }
    fprintf(stderr, "Capturing Flipper Dummy Mic: USB, ID=%u, UID=%s, 16000Hz mono, %lds\n",
            device, uid, seconds);
    Capture capture = {.device = device, .floating = floating, .target = seconds * 16000};
    AudioDeviceIOProcID callback = NULL;
    OSStatus error = AudioDeviceCreateIOProcID(device, receive, &capture, &callback);
    if(error) {
        fprintf(stderr, "Create Flipper IO callback failed: %d\n", (int)error);
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    error = AudioDeviceStart(device, callback);
    if(!error) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
        unsigned waited = 0;
        while(!interrupted && !atomic_load(&capture.invalid_buffer) &&
              atomic_load(&capture.frames) < capture.target && waited++ < (seconds + 5) * 10) {
            nanosleep(&pause, NULL);
        }
        OSStatus stopped = AudioDeviceStop(device, callback);
        if(stopped) error = stopped;
    }
    OSStatus destroyed = AudioDeviceDestroyIOProcID(device, callback);
    if(destroyed && !error) error = destroyed;
    uint32_t frames = atomic_load(&capture.frames);
    if(error || interrupted || atomic_load(&capture.invalid_buffer) || frames < capture.target) {
        fprintf(stderr, "Capture incomplete: status=%d frames=%u/%u interrupted=%d invalid=%d\n",
                (int)error, frames, capture.target, (int)interrupted, atomic_load(&capture.invalid_buffer));
        return 1;
    }
    printf("{\"device\":\"Flipper Dummy Mic\",\"transport\":\"USB\",\"device_id\":%u,"
           "\"frames\":%u,\"seconds\":%ld,\"rms\":%.6f,\"peak\":%.6f}\n",
           device, frames, seconds, sqrt(capture.squares / frames), capture.peak);
    return 0;
}
