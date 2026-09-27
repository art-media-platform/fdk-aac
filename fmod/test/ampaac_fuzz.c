/*
 * libFuzzer target: arbitrary bytes as the file FMOD hands the codec. Exercises the probe, the ADTS walk
 * and resync, decoding, seeks (header walk and estimate) and close, under the sanitizers.
 * Built and run by test/fuzz.sh.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fake_fmod.h"
#include "ampaac.h"

F_EXPORT FMOD_CODEC_DESCRIPTION* F_CALL AMPAAC_GetCodecDescription(void);

#define MAX_READS 16
#define CHUNK     2048

static short pcm[CHUNK * AMPAAC_MAX_CHANNELS];

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FMOD_CODEC_DESCRIPTION* codec = AMPAAC_GetCodecDescription();
    fake_file               file;
    unsigned int            control;
    int                     reads;

    if (size < 2 || size > (1u << 22)) {
        return 0;
    }
    /* The first two bytes steer the harness; the rest is the file. */
    control = (unsigned int)data[0] | ((unsigned int)data[1] << 8);
    fake_file_init(&file, data + 2, (unsigned int)(size - 2));
    file.sizeUnknown = (control & 1) != 0;
    file.maxChunk    = (control & 2) ? 1 + (control >> 8) : 0;
    ampaac_hop_budget_ms = (control & 4) ? 0 : AMPAAC_HOP_BUDGET_MS;

    if (codec->open(&file.state, FMOD_CREATESTREAM | ((control & 8) ? FMOD_ACCURATETIME : 0), NULL) != FMOD_OK) {
        return 0;
    }
    codec->setposition(&file.state, 0, 0, FMOD_TIMEUNIT_PCM);
    for (reads = 0; reads < MAX_READS; reads++) {
        unsigned int got = 0;
        FMOD_RESULT  res;

        if (reads == MAX_READS / 3 || reads == 2 * MAX_READS / 3) {
            /* Seek somewhere steered by the control bits: back, near, far. */
            unsigned int target = (control >> (reads == MAX_READS / 3 ? 4 : 10)) * 997u * (unsigned int)(1 + reads);
            codec->setposition(&file.state, 0, target, FMOD_TIMEUNIT_PCM);
        }
        res = codec->read(&file.state, pcm, CHUNK, &got);
        if (got > CHUNK) {
            abort();   /* the codec wrote past what FMOD asked for */
        }
        if (res != FMOD_OK) {
            break;
        }
    }
    codec->close(&file.state);
    if (fake_live_allocations() != 0) {
        abort();   /* leak across open/close */
    }
    return 0;
}
