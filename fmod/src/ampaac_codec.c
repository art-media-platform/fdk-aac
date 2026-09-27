/*
 * ampaac — FMOD codec plugin: AAC in ADTS or MP4/M4A, decoded by the Fraunhofer FDK AAC decoder.
 *
 * FMOD opens every sound through its codec list in priority order; open() claims only streams it can
 * decode and answers FMOD_ERR_FORMAT for everything else, leaving no side effects.
 *
 * Length. FMOD 2.03 ends a stream at the lengthpcm declared at open: it plays on to that length when the
 * data ends sooner and cuts the tail when it ends later, and it never re-reads the length or calls
 * getlength. So a stream declares its length only when the length is exact; otherwise it declares it
 * unknown, FMOD ends at the decoder's EOF, and the running estimate travels as AMPAAC_LENGTH_TAG for the
 * player's duration.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "ampaac.h"

#ifndef AMPAAC_SOURCE_REV
#define AMPAAC_SOURCE_REV "unknown"
#endif

#define PUBLISH_EVERY_AUS 256   /* re-publish a moving length estimate at most this often */

static const char ampaacIdent[] = "ampaac fdk-aac " AMPAAC_SOURCE_REV;

#ifdef AMPAAC_SPIKE
/* Spike builds trace the callbacks FMOD makes (fmod_harness). */
#define TRACE(...) do { fprintf(stderr, "ampaac: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#else
#define TRACE(...) do { } while (0)
#endif

#define WARN(state, ...) FMOD_CODEC_LOG((state), FMOD_DEBUG_LEVEL_WARNING, "ampaac", __VA_ARGS__)

static FMOD_RESULT F_CALL codec_open(FMOD_CODEC_STATE* state, FMOD_MODE usermode, FMOD_CREATESOUNDEXINFO* exinfo);
static FMOD_RESULT F_CALL codec_close(FMOD_CODEC_STATE* state);
static FMOD_RESULT F_CALL codec_read(FMOD_CODEC_STATE* state, void* buffer, unsigned int samplesIn, unsigned int* samplesOut);
static FMOD_RESULT F_CALL codec_setposition(FMOD_CODEC_STATE* state, int subsound, unsigned int position, FMOD_TIMEUNIT unit);

