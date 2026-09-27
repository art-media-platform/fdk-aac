/*
 * ampaac — ADTS container: probe, frame walk, resync, seek index and length estimate.
 *
 * ADTS carries no index, and FMOD ends a stream at the length declared at open (playing on past the data
 * or cutting the tail), so ADTS declares its length unknown and FMOD ends at the decoder's EOF. The seek index maps
 * file offsets to PCM exactly: frames are fed one at a time while decoding, and a seek walks frame headers
 * forward (no decode) within a time budget. Past what is walked, positions are estimated.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L   /* clock_gettime under strict C11 */
#endif
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#include "ampaac.h"

#define ADTS_MIN_HEADER   7u
#define ADTS_CHAIN        3u      /* consecutive frames that establish sync */
#define INDEX_CAP         4096u   /* anchors; halved (stride doubled) when full */
#define INDEX_STRIDE      8u      /* initial frames between anchors */

unsigned int ampaac_hop_budget_ms = AMPAAC_HOP_BUDGET_MS;

static double clock_ms(void) {
#if defined(_WIN32)
    return (double)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
#endif
}

int ampaac_adts_parse(const unsigned char* p, unsigned int avail, ampaac_adts_header* hdr) {
    unsigned int protectionAbsent;

    if (avail < ADTS_MIN_HEADER) {
        return 0;
    }
    /* syncword 0xFFF, layer 00 */
    if (p[0] != 0xFF || (p[1] & 0xF6) != 0xF0) {
        return 0;
    }
    protectionAbsent    = p[1] & 1;
    hdr->mpegId         = (p[1] >> 3) & 1;
    hdr->profile        = (p[2] >> 6) & 3;
    hdr->sfIndex        = (p[2] >> 2) & 0xF;
    hdr->channelConfig  = ((p[2] & 1) << 2) | (p[3] >> 6);
    hdr->frameLength    = ((unsigned int)(p[3] & 3) << 11) | ((unsigned int)p[4] << 3) | (p[5] >> 5);
    hdr->rawBlocks      = (p[6] & 3) + 1;
    hdr->headerLength   = protectionAbsent ? 7 : 9;

    if (hdr->sfIndex > 12) {
        return 0;
    }
    if (hdr->frameLength <= hdr->headerLength) {
        return 0;
    }
    return 1;
}

int ampaac_adts_same_stream(const ampaac_adts_header* a, const ampaac_adts_header* b) {
    return a->mpegId == b->mpegId
        && a->profile == b->profile
        && a->sfIndex == b->sfIndex
        && a->channelConfig == b->channelConfig;
}

long ampaac_adts_find_sync(const unsigned char* buf, unsigned int len, unsigned int chain, int endIsEOF) {
    unsigned int off;

    for (off = 0; off + ADTS_MIN_HEADER <= len; off++) {
        ampaac_adts_header first;
        ampaac_adts_header next;
        unsigned int       at;
        unsigned int       count = 1;

        if (buf[off] != 0xFF || !ampaac_adts_parse(buf + off, len - off, &first)) {
            continue;
        }
        at = off + first.frameLength;
        while (count < chain && at + ADTS_MIN_HEADER <= len) {
            if (!ampaac_adts_parse(buf + at, len - at, &next) || !ampaac_adts_same_stream(&first, &next)) {
                break;
            }
            count++;
            at += next.frameLength;
        }
        if (count >= chain) {
            return (long)off;
        }
        /* A stream shorter than the chain: every frame parsed and the last one ends exactly at EOF. */
        if (endIsEOF && at == len) {
            return (long)off;
        }
    }
    return -1;
}

/* Adds an exact anchor past the last one; when full, keeps every other anchor and doubles the stride. */
static void index_append(ampaac_codec* aac, unsigned int offset, unsigned int pcm) {
    if (aac->indexLen > 0 && aac->index[aac->indexLen - 1].offset >= offset) {
        return;
    }
    if (aac->indexLen == aac->indexCap) {
        unsigned int keep;
        for (keep = 0; keep * 2 < aac->indexLen; keep++) {
            aac->index[keep] = aac->index[keep * 2];
        }
        aac->indexLen     = keep;
        aac->indexStride *= 2;
    }
    aac->index[aac->indexLen].offset = offset;
    aac->index[aac->indexLen].pcm    = pcm;
    aac->indexLen++;
}

static const ampaac_index_entry* last_anchor(const ampaac_codec* aac) {
    return &aac->index[aac->indexLen - 1];
}

