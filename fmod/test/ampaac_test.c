/*
 * ampaac host tests: the codec driven through its FMOD description against fake_fmod's memory files.
 * Usage: ampaac_test <fixtures dir>
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_fmod.h"
#include "ampaac.h"

F_EXPORT FMOD_CODEC_DESCRIPTION* F_CALL AMPAAC_GetCodecDescription(void);

static int failures;
static int checks;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                  \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

typedef struct blob {
    unsigned char* data;
    unsigned int   size;
} blob;

typedef struct decoded {
    int          rate;
    int          channels;
    unsigned int frames;
    unsigned int declaredLength;   /* waveformat.lengthpcm at open */
    unsigned int tagAtOpenMs;      /* AMPAAC_LENGTH_TAG after open, 0 if none */
    unsigned int tagAtEndMs;       /* ... after EOF */
    int          rateTags;         /* "Sample Rate Change" tags raised */
    short*       pcm;
} decoded;

typedef struct fixture {
    const char* name;
    int         rate;
    int         channels;
    int         mp4;       /* M4A: exact length, priming trimmed to the source's first sample */
    int         moovEnd;   /* moov follows mdat: cutting the file's end cuts moov */
} fixture;

static const fixture fixtures[] = {
    { "adts_lc_44k_stereo.aac",   44100, 2, 0, 0 },
    { "adts_lc_22k_mono.aac",     22050, 1, 0, 0 },
    { "adts_he_48k_stereo.aac",   48000, 2, 0, 0 },
    { "adts_hev2_48k_stereo.aac", 48000, 2, 0, 0 },
    { "m4a_lc_44k_stereo.m4a",    44100, 2, 1, 0 },
    { "m4a_he_48k_stereo.m4a",    48000, 2, 1, 0 },
    { "m4a_hev2_48k_stereo.m4a",  48000, 2, 1, 0 },
    { "m4a_lc_44k_moovend.m4a",   44100, 2, 1, 1 },   /* moov after mdat */
    { "m4a_lc_44k_elst.m4a",      44100, 2, 1, 0 },   /* gapless from elst, no iTunSMPB */
};

static const char* fixtureDir;

static blob load(const char* name) {
    blob  out = { NULL, 0 };
    char  path[1024];
    FILE* file;
    long  size;

    snprintf(path, sizeof(path), "%s/%s", fixtureDir, name);
    file = fopen(path, "rb");
    if (!file) {
        printf("  cannot open %s\n", path);
        exit(2);
    }
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    out.data = (unsigned char*)malloc((size_t)size + 1);
    out.size = (unsigned int)fread(out.data, 1, (size_t)size, file);
    fclose(file);
    return out;
}

static FMOD_CODEC_DESCRIPTION* codec(void) {
    return AMPAAC_GetCodecDescription();
}

static FMOD_RESULT open_file(fake_file* file, FMOD_MODE mode) {
    return codec()->open(&file->state, FMOD_CREATESTREAM | mode, NULL);
}

static void close_file(fake_file* file) {
    codec()->close(&file->state);
}

/* Reads until EOF in `chunk`-frame requests; out->pcm holds every frame. */
static FMOD_RESULT read_all(fake_file* file, unsigned int chunk, decoded* out) {
    unsigned int cap = 1 << 16;
    int          channels = file->state.waveformat->channels;

    out->rate     = file->state.waveformat->frequency;
    out->channels = channels;
    out->frames   = 0;
    out->pcm      = (short*)malloc((size_t)cap * channels * sizeof(short));
    for (;;) {
        unsigned int got = 0;
        FMOD_RESULT  res;

        if (out->frames + chunk > cap) {
            cap *= 2;
            out->pcm = (short*)realloc(out->pcm, (size_t)cap * channels * sizeof(short));
        }
        res = codec()->read(&file->state, out->pcm + (size_t)out->frames * channels, chunk, &got);
        out->frames += got;
        if (res == FMOD_ERR_FILE_EOF) {
            return FMOD_OK;
        }
        if (res != FMOD_OK) {
            return res;
        }
        if (got == 0) {
            return FMOD_ERR_INTERNAL;   /* FMOD_OK with nothing would spin FMOD's stream thread */
        }
    }
}

static FMOD_RESULT decode_blob(const unsigned char* data, unsigned int size, unsigned int chunk, decoded* out) {
    fake_file   file;
    FMOD_RESULT res;

    memset(out, 0, sizeof(*out));
    fake_file_init(&file, data, size);
    res = open_file(&file, 0);
    if (res != FMOD_OK) {
        return res;
    }
    out->declaredLength = file.state.waveformat->lengthpcm;
    out->tagAtOpenMs    = file.lengthTags ? file.lengthTagMs : 0;
    res = read_all(&file, chunk, out);
    out->tagAtEndMs     = file.lengthTags ? file.lengthTagMs : 0;
    out->rateTags       = file.sampleRateTags;
    close_file(&file);
    return res;
}

/* RMS of one channel over frames [from, from + count). */
static double channel_rms(const decoded* d, int channel, unsigned int from, unsigned int count) {
    double       sum = 0;
    unsigned int i;

    for (i = from; i < from + count && i < d->frames; i++) {
        double v = d->pcm[(size_t)i * d->channels + channel];
        sum += v * v;
    }
    return count ? sqrt(sum / count) : 0;
}

static unsigned int frames_to_ms(unsigned int frames, int rate) {
    return (unsigned int)((unsigned long long)frames * 1000u / (unsigned int)rate);
}

/* Frequency of one channel from positive-going zero crossings over [from, from + count). */
static double tone_hz(const decoded* d, int channel, unsigned int from, unsigned int count) {
    unsigned int crossings = 0;
    unsigned int n;

    if (from + count > d->frames) {
        return 0;
    }
    for (n = from + 1; n < from + count; n++) {
        short prev = d->pcm[(size_t)(n - 1) * d->channels + channel];
        short cur  = d->pcm[(size_t)n * d->channels + channel];
        if (prev < 0 && cur >= 0) {
            crossings++;
        }
    }
    return (double)crossings * d->rate / count;
}

static double rms(const short* pcm, unsigned int samples) {
    double       sum = 0;
    unsigned int n;
    for (n = 0; n < samples; n++) {
        sum += (double)pcm[n] * pcm[n];
    }
    return samples ? sqrt(sum / samples) : 0;
}

static void check_tones(const decoded* d, const char* what) {
    unsigned int from  = (unsigned int)d->rate;          /* 1.0 s .. 2.0 s */
    unsigned int count = (unsigned int)d->rate;
    double       left  = tone_hz(d, 0, from, count);

    CHECK(fabs(left - 440) < 440 * 0.015, "%s: left tone %.1f Hz, want 440", what, left);
    if (d->channels == 2) {
        double right = tone_hz(d, 1, from, count);
        CHECK(fabs(right - 660) < 660 * 0.015, "%s: right tone %.1f Hz, want 660", what, right);
    }
}

