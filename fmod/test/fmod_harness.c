/*
 * fmod_harness — drives a real FMOD Core library with ampaac registered, the way the AMP client does:
 * a netstream opened NONBLOCKING | CREATESTREAM, played on NOSOUND output, polled like FmodPlayer.
 *
 *   fmod_harness <libfmod> <libampaac|-> <scenario> <url> [seek-ms]
 *     play       open, report format and length, play to the end
 *     seek       play 1 s, Channel::setPosition(seek-ms), play to the end
 *     getlength  open, Sound::getLength five times, play 1 s, again
 *     open       open to READY (or ERROR) and report the time taken
 *
 *   AMPAAC_REGISTER=after|before|none   when to register the codec (default after System::init)
 *   AMPAAC_PRIORITY=<n>                 codec priority (default 0)
 *   AMPAAC_WAVWRITER=<file.wav>         record the mix with FMOD's realtime WAV writer instead of NOSOUND
 *
 * FMOD's headers come from FMOD_API_INC at build time; FMOD's library is loaded at run time.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fmod.h"

#define FMOD_RUNTIME_VERSION 0x00020314u   /* the FMOD for Unity package AMP ships */

#define FMOD_FUNCTIONS(X)                     \
    X(FMOD_System_Create)                     \
    X(FMOD_System_SetOutput)                  \
    X(FMOD_System_SetSoftwareFormat)          \
    X(FMOD_System_SetStreamBufferSize)        \
    X(FMOD_System_SetNetworkTimeout)          \
    X(FMOD_System_Init)                       \
    X(FMOD_System_RegisterCodec)              \
    X(FMOD_System_CreateSound)                \
    X(FMOD_System_PlaySound)                  \
    X(FMOD_System_Update)                     \
    X(FMOD_System_Close)                      \
    X(FMOD_System_Release)                    \
    X(FMOD_Sound_GetOpenState)                \
    X(FMOD_Sound_GetFormat)                   \
    X(FMOD_Sound_GetLength)                   \
    X(FMOD_Sound_Release)                     \
    X(FMOD_Sound_GetTag)                      \
    X(FMOD_Channel_IsPlaying)                 \
    X(FMOD_Channel_GetPosition)               \
    X(FMOD_Channel_SetPosition)

#define DECLARE(name) static __typeof__(&name) p_##name;
FMOD_FUNCTIONS(DECLARE)

typedef FMOD_CODEC_DESCRIPTION* (F_CALL* describe_fn)(void);

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void check(FMOD_RESULT res, const char* what) {
    if (res != FMOD_OK) {
        printf("error=%s result=%d\n", what, res);
    }
}

static void* load(const char* path) {
    void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "fmod_harness: %s\n", dlerror());
        exit(2);
    }
    return lib;
}

static void register_codec(FMOD_SYSTEM* system, describe_fn describe, unsigned int priority) {
    unsigned int handle = 0;
    FMOD_RESULT  res = p_FMOD_System_RegisterCodec(system, describe(), &handle, priority);
    printf("register=%d handle=%u priority=%u\n", res, handle, priority);
}

/* Polls until the open settles; returns seconds taken (negative on timeout). */
static double wait_ready(FMOD_SYSTEM* system, FMOD_SOUND* sound, FMOD_OPENSTATE* state) {
    double start = now();
    while (now() - start < 20) {
        unsigned int percent;
        FMOD_BOOL    starving;
        FMOD_BOOL    diskbusy;
        p_FMOD_System_Update(system);
        p_FMOD_Sound_GetOpenState(sound, state, &percent, &starving, &diskbusy);
        if (*state == FMOD_OPENSTATE_READY || *state == FMOD_OPENSTATE_ERROR) {
            return now() - start;
        }
        usleep(2000);
    }
    return -1;
}

static void report_length(FMOD_SOUND* sound, const char* when) {
    unsigned int pcm = 0;
    unsigned int ms = 0;
    unsigned int tagMs = 0;
    FMOD_TAG     tag;
    p_FMOD_Sound_GetLength(sound, &pcm, FMOD_TIMEUNIT_PCM);
    p_FMOD_Sound_GetLength(sound, &ms, FMOD_TIMEUNIT_MS);
    /* The codec's length estimate for a stream FMOD reports as unknown length. */
    if (p_FMOD_Sound_GetTag(sound, "AMPAAC_LENGTH_MS", 0, &tag) == FMOD_OK && tag.datalen == sizeof(tagMs)) {
        memcpy(&tagMs, tag.data, sizeof(tagMs));
    }
    printf("length_%s_pcm=%u length_%s_ms=%u tag_%s_ms=%u\n", when, pcm, when, ms, when, tagMs);
}

/* Plays until the channel stops (FmodPlayer: READY and the channel stopped = end of track). */
static void play_to_end(FMOD_SYSTEM* system, FMOD_CHANNEL* channel, double seekAt, unsigned int seekMs) {
    double       start = now();
    unsigned int lastPos = 0;
    int          seeked = 0;

    for (;;) {
        FMOD_BOOL    playing = 0;
        unsigned int pos = 0;

        p_FMOD_System_Update(system);
        if (p_FMOD_Channel_IsPlaying(channel, &playing) != FMOD_OK || !playing) {
            break;
        }
        if (p_FMOD_Channel_GetPosition(channel, &pos, FMOD_TIMEUNIT_MS) == FMOD_OK) {
            lastPos = pos;
        }
        if (!seeked && seekAt > 0 && now() - start >= seekAt) {
            seeked = 1;
            check(p_FMOD_Channel_SetPosition(channel, seekMs, FMOD_TIMEUNIT_MS), "setPosition");
            printf("seek_at_s=%.3f seek_to_ms=%u\n", now() - start, seekMs);
        }
        if (now() - start > 600) {
            printf("error=timeout\n");
            break;
        }
        usleep(5000);
    }
    printf("stopped_after_s=%.3f last_position_ms=%u\n", now() - start, lastPos);
}

