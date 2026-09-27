/*
 * fmod_harness — drives a real FMOD Core library with ampaac registered, the way the AMP client does:
 * a netstream opened NONBLOCKING | CREATESTREAM, played on NOSOUND output, polled like FmodPlayer.
 *
 *   fmod_harness <libfmod> <libampaac|-> <scenario> <url> [seek-ms]
 *     play       open, report format and length, play to the end
 *     seek       play 1 s, Channel::setPosition(seek-ms), play to the end
 *     getlength  open, Sound::getLength five times, play 1 s, again
 *     open       open to READY (or ERROR) and report the time taken
 *     teardown   release the System while the open waits for body bytes (the server holds the body), then
 *                stay alive for seek-ms more (default 2000) while the open resolves; AMPAAC_ABANDON=1 first
 *                drops the network timeout to 50 ms and waits for the open to settle
 *     pause      play 2 s, pause for ms, resume, play to the end
 *     seekdown   play 1 s, Channel::setPosition(ms), wait AMPAAC_SEEKDOWN_WAIT seconds (default 1.5), then
 *                release the System mid-seek and stay alive 5 s; AMPAAC_ABANDON=1 first drops the network
 *                timeout to 50 ms and waits up to 1 s for the sound to settle (the client's CloseSystem)
 *
 *   AMPAAC_REGISTER=after|before|none   when to register the codec (default after System::init)
 *   AMPAAC_PRIORITY=<n>                 codec priority (default 0)
 *   AMPAAC_WAVWRITER=<file.wav>         record the mix with FMOD's realtime WAV writer instead of NOSOUND
 *   AMPAAC_STREAM_BUFFER=<bytes>        stream file buffer (default 131072, as the AMP client; 0 = FMOD's default)
 *   AMPAAC_PLAY_TIMEOUT=<seconds>       give up on a channel that has not stopped (default 600)
 *   AMPAAC_NET_TIMEOUT=<ms>             FMOD's network timeout (default 10000; the AMP client uses 73000)
 *   AMPAAC_OPEN_WAIT=<seconds>          give up on an open that is neither READY nor ERROR (default 20)
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
    X(FMOD_Channel_SetPosition)               \
    X(FMOD_Channel_SetPaused)

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
    const char* limit = getenv("AMPAAC_OPEN_WAIT");
    double      openWait = limit ? atof(limit) : 20;
    double      start = now();
    while (now() - start < openWait) {
        unsigned int percent;
        FMOD_BOOL    starving;
        FMOD_BOOL    diskbusy;
        FMOD_RESULT  res;
        p_FMOD_System_Update(system);
        res = p_FMOD_Sound_GetOpenState(sound, state, &percent, &starving, &diskbusy);
        if (*state == FMOD_OPENSTATE_ERROR) {
            printf("open_result=%d\n", res);   /* why a non-blocking open failed */
        }
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

/* Plays until the channel stops (FmodPlayer: READY and the channel stopped = end of track), logging each
   change of the sound's open state, starving flag, and getOpenState result. `action` is "seek" (at 1.0 s),
   "pause" (at 2.0 s, for ms), "seekdown" (seek at 1.0 s, then release the System), or NULL. Returns 1 when
   the System was released here. */
