/*
 * ampaac — MP4/M4A container (ISO/IEC 14496-12 + 14496-14): the first AAC audio track of a non-fragmented
 * file. `moov` is read whole (it may follow `mdat`; reaching it then costs one Range request) and parsed
 * in memory with every length checked against its parent. Positions are exact: each access unit's offset
 * comes from the sample tables, and the encoder's priming and padding (elst, else iTunSMPB) are trimmed.
 */
#include <stddef.h>
#include <string.h>

#include "ampaac.h"

#define FOURCC(a, b, c, d) (((unsigned int)(a) << 24) | ((unsigned int)(b) << 16) | ((unsigned int)(c) << 8) | (unsigned int)(d))
#define MOOV_CAP     (32u << 20)   /* larger moov boxes are refused */
#define SAMPLE_CAP   (1u << 23)    /* access units per track (~53 h of 44.1 kHz AAC) */
#define ASC_CAP      512u

typedef struct box {
    const unsigned char* data;   /* payload */
    unsigned int         size;
    unsigned int         type;
} box;

static unsigned int be16(const unsigned char* p) { return ((unsigned int)p[0] << 8) | p[1]; }
static unsigned int be32(const unsigned char* p) { return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3]; }
static unsigned long long be64(const unsigned char* p) { return ((unsigned long long)be32(p) << 32) | be32(p + 4); }

/* Next child box in [*cursor, end); 0 at the end or on a malformed header. */
static int next_box(const unsigned char** cursor, const unsigned char* end, box* out) {
    const unsigned char* p = *cursor;
    unsigned long long   size;
    unsigned int         header = 8;

    if (end - p < 8) {
        return 0;
    }
    size = be32(p);
    if (size == 1) {
        if (end - p < 16) {
            return 0;
        }
        size   = be64(p + 8);
        header = 16;
    } else if (size == 0) {
        size = (unsigned long long)(end - p);
    }
    if (size < header || size > (unsigned long long)(end - p)) {
        return 0;
    }
    out->data = p + header;
    out->size = (unsigned int)(size - header);
    out->type = be32(p + 4);
    *cursor   = p + size;
    return 1;
}

static int find_box(const unsigned char* data, unsigned int size, unsigned int type, box* out) {
    const unsigned char* cursor = data;
    const unsigned char* end = data + size;

    while (next_box(&cursor, end, out)) {
        if (out->type == type) {
            return 1;
        }
    }
    return 0;
}

/* MPEG-4 descriptor header (ISO/IEC 14496-1 §8.3.3): tag, then a 1-4 byte size. */
static int read_descriptor(const unsigned char** p, const unsigned char* end, unsigned int* tag, unsigned int* len) {
    unsigned int i;

    if (*p >= end) {
        return 0;
    }
    *tag = *(*p)++;
    *len = 0;
    for (i = 0; i < 4; i++) {
        unsigned int b;
        if (*p >= end) {
            return 0;
        }
        b    = *(*p)++;
        *len = (*len << 7) | (b & 0x7F);
        if (!(b & 0x80)) {
            break;
        }
    }
    return *len <= (unsigned int)(end - *p);
}

/* esds → objectTypeIndication + AudioSpecificConfig. */
static int parse_esds(const box* esds, ampaac_mp4* mp4) {
    const unsigned char* p = esds->data + 4;   /* FullBox version + flags */
    const unsigned char* end = esds->data + esds->size;
    unsigned int         tag;
    unsigned int         len;

    if (esds->size < 4 || !read_descriptor(&p, end, &tag, &len)) {
        return 0;
    }
    if (tag == 0x03) {   /* ES_Descriptor */
        unsigned int flags;
        end = p + len;
        if (end - p < 3) {
            return 0;
        }
        flags = p[2];
        p += 3;
        if (flags & 0x80) {                   /* dependsOn_ES_ID */
            if (end - p < 2) {
                return 0;
            }
            p += 2;
        }
        if (flags & 0x40) {                   /* URL */
            if (end - p < 1 || end - p < 1 + (long)*p) {
                return 0;
            }
            p += 1 + *p;
        }
        if (flags & 0x20) {                   /* OCR_ES_Id */
            if (end - p < 2) {
                return 0;
            }
            p += 2;
        }
        if (!read_descriptor(&p, end, &tag, &len)) {
            return 0;
        }
    }
    if (tag != 0x04 || len < 13) {   /* DecoderConfigDescriptor */
        return 0;
    }
    mp4->objectType = p[0];
    end = p + len;
    p += 13;
    if (!read_descriptor(&p, end, &tag, &len) || tag != 0x05 || len == 0 || len > ASC_CAP) {   /* DecoderSpecificInfo */
        return 0;
    }
    mp4->asc     = p;
    mp4->ascSize = len;
    return 1;
}

