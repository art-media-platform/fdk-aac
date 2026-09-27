/*
 * ampaac — reader over FMOD's codec file functions.
 *
 * One read-ahead window. A netstream turns every FMOD file seek into a reconnect (a new Range request), so
 * the reader seeks the source only when it must: positions inside the window move the read position, and
 * short forward jumps past it are read through and discarded.
 */
#include <string.h>

#include "ampaac.h"

void ampaac_reader_init(ampaac_reader* rd, FMOD_CODEC_STATE* codec) {
    unsigned int size = 0;

    rd->codec    = codec;
    rd->bufStart = 0;
    rd->bufLen   = 0;
    rd->pos      = 0;
    rd->atEnd    = 0;
    rd->fault    = FMOD_OK;

    if (FMOD_CODEC_FILE_SIZE(codec, &size) != FMOD_OK || size == 0) {
        size = AMPAAC_UNKNOWN;
    }
    rd->size = size;
}

static void source_read(ampaac_reader* rd, unsigned char* into, unsigned int want, unsigned int* got) {
    FMOD_RESULT res = FMOD_CODEC_FILE_READ(rd->codec, into, want, got);

    if (res == FMOD_ERR_FILE_EOF) {
        rd->atEnd = 1;
    } else if (res != FMOD_OK) {
        rd->fault = res;
    } else if (*got == 0) {
        rd->atEnd = 1;   /* a source that returns nothing without EOF would otherwise spin */
    }
}

/* Consumes the bytes between the window's end and a read position set past it. */
static void read_through_gap(ampaac_reader* rd) {
    unsigned int end = rd->bufStart + rd->bufLen;

    while (end < rd->pos && !rd->atEnd && rd->fault == FMOD_OK) {
        unsigned int gap = rd->pos - end;
        unsigned int got = 0;
        source_read(rd, rd->buf, gap < AMPAAC_READ_BUF ? gap : AMPAAC_READ_BUF, &got);
        end += got;
    }
    rd->bufStart = end < rd->pos ? end : rd->pos;
    rd->bufLen   = 0;
    rd->pos      = rd->bufStart;
}

/* Reads until `need` bytes are buffered past pos, or the source ends. Requests go out in 4 KB granules
   rather than a whole window: a rejected probe that read past FMOD's buffer costs a reconnect. */
static void reader_fill(ampaac_reader* rd, unsigned int need) {
    while (!rd->atEnd && rd->fault == FMOD_OK) {
        unsigned int have = rd->bufStart + rd->bufLen - rd->pos;
        unsigned int room = AMPAAC_READ_BUF - rd->bufLen;
        unsigned int want;
        unsigned int got = 0;

        if (have >= need) {
            return;
        }
        want = (need - have + AMPAAC_READ_GRANULE - 1) / AMPAAC_READ_GRANULE * AMPAAC_READ_GRANULE;
        source_read(rd, rd->buf + rd->bufLen, want < room ? want : room, &got);
        rd->bufLen += got;
    }
}

unsigned int ampaac_reader_peek(ampaac_reader* rd, unsigned int want, const unsigned char** out) {
    unsigned int offset;
    unsigned int have;

    if (want > AMPAAC_READ_BUF) {
        want = AMPAAC_READ_BUF;
    }
    if (rd->pos > rd->bufStart + rd->bufLen) {
        read_through_gap(rd);
    }

    /* Keep the unread tail and slide it to the front when the window cannot hold `want` more bytes. */
    offset = rd->pos - rd->bufStart;
    if (offset + want > AMPAAC_READ_BUF) {
        memmove(rd->buf, rd->buf + offset, rd->bufLen - offset);
        rd->bufLen  -= offset;
        rd->bufStart = rd->pos;
        offset       = 0;
    }

    reader_fill(rd, want);

    have = rd->bufStart + rd->bufLen - rd->pos;
    *out = rd->buf + offset;
    return have < want ? have : want;
}

void ampaac_reader_skip(ampaac_reader* rd, unsigned int count) {
    ampaac_reader_seek(rd, rd->pos + count);
}

FMOD_RESULT ampaac_reader_seek(ampaac_reader* rd, unsigned int pos) {
    unsigned int end = rd->bufStart + rd->bufLen;
    FMOD_RESULT  res;

    if (pos >= rd->bufStart && pos <= end) {
        rd->pos = pos;
        return FMOD_OK;
    }
    if (rd->size != AMPAAC_UNKNOWN && pos > rd->size) {
        pos = rd->size;
    }
    if (pos > end && pos - end <= AMPAAC_READ_THROUGH && !rd->atEnd && rd->fault == FMOD_OK) {
        rd->pos = pos;   /* read through on the next peek */
        return FMOD_OK;
    }

    res = FMOD_CODEC_FILE_SEEK(rd->codec, pos, FMOD_CODEC_SEEK_METHOD_SET);
    rd->bufStart = pos;
    rd->bufLen   = 0;
    rd->pos      = pos;
    rd->atEnd    = 0;
    rd->fault    = res;
    return res;
}