int main(int argc, char** argv) {
    const char*   scenario;
    const char*   url;
    const char*   when = getenv("AMPAAC_REGISTER");
    const char*   prio = getenv("AMPAAC_PRIORITY");
    const char*   wavPath = getenv("AMPAAC_WAVWRITER");
    unsigned int  priority = prio ? (unsigned int)atoi(prio) : 0;
    unsigned int  seekMs = argc > 5 ? (unsigned int)atoi(argv[5]) : 2000;
    describe_fn   describe = NULL;
    void*         fmod;
    FMOD_SYSTEM*  system = NULL;
    FMOD_SOUND*   sound = NULL;
    FMOD_CHANNEL* channel = NULL;
    FMOD_OPENSTATE state = FMOD_OPENSTATE_LOADING;
    FMOD_CREATESOUNDEXINFO exinfo;
    FMOD_SOUND_TYPE   type;
    FMOD_SOUND_FORMAT format;
    int    channels = 0;
    int    bits = 0;
    double opened;
    double started;

    if (argc < 5) {
        fprintf(stderr, "usage: fmod_harness <libfmod> <libampaac|-> <play|seek|getlength|open> <url> [seek-ms]\n");
        return 2;
    }
    scenario = argv[3];
    url      = argv[4];
    if (!when) {
        when = "after";
    }

    fmod = load(argv[1]);
#define RESOLVE(name)                                                   \
    p_##name = (__typeof__(p_##name))dlsym(fmod, #name);                \
    if (!p_##name) {                                                    \
        fprintf(stderr, "fmod_harness: %s missing\n", #name);           \
        return 2;                                                       \
    }
    FMOD_FUNCTIONS(RESOLVE)
    if (strcmp(argv[2], "-") != 0 && strcmp(when, "none") != 0) {
        describe = (describe_fn)dlsym(load(argv[2]), "AMPAAC_GetCodecDescription");
    }

    check(p_FMOD_System_Create(&system, FMOD_RUNTIME_VERSION), "System_Create");
    check(p_FMOD_System_SetOutput(system, wavPath ? FMOD_OUTPUTTYPE_WAVWRITER : FMOD_OUTPUTTYPE_NOSOUND), "setOutput");
    check(p_FMOD_System_SetSoftwareFormat(system, 48000, FMOD_SPEAKERMODE_DEFAULT, 0), "setSoftwareFormat");
    check(p_FMOD_System_SetNetworkTimeout(system, 10000), "setNetworkTimeout");
    check(p_FMOD_System_SetStreamBufferSize(system, 128 * 1024, FMOD_TIMEUNIT_RAWBYTES), "setStreamBufferSize");
    if (describe && strcmp(when, "before") == 0) {
        register_codec(system, describe, priority);
    }
    check(p_FMOD_System_Init(system, 63, FMOD_INIT_NORMAL | FMOD_INIT_THREAD_UNSAFE, (void*)wavPath), "init");
    if (describe && strcmp(when, "after") == 0) {
        register_codec(system, describe, priority);
    }

    memset(&exinfo, 0, sizeof(exinfo));
    exinfo.cbsize             = sizeof(exinfo);
    exinfo.suggestedsoundtype = FMOD_SOUND_TYPE_UNKNOWN;
    exinfo.nonblockthreadid   = 0;
    started = now();
    check(p_FMOD_System_CreateSound(system, url, FMOD_CREATESTREAM | FMOD_NONBLOCKING, &exinfo, &sound), "createStream");
    opened = wait_ready(system, sound, &state);
    printf("open_s=%.4f openstate=%d\n", opened, (int)state);
    if (state != FMOD_OPENSTATE_READY) {
        goto done;
    }
    p_FMOD_Sound_GetFormat(sound, &type, &format, &channels, &bits);
    printf("type=%d format=%d channels=%d bits=%d\n", (int)type, (int)format, channels, bits);
    report_length(sound, "open");

    if (strcmp(scenario, "open") == 0) {
        goto done;
    }
    if (strcmp(scenario, "getlength") == 0) {
        int i;
        for (i = 0; i < 5; i++) {
            report_length(sound, "repeat");
        }
    }

    check(p_FMOD_System_PlaySound(system, sound, NULL, 0, &channel), "playSound");
    if (strcmp(scenario, "getlength") == 0) {
        double until = now() + 1.0;
        while (now() < until) {
            p_FMOD_System_Update(system);
            usleep(5000);
        }
        report_length(sound, "after1s");
    }
    play_to_end(system, channel, strcmp(scenario, "seek") == 0 ? 1.0 : 0, seekMs);
    report_length(sound, "end");
    printf("total_s=%.3f\n", now() - started);

done:
    if (sound) {
        p_FMOD_Sound_Release(sound);
    }
    p_FMOD_System_Close(system);
    p_FMOD_System_Release(system);
    return 0;
}
