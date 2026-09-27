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
#define AMPAAC_PREROLL_AUS   8         /* access units decoded and dropped ahead of a seek target (PS needs ~8) */
#define AMPAAC_READ_THROUGH  262144u   /* forward jumps up to this far are read through, not seeked */
#define AMPAAC_HOP_BUDGET_MS 100u      /* ADTS: time a seek may spend walking frame headers */
#define AMPAAC_RESYNC_LIMIT  1048576u  /* ADTS: bytes one resync scans before the stream counts as ended */
#define AMPAAC_TRAILER_MAX   65536u    /* ADTS: bytes before the end a trailing tag (ID3v1, APE, Lyrics3) spans */
#define AMPAAC_BOX_LIMIT     4096u     /* MP4: top-level boxes walked looking for moov */
#define AMPAAC_STARTUP_DROP  8u        /* frames at another rate a restarted decoder may drop (SBR start-up) */
#define AMPAAC_CONCEAL_LIMIT_MS 10000u /* ADTS of unknown size: consecutive concealed output that ends it */
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

/* MP4: the first AAC audio track. Per-track fields follow `objectType` (reset when a track is skipped). */
typedef struct ampaac_mp4 {
    unsigned char*       moov;            /* the whole moov box, kept: asc/sizes/sync point into it */
    unsigned int         moovSize;
    unsigned int         movieTimescale;  /* mvhd */
    unsigned int         objectType;      /* esds objectTypeIndication */
    const unsigned char* asc;             /* AudioSpecificConfig */
    unsigned int         ascSize;
    unsigned int         timescale;       /* mdhd */
    unsigned long long   mediaDuration;
    unsigned int*        offsets;         /* per access unit */
    const unsigned char* sizes;           /* stsz table (big-endian), NULL when uniform */
    unsigned int         uniform;
    unsigned int         samples;         /* access units */
    const unsigned char* sync;            /* stss table (1-based, big-endian), NULL when every AU syncs */
    unsigned int         syncCount;
    int                  usac;            /* AOT 42: decoding restarts only at a sync sample (stss) */
    int                  hasEdit;         /* elst: priming and presented duration */
    unsigned long long   editMediaTime;
    unsigned long long   editDuration;
    int                  hasSmpb;         /* iTunSMPB, when no elst */
    unsigned long long   smpbPriming;
    unsigned long long   smpbSamples;
    unsigned int         next;            /* next access unit to feed */
} ampaac_mp4;

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

    /* Output format: channels are fixed at open (pinned in fdk's mixer); the rate and frame size change
       only on a clean frame, with a "Sample Rate Change" tag. */
    int                   channels;
    int                   sampleRate;
    int                   frameSize;     /* PCM frames per decoded access unit */

    /* Decode position, in decoder output samples from the first access unit. FMOD positions are these
       minus leadPcm (decoder delay plus the encoder's priming); endPcm cuts the encoder's padding
       (AMPAAC_UNKNOWN: none). */
    unsigned int          decodedPcm;    /* PCM position of the next decoded frame's first sample */
    unsigned int          leadPcm;
    unsigned int          endPcm;
    unsigned int          lengthPcm;     /* current length estimate, AMPAAC_UNKNOWN if none */
    int                   lengthExact;   /* lengthPcm is the true end, not an estimate */
    int                   lengthFinal;   /* lengthPcm is the decoded total at the end of the stream */
    unsigned int          publishedMs;   /* last AMPAAC_LENGTH_TAG value sent, 0 if none */
    unsigned int          framesSincePublish;
    unsigned int          discard;       /* PCM frames still to drop (seek pre-roll and in-frame offset) */
    unsigned int          decodeFlags;   /* flags for the next aacDecoder_DecodeFrame */
    int                   exhausted;     /* no more access units */
    unsigned int          decoderDelay;  /* CStreamInfo.outputDelay: samples fdk's output trails the model */
    unsigned int          drainLeft;     /* delayed samples still to flush out after the last access unit */
    int                   flushed;       /* AACDEC_FLUSH ran: SBR/PS dropped to upsampling (re-prime on seek) */
    int                   started;       /* a frame has been handed out since the decoder last started clean */
    unsigned int          startupDrops;  /* frames at another rate dropped since then */
    unsigned int          concealRun;    /* consecutive concealed frames */
    int                   gaveUp;        /* a limit ended the stream, not its data: the length stays an estimate */
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

    ampaac_mp4            mp4;

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

FMOD_RESULT ampaac_mp4_open(ampaac_codec* aac);
void        ampaac_mp4_set_trim(ampaac_codec* aac);
FMOD_RESULT ampaac_mp4_next(ampaac_codec* aac, unsigned int* auLen);
FMOD_RESULT ampaac_mp4_seek(ampaac_codec* aac, unsigned int targetPcm);
void        ampaac_mp4_close(ampaac_codec* aac);

/* Seek walk budget and concealment limit; tests lower them. */
extern unsigned int ampaac_hop_budget_ms;
extern unsigned int ampaac_conceal_limit_ms;

#endif
