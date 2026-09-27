/*
 * ampaac — FMOD codec plugin: AAC (ADTS; MP4 to follow) decoded by the Fraunhofer FDK AAC decoder.
 *
 * FMOD opens every sound through its codec list in priority order; open() claims only streams it can
 * decode and answers FMOD_ERR_FORMAT for everything else, leaving no side effects.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "ampaac.h"

#ifndef AMPAAC_SOURCE_REV
#define AMPAAC_SOURCE_REV "unknown"
#endif

static const char ampaacIdent[] = "ampaac fdk-aac " AMPAAC_SOURCE_REV;

#ifdef AMPAAC_SPIKE
/* Spike builds trace the callbacks FMOD makes and can skew the reported length (AMPAAC_LENGTH_SCALE). */
#define TRACE(...) do { fprintf(stderr, "ampaac: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#else
#define TRACE(...) do { } while (0)
#endif

#define WARN(state, ...) FMOD_CODEC_LOG((state), FMOD_DEBUG_LEVEL_WARNING, "ampaac", __VA_ARGS__)

static FMOD_RESULT F_CALLBACK codec_open(FMOD_CODEC_STATE* state, FMOD_MODE usermode, FMOD_CREATESOUNDEXINFO* exinfo);
static FMOD_RESULT F_CALLBACK codec_close(FMOD_CODEC_STATE* state);
static FMOD_RESULT F_CALLBACK codec_read(FMOD_CODEC_STATE* state, void* buffer, unsigned int samplesIn, unsigned int* samplesOut);
static FMOD_RESULT F_CALLBACK codec_getlength(FMOD_CODEC_STATE* state, unsigned int* length, FMOD_TIMEUNIT unit);
static FMOD_RESULT F_CALLBACK codec_setposition(FMOD_CODEC_STATE* state, int subsound, unsigned int position, FMOD_TIMEUNIT unit);

static FMOD_CODEC_DESCRIPTION ampaacDescription = {
    FMOD_CODEC_PLUGIN_VERSION,
    "AMP AAC (fdk-aac)",
    0x00010000,
    0,                   /* defaultasstream */
    FMOD_TIMEUNIT_PCM,
    codec_open,
    codec_close,
    codec_read,
    codec_getlength,
    codec_setposition,
    0,                   /* getposition: FMOD tracks position */
    0,                   /* soundcreate */
    0                    /* getwaveformat: one waveformat, published through codec_state->waveformat */
};

F_EXPORT FMOD_CODEC_DESCRIPTION* F_CALL AMPAAC_GetCodecDescription(void) {
    return &ampaacDescription;
}

#ifndef AMPAAC_STATIC
/* FMOD's standard entry point, so System::loadPlugin can load the shared library directly. */
F_EXPORT FMOD_CODEC_DESCRIPTION* F_CALL FMODGetCodecDescription(void) {
    return &ampaacDescription;
}
#endif

/* ID3v2 tags ahead of ADTS: skipped by their synchsafe size. */
static void skip_id3v2(ampaac_reader* rd) {
    int tags;

    for (tags = 0; tags < 4; tags++) {
        const unsigned char* p;
        unsigned int         size;

        if (ampaac_reader_peek(rd, 10, &p) < 10) {
            return;
        }
        if (p[0] != 'I' || p[1] != 'D' || p[2] != '3' || p[3] == 0xFF || p[4] == 0xFF
            || ((p[6] | p[7] | p[8] | p[9]) & 0x80)) {
            return;
        }
        size = ((unsigned int)p[6] << 21) | ((unsigned int)p[7] << 14) | ((unsigned int)p[8] << 7) | p[9];
        size += 10;
        if (p[3] >= 4 && (p[5] & 0x10)) {
            size += 10;   /* footer */
        }
        ampaac_reader_skip(rd, size);
    }
}