static FMOD_CODEC_DESCRIPTION ampaacDescription = {
    FMOD_CODEC_PLUGIN_VERSION,
    "AMP AAC (fdk-aac)",
    0x00010000,
    0,                   /* defaultasstream */
    FMOD_TIMEUNIT_PCM,
    codec_open,
    codec_close,
    codec_read,
    0,                   /* getlength: FMOD 2.03 never calls it; lengthpcm is read once at open */
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

static void channel_mask(ampaac_codec* aac) {
    switch (aac->channels) {
        case 1:  aac->waveformat.channelmask = FMOD_CHANNELMASK_MONO;    break;
        case 2:  aac->waveformat.channelmask = FMOD_CHANNELMASK_STEREO;  break;
        case 6:  aac->waveformat.channelmask = FMOD_CHANNELMASK_5POINT1; break;
        case 8:  aac->waveformat.channelmask = FMOD_CHANNELMASK_7POINT1; break;
        default: aac->waveformat.channelmask = 0;                        break;
    }
}

/* fdk's PCM mixer pins its output to 1, 2, 6 or 8 channels (libPCMutils pcmdmx_lib.cpp); other layouts
   extend to the next of these, the missing channels zero (3/0/2.1 or 3/0/4.1, WAV order). */
static int pinned_channels(int channels) {
    return channels <= 2 ? channels : channels <= 6 ? 6 : 8;
}

static FMOD_RESULT pin_channels(ampaac_codec* aac) {
    if (aacDecoder_SetParam(aac->decoder, AAC_PCM_MIN_OUTPUT_CHANNELS, aac->channels) != AAC_DEC_OK
        || aacDecoder_SetParam(aac->decoder, AAC_PCM_MAX_OUTPUT_CHANNELS, aac->channels) != AAC_DEC_OK) {
        return FMOD_ERR_FORMAT;
    }
    return FMOD_OK;
}

/* A frame with another channel count (only past 8 encoded channels, where fdk's mixer does not apply):
   the first channels are kept, mono is doubled, the rest are zero. In place: the carry holds 8 channels. */
static void adapt_channels(INT_PCM* pcm, unsigned int frames, int from, int to) {
    unsigned int i;
    int          c;

    if (from > to) {
        for (i = 0; i < frames; i++) {
            for (c = 0; c < to; c++) {
                pcm[(size_t)i * to + c] = pcm[(size_t)i * from + c];
            }
        }
        return;
    }
    for (i = frames; i-- > 0;) {
        for (c = to; c-- > 0;) {
            pcm[(size_t)i * to + c] = c < from ? pcm[(size_t)i * from + c] : from == 1 && c == 1 ? pcm[(size_t)i * from] : 0;
        }
    }
}

unsigned int ampaac_conceal_limit_ms = AMPAAC_CONCEAL_LIMIT_MS;

/* Frames of consecutive concealment that end an ADTS stream of unknown size (a live source that has lost
   its signal). A stream with a size or an access-unit table conceals through damage and decodes what
   follows: its data bounds it. */
static unsigned int conceal_limit(const ampaac_codec* aac) {
    if (aac->container != AMPAAC_ADTS || aac->reader.size != AMPAAC_UNKNOWN) {
        return AMPAAC_UNKNOWN;
    }
    return (unsigned int)((unsigned long long)ampaac_conceal_limit_ms * (unsigned int)aac->sampleRate / 1000u
                          / (unsigned int)aac->frameSize);
}

/* Moves the decode position; past FMOD's 32-bit PCM range it saturates and stops indexing. */
static void advance(ampaac_codec* aac, unsigned int frames) {
    if (aac->decodedPcm > AMPAAC_UNKNOWN - 1 - frames) {
        aac->decodedPcm = AMPAAC_UNKNOWN - 1;
        aac->exact      = 0;
        return;
    }
    aac->decodedPcm += frames;
}

/* Sends the length estimate as AMPAAC_LENGTH_TAG when it moved by half a second or became exact. */
static void publish_length(FMOD_CODEC_STATE* state, ampaac_codec* aac) {
    unsigned int ms;
    unsigned int moved;

    aac->framesSincePublish = 0;
    if (aac->container == AMPAAC_ADTS) {
        ampaac_adts_estimate_length(aac);
    }
    if (aac->lengthPcm == AMPAAC_UNKNOWN || aac->sampleRate <= 0) {
        return;
    }
    ms    = (unsigned int)((unsigned long long)aac->lengthPcm * 1000u / (unsigned int)aac->sampleRate);
    moved = ms > aac->publishedMs ? ms - aac->publishedMs : aac->publishedMs - ms;
    if (aac->publishedMs != 0 && moved < 500 && !aac->lengthExact && !aac->lengthFinal) {
        return;
    }
    if (ms == aac->publishedMs) {
        return;
    }
    aac->publishedMs = ms;
    FMOD_CODEC_METADATA(state, FMOD_TAGTYPE_USER, (char*)AMPAAC_LENGTH_TAG, &ms, sizeof(ms), FMOD_TAGDATATYPE_INT, 1);
    TRACE("length tag %u ms%s", ms, aac->lengthExact ? " (exact)" : "");
}

static FMOD_RESULT feed_next_au(ampaac_codec* aac) {
    unsigned int auLen = 0;
    FMOD_RESULT  res = aac->container == AMPAAC_MP4 ? ampaac_mp4_next(aac, &auLen) : ampaac_adts_next(aac, &auLen);

    if (res == FMOD_OK) {
        UCHAR* ptr   = aac->au;
        UINT   size  = auLen;
        UINT   valid = auLen;
        aacDecoder_Fill(aac->decoder, &ptr, &size, &valid);
    }
    return res;
}

/* At the end of the stream the decoded total is the length: exact when decoding ran continuously from an
   exact anchor, else the final estimate. A stream a limit ended keeps its estimate. */
static void finish_length(FMOD_CODEC_STATE* state, ampaac_codec* aac) {
    if (aac->lengthExact || aac->lengthFinal) {
        return;
    }
    if (aac->gaveUp) {
        publish_length(state, aac);
        return;
    }
    aac->lengthPcm   = aac->decodedPcm > aac->leadPcm ? aac->decodedPcm - aac->leadPcm : 0;
    aac->lengthExact = aac->exact;
    aac->lengthFinal = 1;
    publish_length(state, aac);
}

/* Moves the position past `frames` samples in the carry and hands them out, less the pending discard.
   Returns 1 when samples wait in the carry. */
static int hand_out(FMOD_CODEC_STATE* state, ampaac_codec* aac, unsigned int frames) {
    advance(aac, frames);
    if (++aac->framesSincePublish >= PUBLISH_EVERY_AUS) {
        publish_length(state, aac);
    }
    if (aac->discard >= frames) {
        aac->discard -= frames;
        return 0;
    }
    aac->pcmNext   = aac->discard;
    aac->pcmFrames = frames;
    aac->discard   = 0;
    return 1;
}

/*
 * Decodes one access unit into the PCM carry, feeding the decoder as it asks for bits.
 * Returns FMOD_OK with pcmFrames > 0, FMOD_ERR_FILE_EOF once the stream is drained, or a file error.
 * Every pass hands out a frame, consumes input, or moves the drain toward its end.
 */
static FMOD_RESULT decode_next(FMOD_CODEC_STATE* state, ampaac_codec* aac) {
    int conceal  = 0;
    int failures = 0;

    for (;;) {
        AAC_DECODER_ERROR err;
        CStreamInfo*      info;
        unsigned int      frames;
        int               concealed;
        int               silence = 0;
        unsigned int      flags = aac->decodeFlags | (conceal ? AACDEC_CONCEAL : 0);

        if (aac->exhausted) {
            /* After the last access unit, AACDEC_FLUSH drains the decoder's own delay (outputDelay),
               which the lead trimmed from the front. */
            if (aac->drainLeft == 0) {
                if (aac->endPcm != AMPAAC_UNKNOWN && aac->decodedPcm < aac->endPcm) {
                    /* The data ended before the declared length (a body cut short, a failed decoder):
                       silence to that length, rather than leave it to FMOD (what FMOD plays there is
                       unmeasured). */
                    unsigned int pad = aac->endPcm - aac->decodedPcm;
                    frames = pad < (unsigned int)aac->frameSize ? pad : (unsigned int)aac->frameSize;
                    memset(aac->pcm, 0, (size_t)frames * (size_t)aac->channels * sizeof(INT_PCM));
                    if (hand_out(state, aac, frames)) {
                        return FMOD_OK;
                    }
                    continue;
                }
                finish_length(state, aac);
                return FMOD_ERR_FILE_EOF;
            }
            flags        = AACDEC_FLUSH;
            aac->flushed = 1;
        }

        err = aacDecoder_DecodeFrame(aac->decoder, aac->pcm, AMPAAC_PCM_CAP, flags);

        if (err == AAC_DEC_NOT_ENOUGH_BITS && !conceal) {
            FMOD_RESULT res;

            if (aac->exhausted) {
                aac->drainLeft = 0;
                continue;
            }
            res = feed_next_au(aac);
            if (res == FMOD_ERR_FILE_EOF) {
                aac->exhausted = 1;
                aac->drainLeft = aac->decoderDelay;
                continue;
            }
            if (res != FMOD_OK) {
                return res;
            }
            continue;
        }

        if (err != AAC_DEC_NOT_ENOUGH_BITS) {
            aac->decodeFlags = 0;
        }
        info = IS_OUTPUT_VALID(err) ? aacDecoder_GetStreamInfo(aac->decoder) : NULL;
        if (!info || info->frameSize <= 0 || info->numChannels <= 0 || info->sampleRate <= 0
            || (unsigned int)info->frameSize * (unsigned int)(info->numChannels > aac->channels ? info->numChannels : aac->channels)
                   > AMPAAC_PCM_CAP) {
            if (aac->exhausted) {
                aac->drainLeft = 0;
                continue;
            }
            if ((!IS_OUTPUT_VALID(err) && IS_INIT_ERROR(err)) || err == AAC_DEC_OUT_OF_MEMORY
                || err == AAC_DEC_INVALID_HANDLE || ++failures > 16) {
                WARN(state, "decoder error 0x%x; ending the stream", (unsigned int)err);
                aac->exhausted = 1;
                aac->drainLeft = 0;
                aac->gaveUp    = 1;
                continue;
            }
            /* Transport or bitstream loss: one concealed frame keeps time, then fresh input. */
            conceal = !IS_OUTPUT_VALID(err) && !conceal;
            continue;
        }
        concealed = err != AAC_DEC_OK || (flags & AACDEC_CONCEAL) != 0;
        conceal   = 0;
        failures  = 0;

        if (info->sampleRate != aac->sampleRate || info->frameSize != aac->frameSize) {
            if (concealed) {
                /* Concealment at another rate (implicit SBR falls back to the core rate for a lost frame):
                   a frame of silence at the stream's rate keeps FMOD's timeline and the seek arithmetic. */
                silence = 1;
            } else if (!aac->started && aac->startupDrops < AMPAAC_STARTUP_DROP
                       && !(info->sampleRate == aac->openRate && info->frameSize == aac->openFrameSize)) {
                /* Start-up transient of a decoder started clean (SBR not running yet): the rate stands. A
                   frame in the stream's opening format is none: a restart at the first frame returns to it. */
                aac->startupDrops++;
                if (aac->exhausted) {
                    aac->drainLeft = 0;
                }
                continue;
            } else {
                if (info->sampleRate != aac->sampleRate) {
                    float rate = (float)info->sampleRate;
                    FMOD_CODEC_METADATA(state, FMOD_TAGTYPE_FMOD, (char*)"Sample Rate Change", &rate, sizeof(rate),
                                        FMOD_TAGDATATYPE_FLOAT, 1);
                }
                aac->sampleRate = info->sampleRate;
                aac->frameSize  = info->frameSize;
            }
        }
        if (!concealed) {
            aac->concealRun = 0;
        } else if (++aac->concealRun > conceal_limit(aac)) {
            WARN(state, "%u frames concealed in a row; ending the stream", aac->concealRun);
            aac->exhausted = 1;
            aac->drainLeft = 0;
            aac->gaveUp    = 1;
            continue;
        }
        aac->started = 1;

        frames = (unsigned int)aac->frameSize;
        if (silence) {
            memset(aac->pcm, 0, (size_t)frames * (size_t)aac->channels * sizeof(INT_PCM));
        } else if (info->numChannels != aac->channels) {
            adapt_channels(aac->pcm, frames, info->numChannels, aac->channels);
        }
        if (aac->exhausted) {
            /* A drained frame: only its first drainLeft samples are audio. */
            if (frames > aac->drainLeft) {
                frames = aac->drainLeft;
            }
            aac->drainLeft -= frames;
        }
        if (aac->endPcm != AMPAAC_UNKNOWN) {
            /* The encoder's padding past the presented length is dropped. */
            if (aac->decodedPcm >= aac->endPcm) {
                aac->exhausted = 1;
                aac->drainLeft = 0;
                continue;
            }
            if ((unsigned long long)aac->decodedPcm + frames > aac->endPcm) {
                frames = aac->endPcm - aac->decodedPcm;
            }
        }
        if (hand_out(state, aac, frames)) {
            return FMOD_OK;
        }
    }
}

static HANDLE_AACDECODER open_decoder(TRANSPORT_TYPE transport) {
    HANDLE_AACDECODER decoder = aacDecoder_Open(transport, 1);

    if (decoder) {
        /* fdk normalizes to -24 dBFS by default (targetRefLevel 96), which attenuates streams carrying a
           program reference level; -1 decodes at the encoded level, like every other FMOD codec. */
        aacDecoder_SetParam(decoder, AAC_DRC_REFERENCE_LEVEL, -1);
    }
    return decoder;
}

/* Decoding restarts: from the first frame the probe frames' filter state must not bleed in (AACDEC_CLRHIST,
   which also resets SBR to wait for a header, and the first frame carries one). Mid-stream nothing is
   flagged: AACDEC_INTR and AACDEC_CLRHIST both drop SBR/PS to upsampling-only until the stream's next SBR
   header (libSBRdec sbrdecoder.cpp SBR_BS_INTERRUPTION, SBRDEC_FORCE_RESET), ~0.8 s of dull, narrow
   audio; the configuration is unchanged across a seek and the pre-roll rebuilds frame-to-frame state. */
static void restart_decode(ampaac_codec* aac) {
    aacDecoder_SetParam(aac->decoder, AAC_TPDEC_CLEAR_BUFFER, 1);
    aac->decodeFlags  = aac->exact && aac->decodedPcm == 0 ? AACDEC_CLRHIST : 0;
    aac->pcmFrames    = 0;
    aac->pcmNext      = 0;
    aac->exhausted    = 0;
    aac->concealRun   = 0;
    aac->startupDrops = 0;
    if (aac->decodeFlags & AACDEC_CLRHIST) {
        aac->started = 0;   /* SBR waits for a header again: its start-up transient may recur */
    }
}

/* A decoder for the container: raw access units configured from the AudioSpecificConfig, or ADTS. */
static FMOD_RESULT open_container_decoder(ampaac_codec* aac) {
    if (aac->container == AMPAAC_MP4) {
        UCHAR* asc     = (UCHAR*)aac->mp4.asc;
        UINT   ascSize = aac->mp4.ascSize;
        aac->decoder = open_decoder(TT_MP4_RAW);
        if (!aac->decoder) {
            return FMOD_ERR_MEMORY;
        }
        return aacDecoder_ConfigRaw(aac->decoder, &asc, &ascSize) == AAC_DEC_OK ? FMOD_OK : FMOD_ERR_FORMAT;
    }
    aac->decoder = open_decoder(TT_MP4_ADTS);
    return aac->decoder ? FMOD_OK : FMOD_ERR_MEMORY;
}

static FMOD_RESULT rewind_container(ampaac_codec* aac) {
    if (aac->container == AMPAAC_MP4) {
        aac->mp4.next = 0;
        return FMOD_OK;
    }
    return ampaac_reader_seek(&aac->reader, aac->dataStart);
}

typedef struct ampaac_format {
    int sampleRate;
    int channels;
    int frameSize;
} ampaac_format;

/* Decodes the first access units to learn the output format (implicit SBR/PS changes rate and channels).
   Also primes a fresh decoder's SBR/PS state; the caller decides what the findings change. */
static FMOD_RESULT learn_format(ampaac_codec* aac, ampaac_format* format) {
    int decoded = 0;
    int attempts;

    memset(format, 0, sizeof(*format));
    for (attempts = 0; attempts < 64 && decoded < AMPAAC_OPEN_AUS; attempts++) {
        AAC_DECODER_ERROR err = aacDecoder_DecodeFrame(aac->decoder, aac->pcm, AMPAAC_PCM_CAP, 0);
        CStreamInfo*      info;

        if (err == AAC_DEC_NOT_ENOUGH_BITS) {
            FMOD_RESULT res = feed_next_au(aac);
            if (res == FMOD_ERR_FILE_EOF) {
                break;
            }
            if (res != FMOD_OK) {
                return res;
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
        format->sampleRate = info->sampleRate;
        format->channels   = info->numChannels;
        format->frameSize  = info->frameSize;
        decoded++;
    }
    if (decoded == 0 || format->channels > AMPAAC_MAX_CHANNELS) {
        return FMOD_ERR_FORMAT;
    }
    return FMOD_OK;
}

static FMOD_RESULT fail_open(FMOD_CODEC_STATE* state, FMOD_RESULT res) {
    codec_close(state);
    return res;
}

static FMOD_RESULT F_CALL codec_open(FMOD_CODEC_STATE* state, FMOD_MODE usermode, FMOD_CREATESOUNDEXINFO* exinfo) {
    ampaac_codec*        aac;
    const unsigned char* head;
    unsigned int         avail;
    unsigned int         pos = 0;
    ampaac_format        format;
    FMOD_RESULT          res;

    (void)exinfo;
    aac = (ampaac_codec*)FMOD_CODEC_ALLOC(state, sizeof(ampaac_codec), 16);
    if (!aac) {
        return FMOD_ERR_MEMORY;
    }
    memset(aac, 0, sizeof(*aac));
    state->plugindata = aac;

    /* FMOD hands each codec the file at 0; a netstream turns any seek into a reconnect, so only rewind
       when the position says otherwise. */
    if ((FMOD_CODEC_FILE_TELL(state, &pos) != FMOD_OK || pos != 0)
        && FMOD_CODEC_FILE_SEEK(state, 0, FMOD_CODEC_SEEK_METHOD_SET) != FMOD_OK) {
        return fail_open(state, FMOD_ERR_FORMAT);
    }
    ampaac_reader_init(&aac->reader, state);
    skip_id3v2(&aac->reader);

    /* Until the data shows ftyp or an ADTS chain it is not known to be AAC: a file error here answers
       FMOD_ERR_FORMAT, so FMOD's own codecs still get their turn (an MP3's ID3v2 skip is a hard seek). */
    avail = ampaac_reader_peek(&aac->reader, 12, &head);
    if (aac->reader.fault != FMOD_OK || foreign_magic(head, avail)) {
        return fail_open(state, FMOD_ERR_FORMAT);
    }
    aac->endPcm = AMPAAC_UNKNOWN;
    res = avail >= 8 && memcmp(head + 4, "ftyp", 4) == 0 ? ampaac_mp4_open(aac) : ampaac_adts_open(aac);
    if (res == FMOD_OK) {
        res = open_container_decoder(aac);
    }
    if (res != FMOD_OK) {
        return fail_open(state, res);
    }
    if (aac->container == AMPAAC_MP4) {
        CStreamInfo* info = aacDecoder_GetStreamInfo(aac->decoder);
        aac->mp4.usac = info && info->aot == AOT_USAC;
    }

    /* Probe decode on the decoder the stream keeps (it retains the SBR/PS header), pin the channel count,
       then restart from the first access unit. */
    aac->exact = 1;
    res = learn_format(aac, &format);
    if (res != FMOD_OK) {
        return fail_open(state, res);
    }
    aac->sampleRate    = format.sampleRate;
    aac->frameSize     = format.frameSize;
    aac->openRate      = format.sampleRate;
    aac->openFrameSize = format.frameSize;
    aac->channels      = pinned_channels(format.channels);
    res = pin_channels(aac);
    if (res != FMOD_OK) {
        return fail_open(state, res);
    }
    aac->pcmPerFrame = (unsigned int)aac->frameSize;
    /* fdk's output trails the reference decoder model by outputDelay (filterbank delay plus the limiter's
       lookahead): 1685 samples for AAC-LC at 44.1 kHz, 3730 for HE-AAC at 48 kHz. The lead trims it (and
       the container's priming) so output lines up with the encoder's input; the drain restores the tail. */
    aac->decoderDelay = aacDecoder_GetStreamInfo(aac->decoder)->outputDelay;
    aac->leadPcm      = aac->decoderDelay;
    if (aac->container == AMPAAC_MP4) {
        ampaac_mp4_set_trim(aac);
    } else {
        ampaac_adts_reset_index(aac);
        if (usermode & FMOD_ACCURATETIME) {
            ampaac_adts_walk_to_end(aac);   /* reads the whole stream: exact length and a full seek index */
        }
    }
    res = rewind_container(aac);
    if (res != FMOD_OK) {
        return fail_open(state, res);
    }
    aac->decodedPcm = 0;
    aac->exact      = 1;
    restart_decode(aac);
    aac->discard = aac->leadPcm;

    aac->waveformat.name         = ampaacIdent;
    aac->waveformat.format       = FMOD_SOUND_FORMAT_PCM16;
    aac->waveformat.channels     = aac->channels;
    aac->waveformat.frequency    = aac->sampleRate;
    aac->waveformat.lengthbytes  = aac->reader.size;
    aac->waveformat.lengthpcm    = aac->lengthExact ? aac->lengthPcm : AMPAAC_UNKNOWN;
    aac->waveformat.pcmblocksize = (unsigned int)aac->frameSize;
    aac->waveformat.channelorder = FMOD_CHANNELORDER_WAVEFORMAT;
    channel_mask(aac);

    state->waveformat   = &aac->waveformat;
    state->numsubsounds = 0;
    publish_length(state, aac);

    TRACE("open %s size=%u rate=%d ch=%d frame=%d lengthpcm=%u estimate=%u lead=%u mode=0x%x",
          aac->container == AMPAAC_MP4 ? "mp4" : "adts", aac->reader.size, aac->sampleRate, aac->channels,
          aac->frameSize, aac->waveformat.lengthpcm, aac->lengthPcm, aac->leadPcm, (unsigned int)usermode);
    return FMOD_OK;
}

static FMOD_RESULT F_CALL codec_close(FMOD_CODEC_STATE* state) {
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
    ampaac_mp4_close(aac);
    FMOD_CODEC_FREE(state, aac);
    state->plugindata = 0;
    return FMOD_OK;
}

static FMOD_RESULT F_CALL codec_read(FMOD_CODEC_STATE* state, void* buffer, unsigned int samplesIn, unsigned int* samplesOut) {
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
        TRACE("read eof produced=%u decoded=%u", produced, aac->decodedPcm);
        return produced > 0 ? FMOD_OK : FMOD_ERR_FILE_EOF;
    }
    if (res != FMOD_OK && produced == 0) {
        return res;
    }
    return FMOD_OK;
}

static FMOD_RESULT F_CALL codec_setposition(FMOD_CODEC_STATE* state, int subsound, unsigned int position, FMOD_TIMEUNIT unit) {
    ampaac_codec*      aac = (ampaac_codec*)state->plugindata;
    unsigned long long target;
    FMOD_RESULT        res;

    (void)subsound;
    TRACE("setposition unit=0x%x pos=%u decoded=%u", (unsigned int)unit, position, aac->decodedPcm);
    if (unit != FMOD_TIMEUNIT_PCM) {
        return FMOD_ERR_FORMAT;
    }
    aac->gaveUp = 0;   /* a limit that ended the stream applied to the data it met, not to the target's */
    if (aac->lengthExact && position > aac->lengthPcm) {
        position = aac->lengthPcm;
    }
    target   = (unsigned long long)position + aac->leadPcm;   /* FMOD's position excludes the priming */
    position = target < AMPAAC_UNKNOWN ? (unsigned int)target : AMPAAC_UNKNOWN - 1;

    if (aac->flushed) {
        /* The drain left SBR/PS in upsampling (libSBRdec sbrdecoder.cpp: more flushed frames than its
           delay). A fresh decoder primed on the opening access units is in the state open() left; the
           announced format stands, and the decode loop tags any rate the stream has at the target. */
        ampaac_format primed;
        int           exact = aac->exact;
        aacDecoder_Close(aac->decoder);
        aac->decoder = 0;
        res = open_container_decoder(aac);
        if (res == FMOD_OK) {
            aac->exact = 0;   /* priming must not index */
            res = rewind_container(aac);
        }
        if (res == FMOD_OK) {
            res = learn_format(aac, &primed);
        }
        if (res == FMOD_OK) {
            res = pin_channels(aac);
        }
        aac->exact = exact;
        if (res != FMOD_OK) {
            return res;
        }
        aac->flushed = 0;
    }

    res = aac->container == AMPAAC_MP4 ? ampaac_mp4_seek(aac, position) : ampaac_adts_seek(aac, position);
    if (res != FMOD_OK) {
        /* The reader may have moved: positions are no longer exact and the carry is stale. */
        aac->exact     = 0;
        aac->pcmFrames = 0;
        aac->pcmNext   = 0;
        return res;
    }
    restart_decode(aac);
    aac->discard = position > aac->decodedPcm ? position - aac->decodedPcm : 0;
    publish_length(state, aac);
    return FMOD_OK;
}