/* Largest sample difference between a post-seek decode (from `target`) and the continuous decode, over
   [from, from + frames) past the target. */
static double seek_divergence(const decoded* whole, const decoded* tail, unsigned int target, unsigned int from, unsigned int frames) {
    double       worst = 0;
    unsigned int n;

    for (n = from * (unsigned int)whole->channels;
         n < (from + frames) * (unsigned int)whole->channels && n < tail->frames * (unsigned int)whole->channels; n++) {
        double diff = fabs((double)tail->pcm[n] - whole->pcm[(size_t)target * whole->channels + n]);
        if (diff > worst) {
            worst = diff;
        }
    }
    return worst;
}

typedef enum seek_setup {
    SEEK_FRESH   = 0,   /* before anything decoded: the header walk */
    SEEK_PLAYING = 1,   /* after decoding past the target, not to EOF: a warm decoder */
    SEEK_AFTER_EOF = 2  /* after the drain: the decoder is re-primed */
} seek_setup;

/* Opens, decodes per `setup`, seeks to target and decodes to EOF. */
static FMOD_RESULT seek_and_decode(const blob* src, seek_setup setup, unsigned int target, decoded* tail, fake_file* file) {
    FMOD_RESULT res;

    fake_file_init(file, src->data, src->size);
    res = open_file(file, 0);
    if (res != FMOD_OK) {
        memset(tail, 0, sizeof(*tail));
        return res;
    }
    codec()->setposition(&file->state, 0, 0, FMOD_TIMEUNIT_PCM);   /* FMOD rewinds right after open */
    if (setup == SEEK_AFTER_EOF) {
        read_all(file, 4096, tail);
        free(tail->pcm);
    } else if (setup == SEEK_PLAYING) {
        static short scratch[4096 * AMPAAC_MAX_CHANNELS];
        unsigned int played = 0;
        while (played < target + (unsigned int)file->state.waveformat->frequency / 2) {
            unsigned int got = 0;
            if (codec()->read(&file->state, scratch, 4096, &got) != FMOD_OK || got == 0) {
                break;
            }
            played += got;
        }
    }
    res = codec()->setposition(&file->state, 0, target, FMOD_TIMEUNIT_PCM);
    if (res == FMOD_OK) {
        res = read_all(file, 4096, tail);
    }
    close_file(file);
    return res;
}