/* Formats FMOD's own codecs claim by magic; answering FORMAT at once spares the ADTS scan. */
static int foreign_magic(const unsigned char* p, unsigned int avail) {
    static const char* const magics[] = { "RIFF", "RIFX", "OggS", "fLaC", "FORM", "MThd", "FSB5", "#EXT", "[pla" };
    unsigned int i;

    if (avail < 4) {
        return 0;
    }
    for (i = 0; i < sizeof(magics) / sizeof(magics[0]); i++) {
        if (memcmp(p, magics[i], 4) == 0) {
            return 1;
        }
    }
    return 0;
}

static FMOD_RESULT next_au(ampaac_codec* aac, unsigned int* auLen) {
    return ampaac_adts_next(aac, auLen);
}

static void update_length(ampaac_codec* aac) {
    ampaac_adts_update_length(aac);
}

static void channel_mask(ampaac_codec* aac) {
    switch (aac->channels) {
        case 1:  aac->waveformat.channelmask = FMOD_CHANNELMASK_MONO;    break;
        case 2:  aac->waveformat.channelmask = FMOD_CHANNELMASK_STEREO;  break;
        case 6:  aac->waveformat.channelmask = FMOD_CHANNELMASK_5POINT1; break;
        case 8:  aac->waveformat.channelmask = FMOD_CHANNELMASK_7POINT1; break;
        default: aac->waveformat.channelmask = 0;                        break;
    }
}

/*
 * Decodes one access unit into the PCM carry, feeding the decoder as it asks for bits.
 * Returns FMOD_OK with pcmFrames > 0, FMOD_ERR_FILE_EOF once the stream is drained, or a file error.
 */
static FMOD_RESULT decode_next(FMOD_CODEC_STATE* state, ampaac_codec* aac) {
    int conceal  = 0;
    int failures = 0;

    for (;;) {
        AAC_DECODER_ERROR err;
        CStreamInfo*      info;
        unsigned int      frames;
        unsigned int      flags = aac->decodeFlags | (conceal ? AACDEC_CONCEAL : 0);

        err = aacDecoder_DecodeFrame(aac->decoder, aac->pcm, AMPAAC_PCM_CAP, flags);

        if (err == AAC_DEC_NOT_ENOUGH_BITS && !conceal) {
            unsigned int auLen = 0;
            FMOD_RESULT  res;

            /* Encoders pad the last frame, so no AACDEC_FLUSH drain: the decoded count stays the
               frame count times the frame size, which is what the length reports. */
            if (aac->exhausted) {
                return FMOD_ERR_FILE_EOF;
            }
            res = next_au(aac, &auLen);
            if (res == FMOD_ERR_FILE_EOF) {
                aac->exhausted = 1;
                if (aac->exact) {
                    aac->lengthPcm   = aac->decodedPcm;   /* the true end is known now */
                    aac->lengthExact = 1;
                }
                continue;
            }
            if (res != FMOD_OK) {
                return res;
            }
            {
                UCHAR* ptr   = aac->au;
                UINT   size  = auLen;
                UINT   valid = auLen;
                aacDecoder_Fill(aac->decoder, &ptr, &size, &valid);
            }
            continue;
        }

        if (err != AAC_DEC_NOT_ENOUGH_BITS) {
            aac->decodeFlags = 0;   /* AACDEC_INTR takes effect only on a decode that found its bits */
        }
        if (!IS_OUTPUT_VALID(err)) {
            if (IS_INIT_ERROR(err) || err == AAC_DEC_OUT_OF_MEMORY || err == AAC_DEC_INVALID_HANDLE
                || ++failures > 16) {
                WARN(state, "decoder error 0x%x; ending the stream", (unsigned int)err);
                aac->exhausted = 1;
                return FMOD_ERR_FILE_EOF;
            }
            /* Transport or bitstream loss: one concealed frame keeps time, then fresh input. */
            conceal = !conceal;
            continue;
        }
        conceal  = 0;
        failures = 0;

        info = aacDecoder_GetStreamInfo(aac->decoder);
        if (!info || info->frameSize <= 0 || info->numChannels <= 0) {
            continue;
        }
        if (info->numChannels != aac->channels || (unsigned int)info->frameSize * info->numChannels > AMPAAC_PCM_CAP) {
            continue;   /* pinned channels make this a transient; never hand FMOD a mismatched frame */
        }
        if (info->sampleRate != aac->sampleRate && info->sampleRate > 0) {
            float rate = (float)info->sampleRate;
            if (!aac->started) {
                continue;   /* start-up transient of the fresh decoder; the declared rate stands */
            }
            aac->sampleRate = info->sampleRate;
            FMOD_CODEC_METADATA(state, FMOD_TAGTYPE_FMOD, (char*)"Sample Rate Change", &rate, sizeof(rate),
                                FMOD_TAGDATATYPE_FLOAT, 1);
        }
        aac->frameSize = info->frameSize;
        aac->started   = 1;

        frames = (unsigned int)info->frameSize;
        aac->decodedPcm += frames;
        if (aac->discard >= frames) {
            aac->discard -= frames;
            continue;
        }
        aac->pcmNext   = aac->discard;
        aac->pcmFrames = frames;
        aac->discard   = 0;
        return FMOD_OK;
    }
}

