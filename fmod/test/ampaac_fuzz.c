/*
 * libFuzzer target: arbitrary bytes as the file FMOD hands the codec, after three control bytes (source
 * quirks, seek targets, a mid-transfer file fault). Exercises the probe, the ADTS walk and resync, the MP4
 * box parser and sample tables, decoding through the drain to the end, seeks (header walk, estimate,
 * sync-sample pre-roll, after the end), file faults and close, under the sanitizers.
 * Built and run by test/fuzz.sh.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fake_fmod.h"
#include "ampaac.h"

F_EXPORT FMOD_CODEC_DESCRIPTION* F_CALL AMPAAC_GetCodecDescription(void);

#define CHUNK      2048
#define MAX_FRAMES (1u << 20)   /* per input: about 24 s at 44.1 kHz, enough to reach the drain and EOF */

static short pcm[CHUNK * AMPAAC_MAX_CHANNELS];

/* A seek target steered by six control bits. */
static unsigned int steered(unsigned int bits, int salt) {
    return (bits & 63u) * 997u * (unsigned int)(1 + salt);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FMOD_CODEC_DESCRIPTION* codec = AMPAAC_GetCodecDescription();
    fake_file               file;
    unsigned int            control;
    unsigned int            fault;
    unsigned int            frames = 0;
    int                     reads;
    int                     afterEnd = 0;

    if (size < 3 || size > (1u << 22)) {
        return 0;
    }
    /* The first three bytes steer the harness; the rest is the file. */
    control = (unsigned int)data[0] | ((unsigned int)data[1] << 8);
    fault   = data[2];
    fake_file_init(&file, data + 3, (unsigned int)(size - 3));
    file.sizeUnknown = (control & 1) != 0;
    file.maxChunk    = (control & 2) ? 1 + (control >> 8) : 0;
    if (fault) {
        /* A file error from a steered offset on, like a source that fails mid-transfer. */
        file.failAtPos = FMOD_ERR_FILE_BAD;
        file.failPos   = (unsigned int)((unsigned long long)(size - 3) * fault / 256u);
    }
    /* Deterministic seek walks: none (the estimate path) or all the way to the target. */
    ampaac_hop_budget_ms = (control & 4) ? 0 : 0xFFFFFFFFu;

    if (codec->open(&file.state, FMOD_CREATESTREAM | ((control & 8) ? FMOD_ACCURATETIME : 0), NULL) != FMOD_OK) {
        if (fake_live_allocations() != 0) {
            abort();   /* a failed open leaked */
        }
        return 0;
    }
    codec->setposition(&file.state, 0, 0, FMOD_TIMEUNIT_PCM);
    for (reads = 0; frames < MAX_FRAMES; reads++) {
        unsigned int got = 0;
        FMOD_RESULT  res;

        if (reads == 5 || reads == 11) {
            codec->setposition(&file.state, 0, steered(control >> (reads == 5 ? 4 : 10), reads), FMOD_TIMEUNIT_PCM);
        }
        res = codec->read(&file.state, pcm, CHUNK, &got);
        if (got > CHUNK) {
            abort();   /* the codec wrote past what FMOD asked for */
        }
        frames += got;
        if (res == FMOD_ERR_FILE_EOF && !afterEnd) {
            /* A seek after the end re-primes the drained decoder. */
            afterEnd = 1;
            codec->setposition(&file.state, 0, steered(control >> 4, 3), FMOD_TIMEUNIT_PCM);
            continue;
        }
        if (res != FMOD_OK) {
            break;
        }
        if (got == 0) {
            abort();   /* FMOD_OK with nothing would spin FMOD's stream thread */
        }
    }
    codec->close(&file.state);
    if (fake_live_allocations() != 0) {
        abort();   /* leak across open/close */
    }
    return 0;
}