static void test_fixture(const fixture* fx) {
    blob         src = load(fx->name);
    decoded      whole;
    fake_file    file;
    FMOD_RESULT  res;
    unsigned int want = (unsigned int)(3.0 * fx->rate);

    printf("%s\n", fx->name);

    res = decode_blob(src.data, src.size, 4096, &whole);
    CHECK(res == FMOD_OK, "decode: result %d", res);
    CHECK(whole.rate == fx->rate, "rate %d, want %d", whole.rate, fx->rate);
    CHECK(whole.channels == fx->channels, "channels %d, want %d", whole.channels, fx->channels);
    check_tones(&whole, "whole");

    if (fx->mp4) {
        /* M4A: exact length declared to FMOD; priming and padding trimmed (alignment: test_alignment). */
        CHECK(whole.frames == want, "decoded %u frames, want exactly 3.0 s (%u)", whole.frames, want);
        CHECK(whole.declaredLength == want, "declared length %u, want %u", whole.declaredLength, want);
    } else {
        /* FMOD ends a stream at the declared length, so ADTS declares none; the estimate travels as a tag
           and becomes exact at EOF. */
        CHECK(whole.frames >= want && whole.frames <= want + want / 10,
              "decoded %u frames, want 3.0 s (%u) plus encoder priming and padding", whole.frames, want);
        CHECK(whole.declaredLength == AMPAAC_UNKNOWN, "declared length %u, want unknown", whole.declaredLength);
        CHECK(whole.tagAtOpenMs > 0 && fabs((double)whole.tagAtOpenMs - frames_to_ms(whole.frames, whole.rate))
              < frames_to_ms(whole.frames, whole.rate) * 0.15,
              "length tag at open %u ms vs decoded %u ms", whole.tagAtOpenMs, frames_to_ms(whole.frames, whole.rate));
    }
    CHECK(whole.tagAtEndMs == frames_to_ms(whole.frames, whole.rate),
          "length tag at EOF %u ms, want exact %u ms", whole.tagAtEndMs, frames_to_ms(whole.frames, whole.rate));

    /* FMOD_ACCURATETIME walks the stream at open: exact declared length. */
    fake_file_init(&file, src.data, src.size);
    res = open_file(&file, FMOD_ACCURATETIME);
    CHECK(res == FMOD_OK && file.state.waveformat->lengthpcm == whole.frames,
          "ACCURATETIME declared length %u, want %u", res == FMOD_OK ? file.state.waveformat->lengthpcm : 0, whole.frames);
    if (res == FMOD_OK) {
        close_file(&file);
    }

    /* Trailing tags (an APE footer, then ID3v1) follow the last frame: the length stays exact at EOF and in
       the ACCURATETIME walk. */
    if (!fx->mp4) {
        unsigned char* tagged = (unsigned char*)malloc(src.size + 160);
        decoded        trailed;

        memcpy(tagged, src.data, src.size);
        memset(tagged + src.size, 0, 160);
        memcpy(tagged + src.size, "APETAGEX", 8);
        memcpy(tagged + src.size + 32, "TAG", 3);
        res = decode_blob(tagged, src.size + 160, 4096, &trailed);
        CHECK(res == FMOD_OK && trailed.frames == whole.frames && trailed.tagAtEndMs == frames_to_ms(whole.frames, whole.rate),
              "trailing tags: %d, %u frames, length tag %u ms (want %u frames, %u ms)", res, trailed.frames,
              trailed.tagAtEndMs, whole.frames, frames_to_ms(whole.frames, whole.rate));
        free(trailed.pcm);
        fake_file_init(&file, tagged, src.size + 160);
        res = open_file(&file, FMOD_ACCURATETIME);
        CHECK(res == FMOD_OK && file.state.waveformat->lengthpcm == whole.frames,
              "ACCURATETIME with trailing tags: length %u, want %u", res == FMOD_OK ? file.state.waveformat->lengthpcm : 0,
              whole.frames);
        if (res == FMOD_OK) {
            close_file(&file);
        }
        free(tagged);
    }

    /* Trickling reads decode bit-identically. */
    {
        decoded trickle;
        fake_file_init(&file, src.data, src.size);
        file.maxChunk = 97;
        res = open_file(&file, 0);
        CHECK(res == FMOD_OK, "trickle open %d", res);
        if (res == FMOD_OK) {
            read_all(&file, 1000, &trickle);
            close_file(&file);
            CHECK(trickle.frames == whole.frames
                  && memcmp(trickle.pcm, whole.pcm, (size_t)whole.frames * whole.channels * sizeof(short)) == 0,
                  "trickled decode differs (%u vs %u frames)", trickle.frames, whole.frames);
            free(trickle.pcm);
        }
    }

    /* Rewind right after open (FMOD does this): nothing is skipped. */
    {
        decoded again;
        fake_file_init(&file, src.data, src.size);
        open_file(&file, 0);
        res = codec()->setposition(&file.state, 0, 0, FMOD_TIMEUNIT_PCM);
        CHECK(res == FMOD_OK, "setposition(0) %d", res);
        read_all(&file, 4096, &again);
        CHECK(again.frames == whole.frames
              && memcmp(again.pcm, whole.pcm, (size_t)whole.frames * whole.channels * sizeof(short)) == 0,
              "decode after setposition(0) differs (%u vs %u frames)", again.frames, whole.frames);
        free(again.pcm);
        close_file(&file);
    }

    /* Exact seeks land on the continuous decode's sample positions. A warm decoder (seek while playing):
       LC is bit-exact, SBR/PS leave a few LSB of fixed-point residue after the pre-roll. A decoder that
       knows only the opening frames' SBR/PS headers (seek before decoding; seek after EOF, where the drain
       idled SBR and the decoder is re-primed) renders parametric stereo slightly differently until the
       stream's next header (~0.8 s in these fixtures), then converges. A dormant SBR (the AACDEC_INTR
       failure) shows as thousands of LSB. */
    {
        static const char* const names[] = { "fresh (header walk)", "while playing", "after EOF" };
        unsigned int target = (unsigned int)(1.3 * fx->rate);
        int          setup;

        for (setup = SEEK_FRESH; setup <= SEEK_AFTER_EOF; setup++) {
            decoded tail;
            double  early;
            double  later;

            res = seek_and_decode(&src, (seek_setup)setup, target, &tail, &file);
            CHECK(res == FMOD_OK && tail.frames == whole.frames - target, "seek (%s) %d, %u frames, want %u",
                  names[setup], res, tail.frames, whole.frames - target);
            if (res != FMOD_OK) {
                continue;
            }
            early = seek_divergence(&whole, &tail, target, 0, 4096);
            later = seek_divergence(&whole, &tail, target, (unsigned int)fx->rate, 4096);
            printf("  exact seek (%s): max |diff| %.0f, %.0f after 1 s\n", names[setup], early, later);
            if (setup == SEEK_PLAYING) {
                CHECK(early <= 4, "exact seek (%s) diverges from the continuous decode (max |diff| %.0f)", names[setup], early);
            } else {
                CHECK(early <= 2048, "exact seek (%s) starts with dormant SBR/PS (max |diff| %.0f)", names[setup], early);
            }
            CHECK(later <= 4, "exact seek (%s) has not converged 1 s on (max |diff| %.0f)", names[setup], later);
            free(tail.pcm);
        }
    }

    /* No walk budget: the seek lands by the mean frame size; audio continues near the target. */
    {
        decoded      tail;
        unsigned int target = (unsigned int)(2.5 * fx->rate);
        unsigned int saved = ampaac_hop_budget_ms;

        ampaac_hop_budget_ms = 0;
        res = seek_and_decode(&src, SEEK_FRESH, target, &tail, &file);
        ampaac_hop_budget_ms = saved;
        CHECK(res == FMOD_OK, "estimated seek %d", res);
        CHECK(tail.frames + 3 * 2048 > whole.frames - target && tail.frames < whole.frames - target + 3 * 2048,
              "after estimated seek %u frames, want about %u", tail.frames, whole.frames - target);
        CHECK(tail.frames > 2048 && rms(tail.pcm, 2048 * tail.channels) > 2000, "silence after estimated seek");
        free(tail.pcm);
    }

    /* Source without a size: ADTS has no length at all (FmodPlayer treats it as live); M4A still knows
       its length from the sample tables. Decode unchanged. */
    {
        decoded live;
        fake_file_init(&file, src.data, src.size);
        file.sizeUnknown = 1;
        res = open_file(&file, 0);
        CHECK(res == FMOD_OK, "unknown-size open %d", res);
        if (res == FMOD_OK) {
            if (fx->mp4) {
                CHECK(file.state.waveformat->lengthpcm == want, "unknown-size M4A: declared %u, want %u",
                      file.state.waveformat->lengthpcm, want);
            } else {
                CHECK(file.state.waveformat->lengthpcm == AMPAAC_UNKNOWN && file.lengthTags == 0,
                      "unknown-size: declared %u, %d length tags", file.state.waveformat->lengthpcm, file.lengthTags);
            }
            read_all(&file, 4096, &live);
            CHECK(live.frames == whole.frames, "unknown-size decoded %u, want %u", live.frames, whole.frames);
            free(live.pcm);
            close_file(&file);
        }
    }

    /* ICY cut: the capture starts mid-frame (ADTS only). */
    if (!fx->mp4) {
        unsigned int   junk = 700;
        unsigned char* cut = (unsigned char*)malloc(src.size + junk);
        decoded        icy;
        unsigned int   n;
        unsigned int   seed = 12345;

        for (n = 0; n < junk; n++) {
            seed = seed * 1103515245u + 12345u;
            cut[n] = (unsigned char)(seed >> 16);
        }
        memcpy(cut + junk, src.data, src.size);
        res = decode_blob(cut, src.size + junk, 4096, &icy);
        CHECK(res == FMOD_OK && icy.frames == whole.frames, "junk-prefixed decode %d, %u frames (want %u)", res, icy.frames, whole.frames);
        free(icy.pcm);

        /* ID3v2 ahead of the frames. */
        memset(cut, 0, junk);
        memcpy(cut, "ID3\x04\x00\x00\x00\x00\x05\x32", 10);   /* synchsafe 0x2B2 = 690 bytes of tag body */
        res = decode_blob(cut, src.size + junk, 4096, &icy);
        CHECK(res == FMOD_OK && icy.frames == whole.frames, "ID3-prefixed decode %d, %u frames (want %u)", res, icy.frames, whole.frames);
        free(icy.pcm);
        free(cut);
    }

    /* Truncated mid-frame and corrupted in the middle: decode completes. */
    {
        unsigned char* bad = (unsigned char*)malloc(src.size);
        decoded        damaged;

        res = decode_blob(src.data, src.size - 333, 4096, &damaged);
        if (fx->moovEnd) {
            CHECK(res == FMOD_ERR_FORMAT, "truncated moov: open %d, want FMOD_ERR_FORMAT", res);
        } else {
            CHECK(res == FMOD_OK && damaged.frames <= whole.frames && damaged.frames > whole.frames / 2,
                  "truncated decode %d, %u frames", res, damaged.frames);
            free(damaged.pcm);
        }

        memcpy(bad, src.data, src.size);
        memset(bad + src.size / 2, 0, 200);
        res = decode_blob(bad, src.size, 4096, &damaged);
        CHECK(res == FMOD_OK && damaged.frames + 8 * 2048 > whole.frames && damaged.frames <= whole.frames + 8 * 2048,
              "corrupted decode %d, %u frames (clean %u)", res, damaged.frames, whole.frames);
        /* Concealment runs at the core rate under implicit SBR: it must not read as a rate change. */
        CHECK(damaged.rateTags == 0, "corrupted decode raised %d Sample Rate Change tags", damaged.rateTags);
        free(damaged.pcm);
        free(bad);
    }

    CHECK(fake_live_allocations() == 0, "%ld allocations leaked", fake_live_allocations());
    free(whole.pcm);
    free(src.data);
}

