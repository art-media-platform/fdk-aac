/*
 * ampaac — reader over FMOD's codec file functions.
 *
 * One read-ahead window that can sit anywhere in the file. A seek inside the window moves only the read
 * position; a seek outside it repositions the source (on a netstream FMOD turns that into a Range request).
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

/* Reads from the source at bufStart + bufLen until `need` bytes are buffered past pos, or the source ends. */
static void reader_fill(ampaac_reader* rd, unsigned int need) {
    while (!rd->atEnd && rd->fault == FMOD_OK) {
        unsigned int have = rd->bufStart + rd->bufLen - rd->pos;
        if (have >= need) {
            return;
        }
        unsigned int room = AMPAAC_READ_BUF - rd->bufLen;
        unsigned int got  = 0;
        FMOD_RESULT  res  = FMOD_CODEC_FILE_READ(rd->codec, rd->buf + rd->bufLen, room, &got);

        rd->bufLen += got;
        if (res == FMOD_ERR_FILE_EOF) {
            rd->atEnd = 1;
        } else if (res != FMOD_OK) {
            rd->fault = res;
        } else if (got == 0) {
            rd->atEnd = 1;   /* a source that returns nothing without EOF would otherwise spin */
        }
    }
}

unsigned int ampaac_reader_peek(ampaac_reader* rd, unsigned int want, const unsigned char** out) {
    unsigned int offset;
    unsigned int have;

    if (want > AMPAAC_READ_BUF) {
        want = AMPAAC_READ_BUF;
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
    unsigned int end = rd->bufStart + rd->bufLen;

    if (rd->pos + count <= end) {
        rd->pos += count;
        return;
    }
    ampaac_reader_seek(rd, rd->pos + count);
}

FMOD_RESULT ampaac_reader_seek(ampaac_reader* rd, unsigned int pos) {
    FMOD_RESULT res;

    if (pos >= rd->bufStart && pos <= rd->bufStart + rd->bufLen) {
        rd->pos = pos;
        return FMOD_OK;
    }
    if (rd->size != AMPAAC_UNKNOWN && pos > rd->size) {
        pos = rd->size;
    }

    res = FMOD_CODEC_FILE_SEEK(rd->codec, pos, FMOD_CODEC_SEEK_METHOD_SET);
    rd->bufStart = pos;
    rd->bufLen   = 0;
    rd->pos      = pos;
    rd->atEnd    = 0;
    if (res != FMOD_OK) {
        rd->fault = res;
        return res;
    }
    rd->fault = FMOD_OK;
    return FMOD_OK;
}