/* mp4a sample entry → esds (directly, or inside a QuickTime 'wave' box). */
static int parse_sample_entry(const box* stsd, ampaac_mp4* mp4) {
    const unsigned char* cursor;
    box                  entry;
    box                  esds;
    box                  wave;
    unsigned int         version;
    unsigned int         skip = 28;

    if (stsd->size < 8 || be32(stsd->data + 4) == 0) {
        return 0;
    }
    cursor = stsd->data + 8;
    if (!next_box(&cursor, stsd->data + stsd->size, &entry) || entry.type != FOURCC('m', 'p', '4', 'a')) {
        return 0;   /* enca (encrypted), alac, … are not ours */
    }
    if (entry.size < 28) {
        return 0;
    }
    version = be16(entry.data + 8);
    if (version == 1) {
        skip += 16;
    } else if (version == 2) {
        skip += 36;
    }
    if (entry.size < skip) {
        return 0;
    }
    if (find_box(entry.data + skip, entry.size - skip, FOURCC('e', 's', 'd', 's'), &esds)) {
        return parse_esds(&esds, mp4);
    }
    if (find_box(entry.data + skip, entry.size - skip, FOURCC('w', 'a', 'v', 'e'), &wave)
        && find_box(wave.data, wave.size, FOURCC('e', 's', 'd', 's'), &esds)) {
        return parse_esds(&esds, mp4);
    }
    return 0;
}

static unsigned int media_timescale(const box* mdhd, unsigned long long* duration) {
    if (mdhd->size >= 32 && mdhd->data[0] == 1) {
        *duration = be64(mdhd->data + 24);
        return be32(mdhd->data + 20);
    }
    if (mdhd->size >= 20) {
        *duration = be32(mdhd->data + 16);
        return be32(mdhd->data + 12);
    }
    return 0;
}

/* elst: the first non-empty edit gives priming (media time) and the presented duration (movie time). */
static void parse_elst(const box* elst, ampaac_mp4* mp4) {
    unsigned int version;
    unsigned int count;
    unsigned int entry;
    unsigned int stride;

    if (elst->size < 8) {
        return;
    }
    version = elst->data[0];
    count   = be32(elst->data + 4);
    stride  = version == 1 ? 20 : 12;
    for (entry = 0; entry < count && 8 + (entry + 1) * stride <= elst->size; entry++) {
        const unsigned char* e = elst->data + 8 + entry * stride;
        long long            mediaTime = version == 1 ? (long long)be64(e + 8) : (long long)(int)be32(e + 4);
        unsigned long long   duration  = version == 1 ? be64(e) : be32(e);
        if (mediaTime >= 0) {
            mp4->editMediaTime = (unsigned long long)mediaTime;
            mp4->editDuration  = duration;
            mp4->hasEdit       = 1;
            return;
        }
    }
}

