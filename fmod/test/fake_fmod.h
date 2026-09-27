/*
 * A stand-in for FMOD's side of the codec plugin API: file functions over a memory buffer, allocation
 * accounting, and tag capture. The codec runs unmodified against it.
 */
#ifndef FAKE_FMOD_H
#define FAKE_FMOD_H

#include "fmod.h"

typedef struct fake_file {
    FMOD_CODEC_STATE           state;        /* first member: callbacks cast the state back to fake_file */
    FMOD_CODEC_STATE_FUNCTIONS functions;
    const unsigned char*       data;
    unsigned int               size;
    unsigned int               pos;
    int                        sizeUnknown;  /* FILE_SIZE reports 0, like a source without Content-Length */
    unsigned int               maxChunk;     /* cap per FILE_READ call (0 = none), like a trickling source */
    FMOD_RESULT                failAtPos;    /* error returned once pos reaches failPos (FMOD_OK = never) */
    unsigned int               failPos;
    unsigned int               seeks;
    unsigned int               reads;
    int                        sampleRateTags;
    float                      lastSampleRate;
    int                        lengthTags;       /* AMPAAC_LENGTH_TAG count and last value (ms) */
    unsigned int               lengthTagMs;
} fake_file;

void fake_file_init(fake_file* file, const unsigned char* data, unsigned int size);

/* Allocations still live (FMOD_CODEC_ALLOC minus FMOD_CODEC_FREE) across every fake_file. */
long fake_live_allocations(void);

#endif