/* The analytic chirp make_fixtures.py encodes: left 200 Hz → 4 kHz, right 4 kHz → 200 Hz over 1.5 s. */
static double chirp_ideal(int n, int rate, int rising) {
    const double seconds = 1.5;
    const double low = 200.0;
    const double high = 4000.0;
    double       t = (double)n / rate;
    double       f0 = rising ? low : high;
    double       f1 = rising ? high : low;
    return sin(6.283185307179586 * (f0 * t + (f1 - f0) / (2 * seconds) * t * t));
}

static double chirp_correlation(const decoded* d, int lag) {
    double sum = 0;
    int    n;
    for (n = d->rate / 5; n < (int)(1.2 * d->rate) && n < (int)d->frames; n += 2) {
        int m = n + lag;
        if (m < 0) {
            continue;
        }
        sum += d->pcm[(size_t)n * 2] * chirp_ideal(m, d->rate, 1) + d->pcm[(size_t)n * 2 + 1] * chirp_ideal(m, d->rate, 0);
    }
    return sum;
}

/* The lag at which decoded[n] best matches source[n + lag] (coarse, then fine). */
static int chirp_lag(const decoded* d) {
    int    lag;
    int    best = 0;
    double bestSum = -1e300;
    int    center;

    for (lag = -4500; lag <= 4500; lag += 4) {
        double sum = chirp_correlation(d, lag);
        if (sum > bestSum) {
            bestSum = sum;
            best    = lag;
        }
    }
    center = best;
    for (lag = center - 4; lag <= center + 4; lag++) {
        double sum = chirp_correlation(d, lag);
        if (sum > bestSum) {
            bestSum = sum;
            best    = lag;
        }
    }
    return best;
}

/*
 * Output lines up with the encoder's input: fdk's own delay (outputDelay) is trimmed with the container's
 * priming, and drained back at the end. M4A lands on the source sample for sample; ADTS carries no
 * priming information, so it sits exactly the encoder's 2112-sample priming late (Apple's decoder does
 * the same).
 */
static void test_alignment(void) {
    static const struct { const char* name; int rate; int lag; } cases[] = {
        { "chirp_lc_44k.m4a", 44100, 0 },
        { "chirp_he_48k.m4a", 48000, 0 },
        { "chirp_lc_44k.aac", 44100, -2112 },
    };
    unsigned int i;

    printf("alignment\n");
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        blob        src = load(cases[i].name);
        decoded     d;
        FMOD_RESULT res = decode_blob(src.data, src.size, 4096, &d);
        int         lag;

        CHECK(res == FMOD_OK && d.channels == 2, "%s: decode %d", cases[i].name, res);
        if (res == FMOD_OK && d.channels == 2) {
            lag = chirp_lag(&d);
            printf("  %s: output[n] = source[n %+d]\n", cases[i].name, lag);
            CHECK(lag == cases[i].lag, "%s: output sits at source[n %+d], want %+d", cases[i].name, lag, cases[i].lag);
            if (cases[i].lag == 0) {
                /* The drain restored the tail: the last 50 ms still carries the chirp. */
                unsigned int want = (unsigned int)(1.5 * cases[i].rate);
                unsigned int tail = (unsigned int)cases[i].rate / 20;
                CHECK(d.frames == want, "%s: decoded %u frames, want %u", cases[i].name, d.frames, want);
                CHECK(d.frames >= tail && rms(d.pcm + (size_t)(d.frames - tail) * 2, tail * 2) > 4000,
                      "%s: the last 50 ms is near-silent (drain lost)", cases[i].name);
            }
            free(d.pcm);
        }
        free(src.data);
    }
    CHECK(fake_live_allocations() == 0, "alignment: %ld allocations leaked", fake_live_allocations());
}

/*
 * Stack depth. fdk-aac keeps its scratch on the stack (libSYS genericStds.h C_ALLOC_SCRATCH_START):
 * CAacDecoder_DecodeFrame alone is a 35.9 KB frame (clang -O2, arm64), and a full decode peaks near
 * 50 KB. FMOD runs codec reads on its STREAM thread (96 KiB by default) and opens on NONBLOCKING
 * (112 KiB), under 2x headroom; the AMP client raises both to 192 KiB (FMOD.Thread.SetAttributes before
 * the first System). Each fixture's open, rewind, header-walk seek and decode run on a painted stack;
 * the high-water mark must leave 3x headroom under 192 KiB.
 */
#define STACK_SIZE  (256u * 1024u)
#define STACK_LIMIT (64u * 1024u)

typedef struct stack_job {
    blob        src;
    FMOD_RESULT res;
} stack_job;

static void* stack_job_run(void* arg) {
    stack_job* job = (stack_job*)arg;
    fake_file  file;
    decoded    out;

    fake_file_init(&file, job->src.data, job->src.size);
    job->res = open_file(&file, 0);
    if (job->res == FMOD_OK) {
        codec()->setposition(&file.state, 0, 0, FMOD_TIMEUNIT_PCM);
        codec()->setposition(&file.state, 0, (unsigned int)file.state.waveformat->frequency, FMOD_TIMEUNIT_PCM);
        job->res = read_all(&file, 4096, &out);
        free(out.pcm);
        close_file(&file);
    }
    return NULL;
}