void ampaac_adts_reset_index(ampaac_codec* aac) {
    /* The first frame is an exact anchor before anything decodes: FMOD seeks to 0 right after open. */
    aac->index[0].offset  = aac->dataStart;
    aac->index[0].pcm     = 0;
    aac->indexLen         = 1;
    aac->indexStride      = INDEX_STRIDE;
    aac->framesSinceIndex = 1;
}

/* Scans forward for a chained sync and moves the reader onto it. `from` is 1 when the frame at the read
   position already failed, 0 when the read position itself may start a chain (a seek landing). Past
   AMPAAC_RESYNC_LIMIT bytes without a chain the stream counts as ended, and the codec as having given up:
   one read call must not scan an unbounded transfer. */
static FMOD_RESULT resync(ampaac_codec* aac, unsigned int from) {
    ampaac_reader* rd = &aac->reader;
    unsigned int   scanned = 0;

    for (;;) {
        const unsigned char* window;
        unsigned int         avail = ampaac_reader_peek(rd, AMPAAC_READ_BUF, &window);
        int                  endIsEOF = rd->atEnd && rd->fault == FMOD_OK;
        long                 found;

        if (avail < ADTS_MIN_HEADER + from) {
            return rd->fault != FMOD_OK ? rd->fault : FMOD_ERR_FILE_EOF;
        }
        found = ampaac_adts_find_sync(window + from, avail - from, ADTS_CHAIN, endIsEOF);
        if (found >= 0) {
            ampaac_reader_skip(rd, (unsigned int)found + from);
            return FMOD_OK;
        }
        from = 1;
        if (endIsEOF || rd->fault != FMOD_OK) {
            ampaac_reader_skip(rd, avail);
            return rd->fault != FMOD_OK ? rd->fault : FMOD_ERR_FILE_EOF;
        }
        /* A chain starting in the second half may have run out of window; keep that half. */
        ampaac_reader_skip(rd, avail / 2);
        scanned += avail / 2;
        if (scanned >= AMPAAC_RESYNC_LIMIT) {
            aac->gaveUp = 1;
            return FMOD_ERR_FILE_EOF;
        }
    }
}

FMOD_RESULT ampaac_adts_open(ampaac_codec* aac) {
    ampaac_reader*       rd = &aac->reader;
    const unsigned char* head;
    unsigned int         avail = ampaac_reader_peek(rd, AMPAAC_PROBE_BYTES, &head);
    int                  endIsEOF = rd->atEnd && rd->fault == FMOD_OK;
    long                 found = ampaac_adts_find_sync(head, avail, ADTS_CHAIN, endIsEOF);
    unsigned int         at;
    unsigned int         bytes = 0;
    unsigned int         frames = 0;
    ampaac_adts_header   hdr;

    if (found < 0) {
        return FMOD_ERR_FORMAT;   /* no chain: not established as ADTS, even when the read failed */
    }

    /* Mean frame size over the head window seeds the length estimate until the index outgrows it. */
    at = (unsigned int)found;
    while (at + ADTS_MIN_HEADER <= avail && ampaac_adts_parse(head + at, avail - at, &hdr)
           && at + hdr.frameLength <= avail) {
        bytes += hdr.frameLength;
        frames++;
        at += hdr.frameLength;
    }
    aac->container      = AMPAAC_ADTS;
    aac->dataStart      = rd->pos + (unsigned int)found;
    aac->meanFrameBytes = frames ? bytes / frames : 0;
    aac->indexCap       = INDEX_CAP;
    aac->index          = (ampaac_index_entry*)FMOD_CODEC_ALLOC(rd->codec, INDEX_CAP * sizeof(ampaac_index_entry), 16);
    if (!aac->index) {
        return FMOD_ERR_MEMORY;
    }
    ampaac_adts_reset_index(aac);
    return ampaac_reader_seek(rd, aac->dataStart);
}

