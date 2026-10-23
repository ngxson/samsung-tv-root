#include "mic_core.h"
#include <string.h>

MicCommand mic_parse_command(char* line, uint16_t* level) {
    while(*line == ' ' || *line == '\t')
        line++;
    size_t length = strlen(line);
    while(length && (line[length - 1] == ' ' || line[length - 1] == '\t')) {
        line[--length] = 0;
    }
    static const struct {
        const char* text;
        MicCommand command;
    } commands[] = {
        {"help", MicCommandHelp},
        {"ping", MicCommandPing},
        {"status", MicCommandStatus},
        {"reset", MicCommandReset},
        {"noise", MicCommandNoise},
        {"silence", MicCommandSilence},
        {"reconnect", MicCommandReconnect},
        {"exit", MicCommandExit},
    };
    for(size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if(strcmp(line, commands[i].text) == 0) return commands[i].command;
    }
    if(strncmp(line, "level ", 6) == 0) {
        const char* p = line + 6;
        while(*p == ' ')
            p++;
        if(!*p) return MicCommandInvalid;
        uint32_t value = 0;
        for(; *p; p++) {
            if(*p < '0' || *p > '9') return MicCommandInvalid;
            value = value * 10 + (*p - '0');
            if(value > MIC_MAX_LEVEL) return MicCommandInvalid;
        }
        *level = value;
        return MicCommandLevel;
    }
    return MicCommandInvalid;
}

void mic_generate(int16_t* samples, uint32_t* seed, uint16_t level, bool silence) {
    for(size_t i = 0; i < MIC_SAMPLES_PER_PACKET; i++) {
        uint32_t x = *seed;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        *seed = x;
        // Symmetric, bounded pseudo-random noise; never touches any microphone.
        int32_t centered = (int32_t)(x & 0xffffu) - 32768;
        samples[i] = silence ? 0 : (centered * level) / 32768;
    }
}