static unsigned int peak_stack(stack_job* job) {
    unsigned char* stack = NULL;
    pthread_attr_t attr;
    pthread_t      thread;
    unsigned int   untouched = 0;

    if (posix_memalign((void**)&stack, 16384, STACK_SIZE) != 0) {
        return STACK_SIZE;
    }
    memset(stack, 0xA5, STACK_SIZE);
    pthread_attr_init(&attr);
    pthread_attr_setstack(&attr, stack, STACK_SIZE);
    pthread_create(&thread, &attr, stack_job_run, job);
    pthread_join(thread, NULL);
    pthread_attr_destroy(&attr);
    while (untouched < STACK_SIZE && stack[untouched] == 0xA5) {
        untouched++;   /* the stack grows down: the painted floor survives */
    }
    free(stack);
    return STACK_SIZE - untouched;
}

static void test_stack_depth(void) {
    unsigned int i;
    unsigned int worst = 0;

    printf("stack depth\n");
    for (i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
        stack_job    job;
        unsigned int used;

        job.src = load(fixtures[i].name);
        job.res = FMOD_OK;
        used    = peak_stack(&job);
        CHECK(job.res == FMOD_OK, "%s: decode on the painted stack %d", fixtures[i].name, job.res);
        if (used > worst) {
            worst = used;
        }
        printf("  %-26s %6u bytes\n", fixtures[i].name, used);
        free(job.src.data);
    }
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
    worst = 0;   /* ASan frames are several times larger; the budget applies to release builds */
#endif
#endif
    CHECK(worst <= STACK_LIMIT, "peak stack %u bytes, want at most %u (3x headroom under 192 KiB)", worst, STACK_LIMIT);
}

static void expect_format_error(const char* what, const unsigned char* data, unsigned int size) {
    fake_file   file;
    FMOD_RESULT res;

    fake_file_init(&file, data, size);
    res = open_file(&file, 0);
    CHECK(res == FMOD_ERR_FORMAT, "%s: open %d, want FMOD_ERR_FORMAT", what, res);
    if (res == FMOD_OK) {
        close_file(&file);
    }
    CHECK(fake_live_allocations() == 0, "%s: %ld allocations leaked", what, fake_live_allocations());
}

static void test_rejects(void) {
    static unsigned char buf[65536];
    unsigned int         n;
    unsigned int         seed = 99;
    fake_file            file;
    FMOD_RESULT          res;

    printf("rejects\n");

    /* MPEG-1 Layer III, 128 kbps, 44.1 kHz: 417-byte frames. */
    memset(buf, 0, sizeof(buf));
    for (n = 0; n + 417 <= 20 * 417; n += 417) {
        buf[n] = 0xFF; buf[n + 1] = 0xFB; buf[n + 2] = 0x90; buf[n + 3] = 0x64;
    }
    expect_format_error("mp3", buf, 20 * 417);

    memset(buf, 0, sizeof(buf));
    memcpy(buf, "RIFF\x24\x00\x01\x00WAVEfmt ", 16);
    expect_format_error("wav", buf, 4096);
    memcpy(buf, "OggS", 4);
    expect_format_error("ogg", buf, 4096);
    memcpy(buf, "\x00\x00\x00\x20" "ftypM4A ", 12);
    expect_format_error("ftyp without moov", buf, 4096);

    /* A foreign magic ahead of real ADTS frames: the resync would skip these bytes and open the stream, so
       only the magic check rejects them (formats FMOD tries after ampaac, or level with it: FLAC, AIFF). */
    {
        static const struct { const char* name; const char* magic; unsigned int size; } tagged[] = {
            { "flac magic + adts", "fLaC", 4 },
            { "aiff magic + adts", "FORM\x00\x00\x51\x00" "AIFF", 12 },
            { "ogg magic + adts",  "OggS", 4 },
            { "wav magic + adts",  "RIFF\x24\x00\x01\x00" "WAVE", 12 },
        };
        blob           adts = load("adts_lc_44k_stereo.aac");
        unsigned char* data = (unsigned char*)malloc(adts.size + 12);
        unsigned int   t;

        for (t = 0; t < sizeof(tagged) / sizeof(tagged[0]); t++) {
            memcpy(data, tagged[t].magic, tagged[t].size);
            memcpy(data + tagged[t].size, adts.data, adts.size);
            expect_format_error(tagged[t].name, data, adts.size + tagged[t].size);
        }
        free(data);
        free(adts.data);
    }

    for (n = 0; n < sizeof(buf); n++) {
        seed = seed * 1103515245u + 12345u;
        buf[n] = (unsigned char)(seed >> 16);
    }
    expect_format_error("random", buf, sizeof(buf));
    expect_format_error("empty", buf, 0);
    expect_format_error("tiny", buf, 5);

    /* Before the data shows ftyp or an ADTS chain, a file error answers FMOD_ERR_FORMAT: FMOD's own codecs
       still get their turn. */
    fake_file_init(&file, buf, sizeof(buf));
    file.failAtPos = FMOD_ERR_NET_SOCKET_ERROR;
    file.failPos   = 0;
    res = open_file(&file, 0);
    CHECK(res == FMOD_ERR_FORMAT, "socket error before any AAC evidence: %d, want FMOD_ERR_FORMAT", res);
    CHECK(fake_live_allocations() == 0, "socket error: %ld allocations leaked", fake_live_allocations());

    /* After ftyp it is a load failure: the moov read at the end of the file fails. */
    {
        blob m4a = load("m4a_lc_44k_moovend.m4a");
        fake_file_init(&file, m4a.data, m4a.size);
        file.failAtPos = FMOD_ERR_NET_SOCKET_ERROR;
        file.failPos   = 8192;
        res = open_file(&file, 0);
        CHECK(res == FMOD_ERR_NET_SOCKET_ERROR, "socket error in the moov read: %d, want FMOD_ERR_NET_SOCKET_ERROR", res);
        CHECK(fake_live_allocations() == 0, "moov socket error: %ld allocations leaked", fake_live_allocations());
        free(m4a.data);
    }

    /* Mid-stream, reads return the error after the frames before it, not an end of stream. */
    {
        blob    adts = load("adts_lc_44k_stereo.aac");
        decoded out;
        fake_file_init(&file, adts.data, adts.size);
        file.failAtPos = FMOD_ERR_NET_SOCKET_ERROR;
        file.failPos   = adts.size / 2;
        res = open_file(&file, 0);
        CHECK(res == FMOD_OK, "mid-stream fault: open %d", res);
        if (res == FMOD_OK) {
            res = read_all(&file, 4096, &out);
            CHECK(res == FMOD_ERR_NET_SOCKET_ERROR && out.frames > 0,
                  "mid-stream socket error: %d after %u frames, want FMOD_ERR_NET_SOCKET_ERROR after some", res, out.frames);
            free(out.pcm);
            close_file(&file);
        }
        CHECK(fake_live_allocations() == 0, "mid-stream fault: %ld allocations leaked", fake_live_allocations());
        free(adts.data);
    }
}