FMOD_RESULT ampaac_adts_next(ampaac_codec* aac, unsigned int* auLen) {
    ampaac_reader* rd = &aac->reader;

    for (;;) {
        const unsigned char* p;
        unsigned int         avail = ampaac_reader_peek(rd, ADTS_MIN_HEADER + 2, &p);
        ampaac_adts_header   hdr;
        ampaac_adts_header   next;
        FMOD_RESULT          res;

        if (avail < ADTS_MIN_HEADER) {
            return rd->fault != FMOD_OK ? rd->fault : FMOD_ERR_FILE_EOF;
        }
        if (ampaac_adts_parse(p, avail, &hdr)) {
            unsigned int offset = rd->pos;

            avail = ampaac_reader_peek(rd, hdr.frameLength + ADTS_MIN_HEADER, &p);
            if (avail >= hdr.frameLength) {
                int nextOK   = avail >= hdr.frameLength + ADTS_MIN_HEADER
                            && ampaac_adts_parse(p + hdr.frameLength, avail - hdr.frameLength, &next);
                int endsHere = avail < hdr.frameLength + ADTS_MIN_HEADER && rd->atEnd;

                /* In sync, a frame is taken on its own header (trailing tags do not cost the last frame);
                   after a resync, the next header has to confirm it. */
                if (aac->exact || nextOK || endsHere) {
                    memcpy(aac->au, p, hdr.frameLength);
                    *auLen = hdr.frameLength;
                    aac->pcmPerFrame = (unsigned int)aac->frameSize * hdr.rawBlocks;
                    if (aac->exact) {
                        if (aac->framesSinceIndex == 0) {
                            index_append(aac, offset, aac->decodedPcm);
                        }
                        aac->framesSinceIndex = (aac->framesSinceIndex + 1) % aac->indexStride;
                    }
                    ampaac_reader_skip(rd, hdr.frameLength);
                    return FMOD_OK;
                }
            } else if (rd->atEnd || rd->fault != FMOD_OK) {
                /* Truncated final frame. */
                ampaac_reader_skip(rd, avail);
                return rd->fault != FMOD_OK ? rd->fault : FMOD_ERR_FILE_EOF;
            }
        }

        /* Lost sync: bytes are skipped, so positions past this point are estimates, unless no frame
           follows before the data ends: then the bytes were a trailer (ID3v1, APE, Lyrics3), not lost audio. */
        {
            int wasExact = aac->exact;

            aac->exact = 0;
            res = resync(aac, 1);
            if (res == FMOD_ERR_FILE_EOF && !aac->gaveUp) {
                aac->exact = wasExact;
            }
            if (res != FMOD_OK) {
                return res;
            }
        }
    }
}

/*
 * Walks frame headers from the last anchor toward `target` without decoding, adding anchors. Stops at the
 * frame that holds `target`, at the end of the stream, where the chain breaks, or when `budgetMs` runs out
 * (a still-pulling blob reads slowly; a far seek then estimates rather than waiting on the pull).
 */
static void hop_toward(ampaac_codec* aac, unsigned int target, unsigned int budgetMs) {
    ampaac_reader*     rd = &aac->reader;
    ampaac_index_entry at = *last_anchor(aac);
    double             deadline = clock_ms() + budgetMs;
    unsigned int       frames = 0;

    if (ampaac_reader_seek(rd, at.offset) != FMOD_OK) {
        return;
    }
    for (;;) {
        const unsigned char* p;
        unsigned int         avail = ampaac_reader_peek(rd, ADTS_MIN_HEADER, &p);
        ampaac_adts_header   hdr;
        unsigned int         framePcm;

        if (avail < ADTS_MIN_HEADER) {
            if (rd->atEnd && rd->fault == FMOD_OK && rd->pos == at.offset) {
                /* Walked to the end: the length is exact. */
                aac->lengthPcm   = at.pcm;
                aac->lengthExact = 1;
            }
            break;
        }
        if (!ampaac_adts_parse(p, avail, &hdr)) {
            /* Near the end, no frame following the last one walked means a trailing tag: that frame ends
               the stream. Farther in, the walk stops without scanning: a seek must not search a megabyte. */
            if (rd->size != AMPAAC_UNKNOWN && rd->size - rd->pos <= AMPAAC_TRAILER_MAX
                && resync(aac, 1) == FMOD_ERR_FILE_EOF && aac->reader.fault == FMOD_OK) {
                aac->lengthPcm   = at.pcm;
                aac->lengthExact = 1;
            }
            break;
        }
        framePcm = (unsigned int)aac->frameSize * hdr.rawBlocks;
        if (at.pcm + framePcm > target) {
            break;
        }
        if (ampaac_reader_peek(rd, hdr.frameLength, &p) < hdr.frameLength) {
            break;   /* truncated final frame */
        }
        ampaac_reader_skip(rd, hdr.frameLength);
        at.offset += hdr.frameLength;
        at.pcm    += framePcm;
        if (++frames % aac->indexStride == 0) {
            index_append(aac, at.offset, at.pcm);
        }
        if ((frames & 15) == 0 && clock_ms() > deadline) {
            break;
        }
    }
    index_append(aac, at.offset, at.pcm);
}

void ampaac_adts_walk_to_end(ampaac_codec* aac) {
    hop_toward(aac, AMPAAC_UNKNOWN, 0xFFFFFFFFu);
}

