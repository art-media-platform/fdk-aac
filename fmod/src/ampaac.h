/*
 * ampaac — FMOD codec plugin decoding AAC with the Fraunhofer FDK AAC decoder.
 * Internal declarations shared by the codec, reader and container modules.
 */
#ifndef AMPAAC_H
#define AMPAAC_H

#include "fmod.h"
#include "aacdecoder_lib.h"

#define AMPAAC_PROBE_BYTES   8192u     /* head window searched for an ADTS sync */
#define AMPAAC_READ_BUF      16384u    /* reader read-ahead window */
#define AMPAAC_READ_GRANULE  4096u     /* smallest FMOD file read */
#define AMPAAC_MAX_AU        8192u     /* largest access unit fed to the decoder (ADTS frame_length < 8192) */
#define AMPAAC_MAX_CHANNELS  8
#define AMPAAC_MAX_FRAME     4096      /* largest decoded frame (USAC); HE-AAC is 2048 */
#define AMPAAC_PCM_CAP       (AMPAAC_MAX_FRAME * AMPAAC_MAX_CHANNELS * 2)
#define AMPAAC_OPEN_AUS      3         /* access units decoded at open to learn the output format */
#define AMPAAC_PREROLL_AUS   3         /* access units decoded and dropped ahead of a seek target */
#define AMPAAC_READ_THROUGH  262144u   /* forward jumps up to this far are read through, not seeked */
#define AMPAAC_HOP_BUDGET_MS 100u      /* ADTS: time a seek may spend walking frame headers */
#define AMPAAC_LENGTH_TAG    "AMPAAC_LENGTH_MS"   /* FMOD_TAGTYPE_USER, INT: length estimate for a stream
                                                     FMOD reports as unknown length */
#define AMPAAC_UNKNOWN       0xFFFFFFFFu

/* Reader over FMOD's codec file functions: a read-ahead window positioned anywhere in the file. */
typedef struct ampaac_reader {
    FMOD_CODEC_STATE* codec;
    unsigned int      size;       /* file size in bytes, AMPAAC_UNKNOWN when the source has none */
    unsigned int      bufStart;   /* file offset of buf[0] */
    unsigned int      bufLen;     /* valid bytes in buf */
    unsigned int      pos;        /* logical read position (file offset) */
    int               atEnd;      /* the source ended at bufStart + bufLen */
    FMOD_RESULT       fault;      /* first file error other than EOF; sticky */
    unsigned char     buf[AMPAAC_READ_BUF];
} ampaac_reader;

void         ampaac_reader_init(ampaac_reader* rd, FMOD_CODEC_STATE* codec);
/* Makes up to `want` bytes at the read position available; returns the count available (short only at the end or on a fault). */
unsigned int ampaac_reader_peek(ampaac_reader* rd, unsigned int want, const unsigned char** out);
void         ampaac_reader_skip(ampaac_reader* rd, unsigned int count);
FMOD_RESULT  ampaac_reader_seek(ampaac_reader* rd, unsigned int pos);

/* ADTS fixed + variable header (ISO/IEC 13818-7 §6.2 / ISO/IEC 14496-3 §1.A.2.2). */
typedef struct ampaac_adts_header {
    unsigned int frameLength;    /* bytes, header included */
    unsigned int headerLength;   /* 7, or 9 with CRC */
    unsigned int mpegId;
    unsigned int profile;        /* 0 Main, 1 LC, 2 SSR, 3 LTP */
    unsigned int sfIndex;
    unsigned int channelConfig;
    unsigned int rawBlocks;      /* raw data blocks in the frame (1..4) */
} ampaac_adts_header;

int  ampaac_adts_parse(const unsigned char* p, unsigned int avail, ampaac_adts_header* hdr);
int  ampaac_adts_same_stream(const ampaac_adts_header* a, const ampaac_adts_header* b);
/* Offset of the first frame in buf that starts `chain` consecutive same-stream frames, or -1.
   A chain that runs into the end of buf counts when `endIsEOF` is set. */
long ampaac_adts_find_sync(const unsigned char* buf, unsigned int len, unsigned int chain, int endIsEOF);

typedef enum ampaac_container {
    AMPAAC_ADTS = 1,
    AMPAAC_MP4  = 2
} ampaac_container;

/* Sparse ADTS seek index: (file offset, PCM position) every `stride` frames while the position is exact. */
typedef struct ampaac_index_entry {
    unsigned int offset;
    unsigned int pcm;
} ampaac_index_entry;

typedef struct ampaac_codec {
    FMOD_CODEC_WAVEFORMAT waveformat;
    ampaac_reader         reader;
    HANDLE_AACDECODER     decoder;
    ampaac_container      container;

    /* Output format, fixed at open. */
    int                   channels;
    int                   sampleRate;
    int                   frameSize;     /* PCM frames per decoded access unit */

    /* Decode position. */
    unsigned int          decodedPcm;    /* PCM position of the next decoded frame's first sample */
    unsigned int          lengthPcm;     /* current length estimate, AMPAAC_UNKNOWN if none */
    int                   lengthExact;   /* lengthPcm is the true end, not an estimate */
    unsigned int          publishedMs;   /* last AMPAAC_LENGTH_TAG value sent, 0 if none */
    unsigned int          framesSincePublish;
    unsigned int          discard;       /* PCM frames still to drop (seek pre-roll and in-frame offset) */
    unsigned int          decodeFlags;   /* flags for the next aacDecoder_DecodeFrame */
    int                   exhausted;     /* no more access units */
    int                   started;       /* a frame has been handed out since open */
    int                   exact;         /* decodedPcm is exact (continuous decode from an exact anchor) */

    /* PCM carry: one decoded access unit handed out across read calls. */
    unsigned int          pcmFrames;
    unsigned int          pcmNext;

    /* ADTS */
    unsigned int          dataStart;     /* file offset of the first frame */
    unsigned long long    feedBytes;     /* bytes of frames fed while exact, for the mean frame size */
    unsigned int          feedFrames;
    unsigned int          meanFrameBytes;
    unsigned int          pcmPerFrame;   /* output PCM frames per ADTS frame (frameSize * raw blocks) */
    ampaac_index_entry*   index;
    unsigned int          indexCap;
    unsigned int          indexLen;
    unsigned int          indexStride;
    unsigned int          framesSinceIndex;

    unsigned char         au[AMPAAC_MAX_AU];
    INT_PCM               pcm[AMPAAC_PCM_CAP];
} ampaac_codec;

/* Container hooks used by the codec core. */
FMOD_RESULT ampaac_adts_open(ampaac_codec* aac);
/* Next access unit at the read position; returns FMOD_OK, FMOD_ERR_FILE_EOF at the end, or a file error. */
FMOD_RESULT ampaac_adts_next(ampaac_codec* aac, unsigned int* auLen);
FMOD_RESULT ampaac_adts_seek(ampaac_codec* aac, unsigned int targetPcm);
void        ampaac_adts_reset_index(ampaac_codec* aac);
void        ampaac_adts_estimate_length(ampaac_codec* aac);
void        ampaac_adts_walk_to_end(ampaac_codec* aac);

/* Seek walk budget; tests lower it to exercise the estimate path. */
extern unsigned int ampaac_hop_budget_ms;

#endif