/* iTunSMPB (moov/udta/meta/ilst/'----'): " 00000000 PRIMING PADDING SAMPLES …" in hex, media timescale. */
static void parse_itunsmpb(const unsigned char* data, unsigned int size, ampaac_mp4* mp4) {
    box          udta;
    box          meta;
    box          ilst;
    box          item;
    const unsigned char* cursor;

    if (!find_box(data, size, FOURCC('u', 'd', 't', 'a'), &udta)
        || !find_box(udta.data, udta.size, FOURCC('m', 'e', 't', 'a'), &meta) || meta.size < 4
        || !find_box(meta.data + 4, meta.size - 4, FOURCC('i', 'l', 's', 't'), &ilst)) {
        return;
    }
    cursor = ilst.data;
    while (next_box(&cursor, ilst.data + ilst.size, &item)) {
        box name;
        box value;
        unsigned long long fields[4] = { 0, 0, 0, 0 };
        unsigned int       field = 0;
        unsigned int       i;
        int                inField = 0;

        if (item.type != FOURCC('-', '-', '-', '-')
            || !find_box(item.data, item.size, FOURCC('n', 'a', 'm', 'e'), &name) || name.size != 12
            || memcmp(name.data + 4, "iTunSMPB", 8) != 0
            || !find_box(item.data, item.size, FOURCC('d', 'a', 't', 'a'), &value) || value.size < 8) {
            continue;
        }
        for (i = 8; i < value.size && field < 4; i++) {
            unsigned char c = value.data[i];
            int digit = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                      : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
            if (digit >= 0) {
                fields[field] = (fields[field] << 4) | (unsigned int)digit;
                inField = 1;
            } else if (inField) {
                field++;
                inField = 0;
            }
        }
        if (field >= 4 || (field == 3 && inField)) {
            mp4->smpbPriming = fields[1];
            mp4->smpbSamples = fields[3];
            mp4->hasSmpb     = fields[3] != 0;
        }
        return;
    }
}

/* Offsets of every access unit from stsc runs over stco/co64 and the stsz sizes. */
static int build_offsets(ampaac_codec* aac, const box* stsc, const box* stco, int co64, const box* stsz) {
    ampaac_mp4*  mp4 = &aac->mp4;
    unsigned int runs = be32(stsc->data + 4);
    unsigned int chunks = be32(stco->data + 4);
    unsigned int uniform = be32(stsz->data + 4);
    unsigned int count = be32(stsz->data + 8);
    unsigned int chunkBytes = co64 ? 8 : 4;
    unsigned int sample = 0;
    unsigned int run;

    if (count == 0 || count > SAMPLE_CAP
        || stsc->size < 8 + (unsigned long long)runs * 12
        || stco->size < 8 + (unsigned long long)chunks * chunkBytes
        || (uniform == 0 && stsz->size < 12 + (unsigned long long)count * 4)) {
        return 0;
    }
    mp4->sizes      = uniform ? NULL : stsz->data + 12;
    mp4->uniform    = uniform;
    mp4->offsets    = (unsigned int*)FMOD_CODEC_ALLOC(aac->reader.codec, count * sizeof(unsigned int), 16);
    if (!mp4->offsets) {
        return 0;
    }
    for (run = 0; run < runs && sample < count; run++) {
        const unsigned char* r = stsc->data + 8 + run * 12;
        unsigned int first = be32(r);
        unsigned int perChunk = be32(r + 4);
        unsigned int last = run + 1 < runs ? be32(r + 12) - 1 : chunks;
        unsigned int chunk;

        if (first == 0 || last > chunks || perChunk == 0) {
            break;
        }
        for (chunk = first; chunk <= last && sample < count; chunk++) {
            unsigned long long offset = co64 ? be64(stco->data + 8 + (chunk - 1) * 8)
                                             : be32(stco->data + 8 + (chunk - 1) * 4);
            unsigned int       n;
            for (n = 0; n < perChunk && sample < count; n++) {
                unsigned int size = uniform ? uniform : be32(mp4->sizes + sample * 4);
                if (offset + size > AMPAAC_UNKNOWN || size == 0 || size > AMPAAC_MAX_AU
                    || (aac->reader.size != AMPAAC_UNKNOWN && offset + size > aac->reader.size)) {
                    mp4->samples = sample;   /* stop at the first unusable sample (truncated file) */
                    return sample > 0;
                }
                mp4->offsets[sample++] = (unsigned int)offset;
                offset += size;
            }
        }
    }
    mp4->samples = sample;
    return sample > 0;
}

