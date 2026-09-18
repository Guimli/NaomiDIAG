/* Auto-generated stub for AUDIO=0 builds -- no PCM data embedded.
 * Keeps the CLIP_* enum and the audio_clips[] table so the report code
 * compiles unchanged; every clip is empty and aica_say() no-ops on a zero
 * sample count, so nothing is spoken and the ROM carries no speech at all.
 * Regenerate with: make AUDIO=0 (the Makefile writes this into audio_clips.h). */
#ifndef AUDIO_CLIPS_H
#define AUDIO_CLIPS_H

/* len is a sample count (ADPCM), matching the generated headers. */
typedef struct { const signed char *pcm; unsigned len; } audio_clip;

enum {
    CLIP_TESTS_DONE,
    CLIP_AUDIO_OK,
    CLIP_CPU_CACHE,
    CLIP_CPU_RAM,
    CLIP_DATA_BUS,
    CLIP_ADDR_BUS,
    CLIP_SOUND_RAM,
    CLIP_OK,
    CLIP_FAIL,
    CLIP_DEFECT,
    CLIP_NUM_1,
    CLIP_NUM_2,
    CLIP_NUM_3,
    CLIP_NUM_4,
    CLIP_NUM_5,
    CLIP_NUM_6,
    CLIP_NUM_7,
    CLIP_NUM_8,
    CLIP_IC_9,
    CLIP_IC_10,
    CLIP_IC_11,
    CLIP_IC_12,
    CLIP_IC_16,
    CLIP_IC_18,
    CLIP_IC_20,
    CLIP_IC_22,
    CLIP_IC_29,
    CLIP_IC_35,
    CLIP_IC_27,
    CLIP_BIOS,
    CLIP_VRAM,
    CLIP_SRAM,
    CLIP_RTC,
    CLIP_DIMM,
    CLIP_ABSENT,
    CLIP_PRESENT,
    CLIP_JVS,
    CLIP_NAOMI1,
    CLIP_NAOMI2,
    CLIP_BOARD_UNK,
    CLIP_VRAM_B,
    CLIP_ELAN,
    CLIP_EEPROM,
    CLIP_X76,
    CLIP_CART,
    CLIP_COUNT
};

/* All entries {NULL, 0}: the zero initializer covers every element. */
static const audio_clip audio_clips[CLIP_COUNT] = { { 0, 0 } };

#endif