static int play_to_end(FMOD_SYSTEM* system, FMOD_SOUND* sound, FMOD_CHANNEL* channel, const char* action,
                       unsigned int ms) {
    const char*    limit = getenv("AMPAAC_PLAY_TIMEOUT");
    const char*    wait = getenv("AMPAAC_SEEKDOWN_WAIT");
    double         playTimeout = limit ? atof(limit) : 600;
    double         seekdownWait = wait ? atof(wait) : 1.5;
    double         start = now();
    double         seekedAt = 0;
    unsigned int   lastPos = 0;
    int            seeked = 0;
    int            paused = 0;
    FMOD_OPENSTATE lastState = (FMOD_OPENSTATE)-1;
    FMOD_BOOL      lastStarving = 0;
    FMOD_RESULT    lastResult = FMOD_OK;
    int            isSeek = action && (strcmp(action, "seek") == 0 || strcmp(action, "seekdown") == 0);

    for (;;) {
        FMOD_BOOL      playing = 0;
        unsigned int   pos = 0;
        FMOD_OPENSTATE state;
        unsigned int   percent;
        FMOD_BOOL      starving = 0;
        FMOD_BOOL      diskbusy;
        FMOD_RESULT    res;

        p_FMOD_System_Update(system);
        res = p_FMOD_Sound_GetOpenState(sound, &state, &percent, &starving, &diskbusy);
        if (state != lastState || starving != lastStarving || res != lastResult) {
            printf("t=%.3f openstate=%d starving=%d result=%d position_ms=%u\n", now() - start, (int)state,
                   (int)starving, (int)res, lastPos);
            lastState = state;
            lastStarving = starving;
            lastResult = res;
        }
        if (p_FMOD_Channel_IsPlaying(channel, &playing) != FMOD_OK || !playing) {
            break;
        }
        if (p_FMOD_Channel_GetPosition(channel, &pos, FMOD_TIMEUNIT_MS) == FMOD_OK) {
            lastPos = pos;
        }
        if (isSeek && !seeked && now() - start >= 1.0) {
            seeked = 1;
            seekedAt = now();
            check(p_FMOD_Channel_SetPosition(channel, ms, FMOD_TIMEUNIT_MS), "setPosition");
            printf("seek_at_s=%.3f seek_to_ms=%u\n", now() - start, ms);
        }
        if (seeked && strcmp(action, "seekdown") == 0 && now() - seekedAt >= seekdownWait) {
            printf("seekdown: openstate=%d at %.3f s\n", (int)state, now() - start);
            if (getenv("AMPAAC_ABANDON")) {
                double abandonAt = now();
                p_FMOD_System_SetNetworkTimeout(system, 50);
                while (now() - abandonAt < 1.0) {
                    p_FMOD_System_Update(system);
                    res = p_FMOD_Sound_GetOpenState(sound, &state, &percent, &starving, &diskbusy);
                    if (res != FMOD_OK || state == FMOD_OPENSTATE_READY || state == FMOD_OPENSTATE_ERROR) {
                        break;
                    }
                    usleep(2000);
                }
                printf("abandoned: openstate=%d result=%d after %.3f s\n", (int)state, (int)res, now() - abandonAt);
            }
            printf("releasing the System at %.3f s\n", now() - start);
            fflush(stdout);
            p_FMOD_System_Close(system);
            p_FMOD_System_Release(system);
            printf("released at %.3f s\n", now() - start);
            fflush(stdout);
            usleep(5000000);
            printf("survived 5 s after release\n");
            return 1;
        }
        if (action && strcmp(action, "pause") == 0) {
            if (paused == 0 && now() - start >= 2.0) {
                paused = 1;
                check(p_FMOD_Channel_SetPaused(channel, 1), "setPaused");
                printf("paused_at_s=%.3f position_ms=%u\n", now() - start, lastPos);
            } else if (paused == 1 && now() - start >= 2.0 + ms / 1000.0) {
                paused = 2;
                check(p_FMOD_Channel_SetPaused(channel, 0), "setPaused");
                printf("resumed_at_s=%.3f position_ms=%u\n", now() - start, lastPos);
            }
        }
        if (now() - start > playTimeout) {
            printf("error=timeout (channel still playing after %.0f s)\n", playTimeout);
            break;
        }
        usleep(5000);
    }
    printf("stopped_after_s=%.3f last_position_ms=%u\n", now() - start, lastPos);
    return 0;
}

int main(int argc, char** argv) {
    const char*   scenario;
    const char*   url;
    const char*   when = getenv("AMPAAC_REGISTER");
    const char*   prio = getenv("AMPAAC_PRIORITY");
    const char*   wavPath = getenv("AMPAAC_WAVWRITER");
    const char*   bufferEnv = getenv("AMPAAC_STREAM_BUFFER");
    unsigned int  streamBuffer = bufferEnv ? (unsigned int)atoi(bufferEnv) : 128 * 1024;
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
        fprintf(stderr, "usage: fmod_harness <libfmod> <libampaac|-> <play|seek|getlength|open|teardown|pause|seekdown> <url> [ms]\n");
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
    check(p_FMOD_System_SetNetworkTimeout(system, getenv("AMPAAC_NET_TIMEOUT") ? atoi(getenv("AMPAAC_NET_TIMEOUT")) : 10000),
          "setNetworkTimeout");
    if (streamBuffer > 0) {
        check(p_FMOD_System_SetStreamBufferSize(system, streamBuffer, FMOD_TIMEUNIT_RAWBYTES), "setStreamBufferSize");
    }
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
    if (strcmp(scenario, "teardown") == 0) {
        double       begin = now();
        unsigned int percent;
        FMOD_BOOL    starving;
        FMOD_BOOL    diskbusy;

        do {
            p_FMOD_System_Update(system);
            p_FMOD_Sound_GetOpenState(sound, &state, &percent, &starving, &diskbusy);
            if (state == FMOD_OPENSTATE_BUFFERING || state == FMOD_OPENSTATE_READY || state == FMOD_OPENSTATE_ERROR) {
                break;
            }
            usleep(1000);
        } while (now() - begin < 5);
        printf("teardown_state=%d after %.3f s\n", (int)state, now() - begin);
        if (getenv("AMPAAC_ABANDON")) {
            /* The client's workaround: a short network timeout lets the pending open settle first. */
            p_FMOD_System_SetNetworkTimeout(system, 50);
            opened = wait_ready(system, sound, &state);
            printf("abandoned: openstate=%d after %.3f s\n", (int)state, opened);
        }
        fflush(stdout);
        p_FMOD_System_Close(system);
        p_FMOD_System_Release(system);
        printf("released at %.3f s\n", now() - begin);
        fflush(stdout);
        usleep(seekMs * 1000u);   /* the held body arrives and the open resolves */
        printf("survived %.3f s after release\n", seekMs / 1000.0);
        return 0;
    }
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
    if (play_to_end(system, sound, channel, strcmp(scenario, "play") == 0 ? NULL : scenario, seekMs)) {
        return 0;   /* seekdown released the System */
    }
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