static int parse_track(ampaac_codec* aac, const box* trak) {
    ampaac_mp4* mp4 = &aac->mp4;
    box mdia, hdlr, mdhd, minf, stbl, stsd, stsc, stco, stsz, stss, edts, elst;
    int co64 = 0;

    if (!find_box(trak->data, trak->size, FOURCC('m', 'd', 'i', 'a'), &mdia)
        || !find_box(mdia.data, mdia.size, FOURCC('h', 'd', 'l', 'r'), &hdlr) || hdlr.size < 12
        || be32(hdlr.data + 8) != FOURCC('s', 'o', 'u', 'n')
        || !find_box(mdia.data, mdia.size, FOURCC('m', 'd', 'h', 'd'), &mdhd)
        || !find_box(mdia.data, mdia.size, FOURCC('m', 'i', 'n', 'f'), &minf)
        || !find_box(minf.data, minf.size, FOURCC('s', 't', 'b', 'l'), &stbl)
        || !find_box(stbl.data, stbl.size, FOURCC('s', 't', 's', 'd'), &stsd)
        || !parse_sample_entry(&stsd, mp4)) {
        return 0;
    }
    /* MPEG-4 Audio, or MPEG-2 AAC Main/LC/SSR profiles. */
    if (mp4->objectType != 0x40 && (mp4->objectType < 0x66 || mp4->objectType > 0x68)) {
        return 0;
    }
    mp4->timescale = media_timescale(&mdhd, &mp4->mediaDuration);
    if (mp4->timescale == 0) {
        return 0;
    }
    if (!find_box(stbl.data, stbl.size, FOURCC('s', 't', 's', 'c'), &stsc) || stsc.size < 8
        || !find_box(stbl.data, stbl.size, FOURCC('s', 't', 's', 'z'), &stsz) || stsz.size < 12) {
        return 0;
    }
    if (!find_box(stbl.data, stbl.size, FOURCC('s', 't', 'c', 'o'), &stco)) {
        if (!find_box(stbl.data, stbl.size, FOURCC('c', 'o', '6', '4'), &stco)) {
            return 0;
        }
        co64 = 1;
    }
    if (stco.size < 8 || !build_offsets(aac, &stsc, &stco, co64, &stsz)) {
        return 0;
    }
    if (find_box(stbl.data, stbl.size, FOURCC('s', 't', 's', 's'), &stss) && stss.size >= 8
        && stss.size >= 8 + (unsigned long long)be32(stss.data + 4) * 4) {
        mp4->sync      = stss.data + 8;
        mp4->syncCount = be32(stss.data + 4);
    }
    if (find_box(trak->data, trak->size, FOURCC('e', 'd', 't', 's'), &edts)
        && find_box(edts.data, edts.size, FOURCC('e', 'l', 's', 't'), &elst)) {
        parse_elst(&elst, mp4);
    }
    return 1;
}

/* Walks the top-level boxes for moov (reading past mdat when the file is not "fast start"). */
static FMOD_RESULT read_moov(ampaac_codec* aac) {
    ampaac_reader* rd = &aac->reader;
    unsigned int   pos = 0;
    unsigned int   boxes;

    for (boxes = 0; boxes < AMPAAC_BOX_LIMIT; boxes++) {
        const unsigned char* p;
        unsigned int         avail;
        unsigned long long   size;
        unsigned int         header = 8;
        unsigned int         type;

        if (ampaac_reader_seek(rd, pos) != FMOD_OK) {
            return rd->fault;
        }
        avail = ampaac_reader_peek(rd, 16, &p);
        if (avail < 8) {
            return rd->fault != FMOD_OK ? rd->fault : FMOD_ERR_FORMAT;
        }
        size = be32(p);
        type = be32(p + 4);
        if (size == 1) {
            if (avail < 16) {
                return FMOD_ERR_FORMAT;
            }
            size   = be64(p + 8);
            header = 16;
        } else if (size == 0) {
            size = rd->size == AMPAAC_UNKNOWN ? 0 : (unsigned long long)(rd->size - pos);
        }
        if (size < header || pos + size > AMPAAC_UNKNOWN) {
            return FMOD_ERR_FORMAT;
        }
        if (type == FOURCC('m', 'o', 'o', 'f')) {
            return FMOD_ERR_FORMAT;   /* fragmented MP4 */
        }
        if (type == FOURCC('m', 'o', 'o', 'v')) {
            unsigned int got = 0;
            if (size > MOOV_CAP) {
                return FMOD_ERR_FORMAT;
            }
            aac->mp4.moov = (unsigned char*)FMOD_CODEC_ALLOC(rd->codec, (unsigned int)size, 16);
            if (!aac->mp4.moov) {
                return FMOD_ERR_MEMORY;
            }
            /* moov can exceed the read window: copy it through in window-sized pieces. */
            while (got < size) {
                unsigned int want = (unsigned int)size - got < AMPAAC_READ_BUF ? (unsigned int)size - got : AMPAAC_READ_BUF;
                avail = ampaac_reader_peek(rd, want, &p);
                if (avail == 0) {
                    return rd->fault != FMOD_OK ? rd->fault : FMOD_ERR_FORMAT;
                }
                memcpy(aac->mp4.moov + got, p, avail);
                ampaac_reader_skip(rd, avail);
                got += avail;
            }
            aac->mp4.moovSize = (unsigned int)size;
            return FMOD_OK;
        }
        pos += (unsigned int)size;
        if (rd->size != AMPAAC_UNKNOWN && pos >= rd->size) {
            return FMOD_ERR_FORMAT;
        }
    }
    return FMOD_ERR_FORMAT;
}

