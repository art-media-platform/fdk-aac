/*
 * ampaac — ADTS container: probe, frame walk, resync, sparse seek index and length estimate.
 *
 * Frames are fed to the decoder one at a time, so each frame's file offset maps exactly to the PCM
 * position of its first output sample while decoding runs continuously from an exact anchor.
 */
#include <string.h>

#include "ampaac.h"

#define ADTS_MIN_HEADER   7u
#define ADTS_CHAIN        3u      /* consecutive frames that establish sync */
#define INDEX_CAP         4096u   /* entries; halved (stride doubled) when full */
#define INDEX_STRIDE      8u      /* initial frames between entries */

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

static void index_add(ampaac_codec* aac, unsigned int offset) {
    if (aac->framesSinceIndex != 0) {
        aac->framesSinceIndex = (aac->framesSinceIndex + 1) % aac->indexStride;
        return;
    }
    aac->framesSinceIndex = 1 % aac->indexStride;

    if (aac->indexLen > 0 && aac->index[aac->indexLen - 1].offset >= offset) {
        return;   /* re-decoding an indexed region after a seek back */
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
    aac->index[aac->indexLen].pcm    = aac->decodedPcm;
    aac->indexLen++;
}

/* Scans forward from the read position for a chained sync and moves the reader onto it. */
static FMOD_RESULT resync(ampaac_codec* aac) {
    ampaac_reader* rd = &aac->reader;

    for (;;) {
        const unsigned char* window;
        unsigned int         avail = ampaac_reader_peek(rd, AMPAAC_READ_BUF, &window);
        int                  endIsEOF = rd->atEnd && rd->fault == FMOD_OK;
        long                 found;

        if (avail < ADTS_MIN_HEADER) {
            return rd->fault != FMOD_OK ? rd->fault : FMOD_ERR_FILE_EOF;
        }
        /* Start at 1: the frame at the read position already failed. */
        found = ampaac_adts_find_sync(window + 1, avail - 1, ADTS_CHAIN, endIsEOF);
        if (found >= 0) {
            ampaac_reader_skip(rd, (unsigned int)found + 1);
            return FMOD_OK;
        }
        if (endIsEOF || rd->fault != FMOD_OK) {
            ampaac_reader_skip(rd, avail);
            return rd->fault != FMOD_OK ? rd->fault : FMOD_ERR_FILE_EOF;
        }
        /* A chain starting in the second half may have run out of window; keep that half. */
        ampaac_reader_skip(rd, avail / 2);
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
        /* Too few bytes to decide is a load failure, not a verdict on the format. */
        if (rd->fault != FMOD_OK) {
            return rd->fault;
        }
        return FMOD_ERR_FORMAT;
    }

    /* Mean frame size over every whole frame in the head window seeds the length estimate. */
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
    aac->indexStride    = INDEX_STRIDE;
    aac->index          = (ampaac_index_entry*)FMOD_CODEC_ALLOC(rd->codec, INDEX_CAP * sizeof(ampaac_index_entry), 16);
    if (!aac->index) {
        return FMOD_ERR_MEMORY;
    }
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
                    if (aac->exact) {
                        index_add(aac, offset);
                    }
                    aac->feedBytes  += hdr.frameLength;
                    aac->feedFrames += 1;
                    aac->pcmPerFrame = aac->frameSize > 0 ? (unsigned int)aac->frameSize * hdr.rawBlocks : 0;
                    ampaac_reader_skip(rd, hdr.frameLength);
                    return FMOD_OK;
                }
            } else if (rd->atEnd || rd->fault != FMOD_OK) {
                /* Truncated final frame. */
                ampaac_reader_skip(rd, avail);
                return rd->fault != FMOD_OK ? rd->fault : FMOD_ERR_FILE_EOF;
            }
        }

        /* Lost sync: bytes are skipped, so positions past this point are estimates. */
        aac->exact = 0;
        res = resync(aac);
        if (res != FMOD_OK) {
            return res;
        }
    }
}

void ampaac_adts_update_length(ampaac_codec* aac) {
    unsigned int size = aac->reader.size;
    unsigned int mean;
    unsigned long long frames;
    unsigned long long pcm;

    if (aac->lengthExact) {
        return;
    }
    if (aac->feedFrames >= 32) {
        aac->meanFrameBytes = (unsigned int)(aac->feedBytes / aac->feedFrames);
    }
    mean = aac->meanFrameBytes;
    if (size == AMPAAC_UNKNOWN || mean == 0 || aac->pcmPerFrame == 0 || size <= aac->dataStart) {
        aac->lengthPcm = AMPAAC_UNKNOWN;
        return;
    }
    frames = ((unsigned long long)(size - aac->dataStart) + mean / 2) / mean;
    pcm    = frames * aac->pcmPerFrame;
    if (pcm < aac->decodedPcm) {
        pcm = aac->decodedPcm;
    }
    aac->lengthPcm = pcm >= AMPAAC_UNKNOWN ? AMPAAC_UNKNOWN - 1 : (unsigned int)pcm;
}

FMOD_RESULT ampaac_adts_seek(ampaac_codec* aac, unsigned int targetPcm) {
    ampaac_reader* rd = &aac->reader;
    unsigned int   preroll = AMPAAC_PREROLL_AUS * aac->pcmPerFrame;
    unsigned int   start = targetPcm > preroll ? targetPcm - preroll : 0;
    unsigned int   lastAnchor = aac->indexLen ? aac->index[aac->indexLen - 1].pcm : 0;
    unsigned int   decodeAhead = (unsigned int)aac->sampleRate * AMPAAC_EXACT_AHEAD_SECONDS;
    FMOD_RESULT    res;

    /* An anchor before start is exact by construction; decoding forward from it stays exact, so a target
       up to decodeAhead past the last anchor is reached exactly rather than estimated. */
    if (aac->indexLen > 0 && start <= lastAnchor + decodeAhead) {
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
        aac->framesSinceIndex = 1 % aac->indexStride;   /* the anchor frame is already indexed */
    } else {
        /* Past the exact region: land by the mean frame size, then resync. */
        unsigned long long frames = aac->pcmPerFrame ? start / aac->pcmPerFrame : 0;
        unsigned long long offset = aac->dataStart + frames * aac->meanFrameBytes;
        unsigned long long landed;

        if (aac->reader.size != AMPAAC_UNKNOWN && offset >= aac->reader.size) {
            offset = aac->reader.size;
        }
        res = ampaac_reader_seek(rd, (unsigned int)offset);
        if (res != FMOD_OK) {
            return res;
        }
        res = resync(aac);
        if (res != FMOD_OK && res != FMOD_ERR_FILE_EOF) {
            return res;
        }
        landed = aac->meanFrameBytes && rd->pos > aac->dataStart
               ? ((unsigned long long)(rd->pos - aac->dataStart) + aac->meanFrameBytes / 2) / aac->meanFrameBytes * aac->pcmPerFrame
               : 0;
        aac->decodedPcm = landed > targetPcm ? targetPcm : (unsigned int)landed;
        aac->exact      = 0;
    }
    return FMOD_OK;
}