/* A long ADTS stream (the LC fixture's frames repeated past the index's 4096 anchors x 8 frames): the index
   halves itself as the walk fills it, and seeks through it still land exactly. */
static void test_long_adts(void) {
    blob           src = load("adts_lc_44k_stereo.aac");
    unsigned int   copies = 260;   /* 132 frames each: 34,320 frames, ~13 min */
    unsigned int   size = src.size * copies;
    unsigned char* data = (unsigned char*)malloc(size);
    decoded        whole;
    fake_file      file;
    FMOD_RESULT    res;
    unsigned int   n;

    printf("long adts\n");
    for (n = 0; n < copies; n++) {
        memcpy(data + (size_t)n * src.size, src.data, src.size);
    }
    res = decode_blob(data, size, 1 << 16, &whole);
    CHECK(res == FMOD_OK && whole.frames > 34000u * 1024u, "long decode %d, %u frames", res, whole.frames);

    fake_file_init(&file, data, size);
    res = open_file(&file, FMOD_ACCURATETIME);
    CHECK(res == FMOD_OK && file.state.waveformat->lengthpcm == whole.frames,
          "long ACCURATETIME length %u, want %u", res == FMOD_OK ? file.state.waveformat->lengthpcm : 0, whole.frames);
    if (res == FMOD_OK) {
        ampaac_codec* aac = (ampaac_codec*)file.state.plugindata;
        unsigned int  targets[] = { whole.frames - 44100 * 3, whole.frames / 2 + 777, 1024 * 1000 + 5 };
        unsigned int  t;

        CHECK(aac->indexStride > 8, "index stride %u: the walk never filled the index", aac->indexStride);
        for (t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
            unsigned int got = 0;
            short        pcm[4096 * 2];
            unsigned int count;
            double       diff = 0;
            unsigned int i;

            res = codec()->setposition(&file.state, 0, targets[t], FMOD_TIMEUNIT_PCM);
            CHECK(res == FMOD_OK, "long seek %u: %d", targets[t], res);
            res = codec()->read(&file.state, pcm, 4096, &got);
            count = got < whole.frames - targets[t] ? got : whole.frames - targets[t];
            for (i = 0; i < count * 2; i++) {
                double d = fabs((double)pcm[i] - whole.pcm[(size_t)targets[t] * 2 + i]);
                diff = d > diff ? d : diff;
            }
            CHECK(res == FMOD_OK && count > 0 && diff <= 1, "long seek to %u: max |diff| %.0f over %u frames",
                  targets[t], diff, count);
        }
        close_file(&file);
    }
    free(whole.pcm);
    free(data);
    free(src.data);
    CHECK(fake_live_allocations() == 0, "long adts: %ld allocations leaked", fake_live_allocations());
}

/* fdk's mixer pins output to 1, 2, 6 or 8 channels: 3 channels extend to 6 (3/0/2.1, the new channels zero),
   and a stream whose layout changes mid-way keeps FMOD's format and reaches its end. */
static void test_channels(void) {
    static const struct { const char* name; unsigned int seconds; int parts[2]; } cases[] = {
        { "adts_lc_44k_3ch.aac",             1, { 3, 0 } },
        { "adts_lc_44k_5ch_then_stereo.aac", 2, { 5, 2 } },
    };
    unsigned int i;

    printf("channels\n");
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        blob         src = load(cases[i].name);
        decoded      out;
        unsigned int want = cases[i].seconds * 44100;
        FMOD_RESULT  res = decode_blob(src.data, src.size, 4096, &out);

        CHECK(res == FMOD_OK && out.channels == 6 && out.frames >= want && out.frames <= want + want / 10,
              "%s: %d, %d channels, %u frames (want 6 channels, %u frames plus priming)", cases[i].name, res,
              out.channels, out.frames, want);
        if (res == FMOD_OK && out.channels == 6) {
            unsigned int p;
            for (p = 0; p < 2 && cases[i].parts[p]; p++) {
                /* The middle of each 1 s part; WAV order FL FR FC LFE BL BR. */
                unsigned int from = p * 44100 + 11025;
                int          c;
                for (c = 0; c < 6; c++) {
                    int    carries = cases[i].parts[p] == 2 ? c < 2 : cases[i].parts[p] == 3 ? c < 3 : c != 3;
                    double level = channel_rms(&out, c, from, 22050);
                    CHECK(carries ? level > 1000 : level < 1, "%s part %u: channel %d rms %.1f (%s)", cases[i].name, p,
                          c, level, carries ? "want signal" : "want silence");
                }
            }
            free(out.pcm);
        }
        free(src.data);
    }
    CHECK(fake_live_allocations() == 0, "channels: %ld allocations leaked", fake_live_allocations());
}

static unsigned char* repeat_blob(const blob* src, unsigned int copies, unsigned int* size) {
    unsigned char* data = (unsigned char*)malloc((size_t)src->size * copies);
    unsigned int   n;

    for (n = 0; n < copies; n++) {
        memcpy(data + (size_t)n * src->size, src->data, src->size);
    }
    *size = src->size * copies;
    return data;
}

/* Zeroes the payload of ADTS frames [first, last), headers kept: the decoder conceals them. */
static void zero_adts_payloads(unsigned char* data, unsigned int size, unsigned int first, unsigned int last) {
    unsigned int       at = 0;
    unsigned int       frame = 0;
    ampaac_adts_header hdr;

    while (at + 7 <= size && ampaac_adts_parse(data + at, size - at, &hdr) && at + hdr.frameLength <= size) {
        if (frame >= first && frame < last) {
            memset(data + at + hdr.headerLength, 0, hdr.frameLength - hdr.headerLength);
        }
        at += hdr.frameLength;
        frame++;
    }
}

static double max_diff(const short* a, const short* b, size_t samples) {
    double diff = 0;
    size_t i;

    for (i = 0; i < samples; i++) {
        double d = fabs((double)a[i] - b[i]);
        diff = d > diff ? d : diff;
    }
    return diff;
}

/* Reads `frames` frames at the current position into pcm (channels interleaved); returns the count read. */
static unsigned int read_frames(fake_file* file, short* pcm, unsigned int frames) {
    unsigned int total = 0;
    int          channels = file->state.waveformat->channels;

    while (total < frames) {
        unsigned int got = 0;
        FMOD_RESULT  res = codec()->read(&file->state, pcm + (size_t)total * channels, frames - total, &got);
        total += got;
        if (res != FMOD_OK || got == 0) {
            break;
        }
    }
    return total;
}