FMOD_RESULT ampaac_mp4_open(ampaac_codec* aac) {
    ampaac_mp4*          mp4 = &aac->mp4;
    const unsigned char* cursor;
    box                  moov;
    box                  trak;
    box                  mvex;
    FMOD_RESULT          res = read_moov(aac);

    if (res != FMOD_OK) {
        return res;
    }
    cursor = mp4->moov;
    if (!next_box(&cursor, mp4->moov + mp4->moovSize, &moov)
        || find_box(moov.data, moov.size, FOURCC('m', 'v', 'e', 'x'), &mvex)) {
        return FMOD_ERR_FORMAT;   /* malformed, or fragmented (mvex) */
    }
    {
        box mvhd;
        unsigned long long unused;
        if (find_box(moov.data, moov.size, FOURCC('m', 'v', 'h', 'd'), &mvhd)) {
            mp4->movieTimescale = media_timescale(&mvhd, &unused);   /* mvhd shares mdhd's layout here */
        }
    }
    cursor = moov.data;
    while (next_box(&cursor, moov.data + moov.size, &trak)) {
        if (trak.type != FOURCC('t', 'r', 'a', 'k')) {
            continue;
        }
        if (parse_track(aac, &trak)) {
            break;
        }
        /* Not an AAC audio track: drop what it left and try the next. */
        if (mp4->offsets) {
            FMOD_CODEC_FREE(aac->reader.codec, mp4->offsets);
        }
        memset(&mp4->objectType, 0, sizeof(*mp4) - offsetof(ampaac_mp4, objectType));
    }
    if (!mp4->offsets) {
        return FMOD_ERR_FORMAT;
    }
    if (!mp4->hasEdit) {
        parse_itunsmpb(moov.data, moov.size, mp4);
    }
    aac->container = AMPAAC_MP4;
    mp4->next      = 0;
    return FMOD_OK;
}

/* value * rate / timescale, saturating: a 64-bit edit or iTunSMPB field can exceed what the product holds, and
   the clamps in ampaac_mp4_set_trim then treat the value as absent. */
static unsigned long long scale_samples(unsigned long long value, unsigned int rate, unsigned int timescale) {
    unsigned long long whole = value / timescale;

    if (rate != 0 && whole >= ~0ULL / rate) {
        return ~0ULL;
    }
    return whole * rate + value % timescale * rate / timescale;
}