static HANDLE_AACDECODER open_decoder(TRANSPORT_TYPE transport, int channels) {
    HANDLE_AACDECODER decoder = aacDecoder_Open(transport, 1);

    if (!decoder) {
        return 0;
    }
    /* fdk normalizes to -24 dBFS by default (targetRefLevel 96), which attenuates streams carrying a program
       reference level; -1 decodes at the encoded level, like every other FMOD codec. */
    aacDecoder_SetParam(decoder, AAC_DRC_REFERENCE_LEVEL, -1);
    if (channels > 0) {
        aacDecoder_SetParam(decoder, AAC_PCM_MIN_OUTPUT_CHANNELS, channels);
        aacDecoder_SetParam(decoder, AAC_PCM_MAX_OUTPUT_CHANNELS, channels);
    }
    return decoder;
}

/* Decodes the first access units to learn the output format (implicit SBR/PS changes rate and channels). */
static FMOD_RESULT learn_format(FMOD_CODEC_STATE* state, ampaac_codec* aac) {
    int decoded = 0;
    int attempts;

    for (attempts = 0; attempts < 64 && decoded < AMPAAC_OPEN_AUS; attempts++) {
        AAC_DECODER_ERROR err = aacDecoder_DecodeFrame(aac->decoder, aac->pcm, AMPAAC_PCM_CAP, 0);
        CStreamInfo*      info;

        if (err == AAC_DEC_NOT_ENOUGH_BITS) {
            unsigned int auLen = 0;
            FMOD_RESULT  res = next_au(aac, &auLen);
            if (res == FMOD_ERR_FILE_EOF) {
                break;
            }
            if (res != FMOD_OK) {
                return res;
            }
            {
                UCHAR* ptr   = aac->au;
                UINT   size  = auLen;
                UINT   valid = auLen;
                aacDecoder_Fill(aac->decoder, &ptr, &size, &valid);
            }
            continue;
        }
        if (IS_INIT_ERROR(err)) {
            return FMOD_ERR_FORMAT;
        }
        if (err != AAC_DEC_OK) {
            continue;
        }
        info = aacDecoder_GetStreamInfo(aac->decoder);
        if (!info || info->sampleRate <= 0 || info->numChannels <= 0 || info->frameSize <= 0) {
            continue;
        }
        aac->sampleRate = info->sampleRate;
        aac->channels   = info->numChannels;
        aac->frameSize  = info->frameSize;
        decoded++;
    }
    if (decoded == 0 || aac->channels > AMPAAC_MAX_CHANNELS) {
        (void)state;
        return FMOD_ERR_FORMAT;
    }
    return FMOD_OK;
}