/* Mean frame size over the exactly walked region once it holds enough frames, else the head window's. */
static unsigned int mean_frame_bytes(const ampaac_codec* aac) {
    const ampaac_index_entry* last = last_anchor(aac);
    unsigned int              frames = aac->pcmPerFrame ? last->pcm / aac->pcmPerFrame : 0;

    if (frames >= 64) {
        return (last->offset - aac->dataStart + frames / 2) / frames;
    }
    return aac->meanFrameBytes;
}

void ampaac_adts_estimate_length(ampaac_codec* aac) {
    unsigned int              size = aac->reader.size;
    unsigned int              mean;
    const ampaac_index_entry* last;
    unsigned long long        pcm;

    if (aac->lengthExact || aac->lengthFinal || aac->indexLen == 0) {
        return;
    }
    mean = mean_frame_bytes(aac);
    last = last_anchor(aac);
    if (size == AMPAAC_UNKNOWN || mean == 0 || aac->pcmPerFrame == 0 || size <= last->offset) {
        aac->lengthPcm = size == AMPAAC_UNKNOWN ? AMPAAC_UNKNOWN : last->pcm;
        return;
    }
    pcm = last->pcm + ((unsigned long long)(size - last->offset) + mean / 2) / mean * aac->pcmPerFrame;
    if (pcm + aac->leadPcm < aac->decodedPcm) {
        pcm = aac->decodedPcm - aac->leadPcm;
    }
    aac->lengthPcm = pcm >= AMPAAC_UNKNOWN ? AMPAAC_UNKNOWN - 1 : (unsigned int)pcm;
}

FMOD_RESULT ampaac_adts_seek(ampaac_codec* aac, unsigned int targetPcm) {
    ampaac_reader*            rd = &aac->reader;
    unsigned int              preroll = AMPAAC_PREROLL_AUS * aac->pcmPerFrame;
    unsigned int              start = targetPcm > preroll ? targetPcm - preroll : 0;
    const ampaac_index_entry* last;
    FMOD_RESULT               res;

    last = last_anchor(aac);
    if (start >= last->pcm + aac->pcmPerFrame
        && (unsigned long long)(start - last->pcm) / aac->pcmPerFrame * mean_frame_bytes(aac) <= AMPAAC_READ_THROUGH) {
        /* The walk covers only bytes a netstream likely holds already (FMOD's ring is 256 KiB at AMP's stream
           buffer): one of its reads waits for bytes still arriving, which the time budget cannot cut short.
           A farther target is estimated at once, so its Range is the first request the server sees. */
        hop_toward(aac, start, ampaac_hop_budget_ms);
    }
    last = last_anchor(aac);

    if (start < last->pcm + aac->pcmPerFrame) {
        /* Exact: the last anchor at or before start. */
        unsigned int lo = 0;
        unsigned int hi = aac->indexLen - 1;
        while (lo < hi) {
            unsigned int mid = (lo + hi + 1) / 2;
            if (aac->index[mid].pcm <= start) {
                lo = mid;
            } else {
                hi = mid - 1;
            }
        }
        res = ampaac_reader_seek(rd, aac->index[lo].offset);
        if (res != FMOD_OK) {
            return res;
        }
        aac->decodedPcm       = aac->index[lo].pcm;
        aac->exact            = 1;
        aac->framesSinceIndex = 1;   /* the anchor frame is indexed already */
        return FMOD_OK;
    }

    /* Past the walk (its budget, or a target beyond its reach): land by the mean frame size from the furthest
       exact anchor, then resync. */
    {
        unsigned int       mean = mean_frame_bytes(aac);
        unsigned long long frames = (start - last->pcm) / aac->pcmPerFrame;
        unsigned long long offset = last->offset + frames * mean;
        unsigned long long landed;

        if (rd->size != AMPAAC_UNKNOWN && offset >= rd->size) {
            offset = rd->size;
        }
        res = ampaac_reader_seek(rd, (unsigned int)offset);
        if (res != FMOD_OK) {
            return res;
        }
        res = resync(aac, 0);
        if (res != FMOD_OK && res != FMOD_ERR_FILE_EOF) {
            return res;
        }
        landed = mean ? last->pcm + ((unsigned long long)(rd->pos - last->offset) + mean / 2) / mean * aac->pcmPerFrame
                      : last->pcm;
        aac->decodedPcm = landed > targetPcm ? targetPcm : (unsigned int)landed;
        aac->exact      = 0;
    }
    return FMOD_OK;
}