/* Trim (priming and presented length) in output samples, once the output rate and frame size are known. */
void ampaac_mp4_set_trim(ampaac_codec* aac) {
    ampaac_mp4*        mp4 = &aac->mp4;
    unsigned long long total = (unsigned long long)mp4->samples * (unsigned int)aac->frameSize;
    unsigned long long lead = 0;
    unsigned long long length = total;
    unsigned int       rate = (unsigned int)aac->sampleRate;
    unsigned int       delay = aac->decoderDelay;   /* the drain restores these at the end */

    if (mp4->hasEdit && mp4->movieTimescale) {
        lead   = scale_samples(mp4->editMediaTime, rate, mp4->timescale);
        length = scale_samples(mp4->editDuration, rate, mp4->movieTimescale);
    } else if (mp4->hasSmpb) {
        lead   = scale_samples(mp4->smpbPriming, rate, mp4->timescale);
        length = scale_samples(mp4->smpbSamples, rate, mp4->timescale);
    }
    if (lead > total || lead + delay >= AMPAAC_UNKNOWN) {
        lead = 0;
    }
    if (length == 0 || length > total - lead) {
        length = total - lead;
    }
    aac->leadPcm = (unsigned int)(lead + delay);
    if (lead + delay + length >= AMPAAC_UNKNOWN) {
        /* Past FMOD's 32-bit PCM positions: play to the end without declaring a length. */
        aac->endPcm      = AMPAAC_UNKNOWN;
        aac->lengthPcm   = AMPAAC_UNKNOWN;
        aac->lengthExact = 0;
        return;
    }
    aac->endPcm      = (unsigned int)(lead + delay + length);
    aac->lengthPcm   = (unsigned int)length;
    aac->lengthExact = 1;
}

FMOD_RESULT ampaac_mp4_next(ampaac_codec* aac, unsigned int* auLen) {
    ampaac_mp4*          mp4 = &aac->mp4;
    const unsigned char* p;
    unsigned int         size;
    FMOD_RESULT          res;

    if (mp4->next >= mp4->samples) {
        return FMOD_ERR_FILE_EOF;
    }
    size = mp4->sizes ? be32(mp4->sizes + mp4->next * 4) : mp4->uniform;
    res  = ampaac_reader_seek(&aac->reader, mp4->offsets[mp4->next]);
    if (res != FMOD_OK) {
        return res;
    }
    if (ampaac_reader_peek(&aac->reader, size, &p) < size) {
        return aac->reader.fault != FMOD_OK ? aac->reader.fault : FMOD_ERR_FILE_EOF;
    }
    memcpy(aac->au, p, size);
    ampaac_reader_skip(&aac->reader, size);
    *auLen = size;
    mp4->next++;
    return FMOD_OK;
}

/* Decoding restarts AMPAAC_PREROLL_AUS access units ahead of the target's. xHE-AAC (USAC) decodes only
   from immediate-playout frames, so there it starts at the sync sample (stss) at or before that point;
   for other object types every access unit decodes after the pre-roll, and stss is ignored. */
FMOD_RESULT ampaac_mp4_seek(ampaac_codec* aac, unsigned int targetPcm) {
    ampaac_mp4*        mp4 = &aac->mp4;
    unsigned int       au = aac->frameSize ? targetPcm / (unsigned int)aac->frameSize : 0;
    unsigned int       start;
    unsigned long long pcm;

    if (au >= mp4->samples) {
        au = mp4->samples ? mp4->samples - 1 : 0;
    }
    start = au > AMPAAC_PREROLL_AUS ? au - AMPAAC_PREROLL_AUS : 0;
    if (mp4->usac && mp4->sync && mp4->syncCount > 0) {
        /* The last sync sample at or before start (entries are 1-based and ascending). */
        unsigned int lo = 0;
        unsigned int hi = mp4->syncCount;
        while (lo < hi) {
            unsigned int mid = (lo + hi) / 2;
            if (be32(mp4->sync + mid * 4) - 1 <= start) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        start = lo > 0 ? be32(mp4->sync + (lo - 1) * 4) - 1 : 0;
    }
    pcm             = (unsigned long long)start * (unsigned int)aac->frameSize;
    mp4->next       = start;
    aac->decodedPcm = pcm < AMPAAC_UNKNOWN ? (unsigned int)pcm : AMPAAC_UNKNOWN - 1;
    aac->exact      = 1;
    return FMOD_OK;
}

void ampaac_mp4_close(ampaac_codec* aac) {
    if (aac->mp4.offsets) {
        FMOD_CODEC_FREE(aac->reader.codec, aac->mp4.offsets);
    }
    if (aac->mp4.moov) {
        FMOD_CODEC_FREE(aac->reader.codec, aac->mp4.moov);
    }
}