/* One failed read (a transient 503) is retried at once: the decode matches a clean one. A timeout is not
   retried: its error stands. */
static void test_retry(void) {
    static const char* names[] = { "adts_lc_44k_stereo.aac", "m4a_lc_44k_stereo.m4a" };
    fake_file          file;
    FMOD_RESULT        res;
    unsigned int       i;

    printf("retry\n");
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        blob    src = load(names[i]);
        decoded clean;
        decoded faulted;

        res = decode_blob(src.data, src.size, 4096, &clean);
        CHECK(res == FMOD_OK, "%s: clean decode %d", names[i], res);

        fake_file_init(&file, src.data, src.size);
        file.failAtPos = FMOD_ERR_HTTP_SERVER_ERROR;
        file.failPos   = src.size / 2;
        file.failTimes = 1;
        res = open_file(&file, 0);
        CHECK(res == FMOD_OK, "%s: transient fault open %d", names[i], res);
        if (res == FMOD_OK) {
            res = read_all(&file, 4096, &faulted);
            CHECK(res == FMOD_OK && file.failed == 1 && faulted.frames == clean.frames
                      && memcmp(faulted.pcm, clean.pcm, (size_t)clean.frames * clean.channels * sizeof(short)) == 0,
                  "%s: after one 503, %d with %u frames (clean %u), %u failures", names[i], res, faulted.frames,
                  clean.frames, file.failed);
            free(faulted.pcm);
            close_file(&file);
        }

        fake_file_init(&file, src.data, src.size);
        file.failAtPos = FMOD_ERR_NET_SOCKET_ERROR;
        file.failPos   = src.size / 2;
        file.failTimes = 1;
        res = open_file(&file, 0);
        if (res == FMOD_OK) {
            res = read_all(&file, 4096, &faulted);
            CHECK(res == FMOD_ERR_NET_SOCKET_ERROR, "%s: a timeout retried: %d, want FMOD_ERR_NET_SOCKET_ERROR", names[i], res);
            free(faulted.pcm);
            close_file(&file);
        }
        free(clean.pcm);
        free(src.data);
    }

    CHECK(fake_live_allocations() == 0, "retry: %ld allocations leaked", fake_live_allocations());
}

/* Damage a stream conceals through, and the limits that end one without taking the end for its length. */
static void test_limits(void) {
    fake_file   file;
    FMOD_RESULT res;

    printf("limits\n");
    /* 12 s of damaged frames (past the 10 s concealment limit) in a 60 s ADTS stream: with a size, the stream
       conceals through and decodes what follows; without one (a live source), the limit ends it, and that
       end is not taken for the stream's real length: a later seek past it still plays. */
    {
        blob           src = load("adts_lc_44k_stereo.aac");
        unsigned int   size;
        unsigned char* data = repeat_blob(&src, 20, &size);   /* 132 frames (3 s) per copy */
        decoded        clean;
        decoded        damaged;

        res = decode_blob(data, size, 4096, &clean);
        zero_adts_payloads(data, size, 264, 264 + 517);       /* 6-18 s */

        res = decode_blob(data, size, 4096, &damaged);
        CHECK(res == FMOD_OK && damaged.frames + 8 * 1024 >= clean.frames && damaged.frames <= clean.frames + 8 * 1024,
              "damaged ADTS with a size: %d, %u frames (clean %u)", res, damaged.frames, clean.frames);
        if (res == FMOD_OK && damaged.frames > 25 * 44100) {
            double level = channel_rms(&damaged, 0, 21 * 44100, 44100);
            CHECK(level > 1000, "damaged ADTS with a size: rms %.1f at 21 s, want the stream decoded past the damage", level);
        }
        if (res == FMOD_OK) {
            free(damaged.pcm);
        }

        fake_file_init(&file, data, size);
        file.sizeUnknown = 1;
        res = open_file(&file, 0);
        CHECK(res == FMOD_OK, "damaged ADTS without a size: open %d", res);
        if (res == FMOD_OK) {
            short        pcm[4096 * 2];
            unsigned int got;
            decoded      one;

            res = read_all(&file, 4096, &damaged);
            CHECK(res == FMOD_OK && damaged.frames < 18 * 44100 && damaged.frames > 15 * 44100,
                  "damaged ADTS without a size: %d, %u frames, want an end ~10 s into the damage at 6 s", res,
                  damaged.frames);
            free(damaged.pcm);
            res = codec()->setposition(&file.state, 0, 40 * 44100, FMOD_TIMEUNIT_PCM);
            got = read_frames(&file, pcm, 4096);
            memset(&one, 0, sizeof(one));
            one.pcm      = pcm;
            one.channels = 2;
            one.frames   = got;
            CHECK(res == FMOD_OK && got == 4096 && channel_rms(&one, 0, 0, got) > 1000,
                  "seek to 40 s after the limit ended the stream: %d, %u frames, rms %.1f", res, got,
                  got ? channel_rms(&one, 0, 0, got) : 0.0);
            close_file(&file);
        }
        free(clean.pcm);
        free(data);
        free(src.data);
    }

    /* An M4A conceals through damaged access units and decodes what follows (its table bounds it; the limit
       does not apply), to its exact length. */
    {
        blob           src = load("m4a_lc_44k_stereo.m4a");
        unsigned char* data = (unsigned char*)malloc(src.size);
        decoded        clean;
        decoded        damaged;

        memcpy(data, src.data, src.size);
        res = decode_blob(src.data, src.size, 4096, &clean);
        fake_file_init(&file, src.data, src.size);
        if (res == FMOD_OK && open_file(&file, 0) == FMOD_OK) {
            ampaac_codec* aac = (ampaac_codec*)file.state.plugindata;
            unsigned int  au;

            for (au = 43; au < 86 && au < aac->mp4.samples; au++) {   /* 1-2 s */
                unsigned int len = aac->mp4.sizes ? ((unsigned int)aac->mp4.sizes[au * 4] << 24 | (unsigned int)aac->mp4.sizes[au * 4 + 1] << 16
                                                     | (unsigned int)aac->mp4.sizes[au * 4 + 2] << 8 | aac->mp4.sizes[au * 4 + 3])
                                                  : aac->mp4.uniform;
                memset(data + aac->mp4.offsets[au], 0, len);
            }
            close_file(&file);
            ampaac_conceal_limit_ms = 200;
            res = decode_blob(data, src.size, 4096, &damaged);
            ampaac_conceal_limit_ms = AMPAAC_CONCEAL_LIMIT_MS;
            CHECK(res == FMOD_OK && damaged.frames == clean.frames, "damaged M4A: %d, %u frames (clean %u)", res,
                  damaged.frames, clean.frames);
            if (res == FMOD_OK && damaged.frames > (unsigned int)(2.6 * 44100)) {
                double level = channel_rms(&damaged, 0, (unsigned int)(2.2 * 44100), 44100 / 4);
                CHECK(level > 1000, "damaged M4A: rms %.1f at 2.2 s, want the stream decoded past the damage", level);
            }
            if (res == FMOD_OK) {
                free(damaged.pcm);
            }
            free(clean.pcm);
        }
        free(data);
        free(src.data);
    }
    CHECK(fake_live_allocations() == 0, "limits: %ld allocations leaked", fake_live_allocations());
}

