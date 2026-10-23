#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIC_RATE               16000u
#define MIC_SAMPLES_PER_PACKET (MIC_RATE / 1000u)
#define MIC_PACKET_BYTES       (MIC_SAMPLES_PER_PACKET * 2u)
#define MIC_DEFAULT_LEVEL      128u
#define MIC_MAX_LEVEL          4096u
#define MIC_COMMAND_SIZE       64u

typedef enum {
    MicCommandInvalid,
    MicCommandHelp,
    MicCommandPing,
    MicCommandStatus,
    MicCommandReset,
    MicCommandNoise,
    MicCommandSilence,
    MicCommandLevel,
    MicCommandReconnect,
    MicCommandExit,
} MicCommand;

MicCommand mic_parse_command(char* line, uint16_t* level);
void mic_generate(int16_t* samples, uint32_t* seed, uint16_t level, bool silence);