static FMOD_RESULT F_CALLBACK codec_open(FMOD_CODEC_STATE* state, FMOD_MODE usermode, FMOD_CREATESOUNDEXINFO* exinfo) {
    ampaac_codec*        aac;
    const unsigned char* head;
    unsigned int         avail;
    FMOD_RESULT          res;

    (void)exinfo;
    aac = (ampaac_codec*)FMOD_CODEC_ALLOC(state, sizeof(ampaac_codec), 16);
    if (!aac) {
        return FMOD_ERR_MEMORY;
    }
    memset(aac, 0, sizeof(*aac));
    state->plugindata = aac;

    FMOD_CODEC_FILE_SEEK(state, 0, FMOD_CODEC_SEEK_METHOD_SET);
    ampaac_reader_init(&aac->reader, state);
    skip_id3v2(&aac->reader);

    avail = ampaac_reader_peek(&aac->reader, 12, &head);
    if (foreign_magic(head, avail) || (avail >= 8 && memcmp(head + 4, "ftyp", 4) == 0)) {
        codec_close(state);
        return FMOD_ERR_FORMAT;
    }

    res = ampaac_adts_open(aac);
    if (res != FMOD_OK) {
        codec_close(state);
        return res;
    }

    /* Probe decode, then restart from the first frame with the channel count pinned. */
    aac->decoder = open_decoder(TT_MP4_ADTS, 0);
    if (!aac->decoder) {
        codec_close(state);
        return FMOD_ERR_MEMORY;
    }
    aac->exact = 1;
    res = learn_format(state, aac);
    aacDecoder_Close(aac->decoder);
    aac->decoder = 0;
    if (res == FMOD_OK) {
        res = ampaac_reader_seek(&aac->reader, aac->dataStart);
    }
    if (res != FMOD_OK) {
        codec_close(state);
        return res;
    }
    aac->decoder = open_decoder(TT_MP4_ADTS, aac->channels);
    if (!aac->decoder) {
        codec_close(state);
        return FMOD_ERR_MEMORY;
    }
    aac->decodedPcm       = 0;
    aac->feedBytes        = 0;
    aac->feedFrames       = 0;
    aac->indexLen         = 0;
    aac->framesSinceIndex = 0;
    aac->exact            = 1;
    aac->pcmPerFrame      = (unsigned int)aac->frameSize;
    update_length(aac);

#ifdef AMPAAC_SPIKE
    {
        const char* scale = getenv("AMPAAC_LENGTH_SCALE");
        if (scale && aac->lengthPcm != AMPAAC_UNKNOWN) {
            aac->lengthPcm = (unsigned int)((double)aac->lengthPcm * atof(scale));
        }
    }
#endif

    aac->waveformat.name         = ampaacIdent;
    aac->waveformat.format       = FMOD_SOUND_FORMAT_PCM16;
    aac->waveformat.channels     = aac->channels;
    aac->waveformat.frequency    = aac->sampleRate;
    aac->waveformat.lengthbytes  = aac->reader.size == AMPAAC_UNKNOWN ? 0xFFFFFFFFu : aac->reader.size;
    aac->waveformat.lengthpcm    = aac->lengthPcm;
    aac->waveformat.pcmblocksize = (unsigned int)aac->frameSize;
    aac->waveformat.channelorder = FMOD_CHANNELORDER_WAVEFORMAT;
    channel_mask(aac);

    state->waveformat   = &aac->waveformat;
    state->numsubsounds = 0;

    TRACE("open adts start=%u size=%u rate=%d ch=%d frame=%d mean=%u lengthpcm=%u mode=0x%x",
          aac->dataStart, aac->reader.size, aac->sampleRate, aac->channels, aac->frameSize,
          aac->meanFrameBytes, aac->lengthPcm, (unsigned int)usermode);
    (void)usermode;
    return FMOD_OK;
}

static FMOD_RESULT F_CALLBACK codec_close(FMOD_CODEC_STATE* state) {
    ampaac_codec* aac = (ampaac_codec*)state->plugindata;

    if (!aac) {
        return FMOD_OK;
    }
    TRACE("close decoded=%u", aac->decodedPcm);
    if (aac->decoder) {
        aacDecoder_Close(aac->decoder);
    }
    if (aac->index) {
        FMOD_CODEC_FREE(state, aac->index);
    }
    FMOD_CODEC_FREE(state, aac);
    state->plugindata = 0;
    return FMOD_OK;
}