/* A seek to the start after a mid-stream rate change plays the start as the first decode did: no frame of
   the opening format is dropped as a start-up transient. */
static void test_rate_restart(void) {
    static const struct { const char* first; const char* second; } cases[] = {
        { "adts_lc_44k_stereo.aac", "adts_lc_22k_mono.aac" },
        { "adts_he_48k_stereo.aac", "adts_lc_44k_stereo.aac" },
    };
    unsigned int i;

    printf("rate restart\n");
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        blob           a = load(cases[i].first);
        blob           b = load(cases[i].second);
        unsigned int   size = a.size + b.size;
        unsigned char* data = (unsigned char*)malloc(size);
        decoded        whole;
        fake_file      file;
        FMOD_RESULT    res;
        int            pass;

        memcpy(data, a.data, a.size);
        memcpy(data + a.size, b.data, b.size);
        res = decode_blob(data, size, 4096, &whole);
        CHECK(res == FMOD_OK && whole.rateTags >= 1, "%s + %s: %d, %d rate tags", cases[i].first, cases[i].second, res,
              whole.rateTags);
        /* pass 0: seek back while playing past the change; pass 1: after the end of the stream */
        for (pass = 0; pass < 2 && res == FMOD_OK; pass++) {
            short*       pcm = (short*)malloc(8192 * 2 * sizeof(short));
            unsigned int got;

            fake_file_init(&file, data, size);
            if (open_file(&file, 0) != FMOD_OK) {
                free(pcm);
                break;
            }
            if (pass == 0) {
                short*       skip = (short*)malloc((size_t)whole.frames * 2 * sizeof(short));
                unsigned int past = read_frames(&file, skip, whole.frames - 4096);
                (void)past;
                free(skip);
            } else {
                decoded rest;
                read_all(&file, 4096, &rest);
                free(rest.pcm);
            }
            res = codec()->setposition(&file.state, 0, 0, FMOD_TIMEUNIT_PCM);
            got = read_frames(&file, pcm, 8192);
            CHECK(res == FMOD_OK && got == 8192 && max_diff(pcm, whole.pcm, (size_t)got * 2) == 0,
                  "%s + %s: seek to 0 %s: %d, %u frames, max |diff| %.0f against the first decode", cases[i].first,
                  cases[i].second, pass ? "after the end" : "while playing", res, got,
                  got ? max_diff(pcm, whole.pcm, (size_t)got * 2) : -1.0);
            close_file(&file);
            free(pcm);
        }
        if (whole.pcm) {
            free(whole.pcm);
        }
        free(data);
        free(a.data);
        free(b.data);
    }
    CHECK(fake_live_allocations() == 0, "rate restart: %ld allocations leaked", fake_live_allocations());
}

/* ADTS seeks: a target within 256 KiB of the last known frame is walked to and lands exactly; a farther one
   is estimated at once, without reading the bytes between (a netstream would wait on each). */
static void test_far_seek(void) {
    blob           src = load("adts_lc_44k_stereo.aac");
    unsigned int   size;
    unsigned char* data = repeat_blob(&src, 40, &size);   /* 120 s, 6.9 KB/s: 256 KiB is 38 s of it */
    decoded        whole;
    fake_file      file;
    FMOD_RESULT    res;
    short          pcm[4096 * 2];

    printf("far seek\n");
    res = decode_blob(data, size, 4096, &whole);
    CHECK(res == FMOD_OK && whole.frames > 119 * 44100, "120 s ADTS: %d, %u frames", res, whole.frames);

    fake_file_init(&file, data, size);
    if (res == FMOD_OK && open_file(&file, 0) == FMOD_OK) {
        unsigned int reads;
        unsigned int seeks;
        unsigned int got;
        unsigned int near = 12 * 44100;   /* ~80 KB ahead */
        unsigned int far = 100 * 44100;   /* ~610 KB past the 12 s the near seek walked to */
        decoded      one;

        read_frames(&file, pcm, 4096);
        res = codec()->setposition(&file.state, 0, near, FMOD_TIMEUNIT_PCM);
        got = read_frames(&file, pcm, 4096);
        CHECK(res == FMOD_OK && got == 4096 && max_diff(pcm, whole.pcm + (size_t)near * 2, (size_t)got * 2) <= 1,
              "near seek to 12 s: %d, %u frames, max |diff| %.0f", res, got,
              got ? max_diff(pcm, whole.pcm + (size_t)near * 2, (size_t)got * 2) : -1.0);

        reads = file.reads;
        seeks = file.seeks;
        res   = codec()->setposition(&file.state, 0, far, FMOD_TIMEUNIT_PCM);
        CHECK(res == FMOD_OK && file.reads - reads <= 8 && file.seeks > seeks,
              "far seek to 100 s: %d after %u reads and %u source seeks, want the bytes between skipped", res,
              file.reads - reads, file.seeks - seeks);
        got = read_frames(&file, pcm, 4096);
        memset(&one, 0, sizeof(one));
        one.pcm      = pcm;
        one.channels = 2;
        one.frames   = got;
        CHECK(got == 4096 && channel_rms(&one, 0, 0, got) > 1000, "far seek to 100 s: %u frames, rms %.1f", got,
              got ? channel_rms(&one, 0, 0, got) : 0.0);
        close_file(&file);
    }
    if (whole.pcm) {
        free(whole.pcm);
    }
    free(data);
    free(src.data);
    CHECK(fake_live_allocations() == 0, "far seek: %ld allocations leaked", fake_live_allocations());
}

int main(int argc, char** argv) {
    unsigned int i;

    if (argc < 2) {
        fprintf(stderr, "usage: ampaac_test <fixtures dir>\n");
        return 2;
    }
    fixtureDir = argv[1];

    for (i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
        test_fixture(&fixtures[i]);
    }
    test_alignment();
    test_stack_depth();
    test_rejects();
    test_channels();
    test_long_adts();
    test_retry();
    test_limits();
    test_rate_restart();
    test_far_seek();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