static FMOD_RESULT F_CALLBACK codec_read(FMOD_CODEC_STATE* state, void* buffer, unsigned int samplesIn, unsigned int* samplesOut) {
    ampaac_codec* aac = (ampaac_codec*)state->plugindata;
    INT_PCM*      out = (INT_PCM*)buffer;
    unsigned int  produced = 0;
    FMOD_RESULT   res = FMOD_OK;

    while (produced < samplesIn) {
        unsigned int held = aac->pcmFrames - aac->pcmNext;

        if (held > 0) {
            unsigned int take = samplesIn - produced < held ? samplesIn - produced : held;
            memcpy(out + (size_t)produced * aac->channels,
                   aac->pcm + (size_t)aac->pcmNext * aac->channels,
                   (size_t)take * aac->channels * sizeof(INT_PCM));
            aac->pcmNext += take;
            produced     += take;
            continue;
        }
        res = decode_next(state, aac);
        if (res != FMOD_OK) {
            break;
        }
    }
    *samplesOut = produced;

    if (res == FMOD_ERR_FILE_EOF) {
        TRACE("read eof produced=%u decoded=%u length=%u", produced, aac->decodedPcm, aac->lengthPcm);
        return produced > 0 ? FMOD_OK : FMOD_ERR_FILE_EOF;
    }
    if (res != FMOD_OK && produced == 0) {
        return res;
    }
    return FMOD_OK;
}

static FMOD_RESULT F_CALLBACK codec_getlength(FMOD_CODEC_STATE* state, unsigned int* length, FMOD_TIMEUNIT unit) {
    ampaac_codec* aac = (ampaac_codec*)state->plugindata;

#ifndef AMPAAC_SPIKE
    update_length(aac);
#endif
    TRACE("getlength unit=0x%x pcm=%u", (unsigned int)unit, aac->lengthPcm);
    switch (unit) {
        case FMOD_TIMEUNIT_PCM:
            *length = aac->lengthPcm;
            return FMOD_OK;
        case FMOD_TIMEUNIT_PCMBYTES:
            *length = aac->lengthPcm == AMPAAC_UNKNOWN ? AMPAAC_UNKNOWN : aac->lengthPcm * (unsigned int)aac->channels * 2u;
            return FMOD_OK;
        case FMOD_TIMEUNIT_MS:
            *length = aac->lengthPcm == AMPAAC_UNKNOWN
                    ? AMPAAC_UNKNOWN
                    : (unsigned int)((unsigned long long)aac->lengthPcm * 1000u / (unsigned int)aac->sampleRate);
            return FMOD_OK;
        case FMOD_TIMEUNIT_RAWBYTES:
            *length = aac->reader.size;
            return FMOD_OK;
        default:
            return FMOD_ERR_FORMAT;
    }
}

static FMOD_RESULT F_CALLBACK codec_setposition(FMOD_CODEC_STATE* state, int subsound, unsigned int position, FMOD_TIMEUNIT unit) {
    ampaac_codec* aac = (ampaac_codec*)state->plugindata;
    FMOD_RESULT   res;

    (void)subsound;
    TRACE("setposition unit=0x%x pos=%u decoded=%u", (unsigned int)unit, position, aac->decodedPcm);
    if (unit != FMOD_TIMEUNIT_PCM) {
        return FMOD_ERR_FORMAT;
    }
    if (aac->lengthPcm != AMPAAC_UNKNOWN && position > aac->lengthPcm) {
        position = aac->lengthPcm;
    }

    res = ampaac_adts_seek(aac, position);
    if (res != FMOD_OK) {
        return res;
    }
    /* No AACDEC_INTR: it drops SBR (and PS) to upsampling-only until the stream's next SBR header
       (libSBRdec sbrdecoder.cpp SBR_BS_INTERRUPTION), which is ~0.8 s of dull, narrow audio after every
       seek. The configuration is unchanged across a seek; the pre-roll rebuilds the frame-to-frame state. */
    aacDecoder_SetParam(aac->decoder, AAC_TPDEC_CLEAR_BUFFER, 1);
    aac->decodeFlags = 0;
    aac->discard     = position > aac->decodedPcm ? position - aac->decodedPcm : 0;
    aac->pcmFrames   = 0;
    aac->pcmNext     = 0;
    aac->exhausted   = 0;
    return FMOD_OK;
}
