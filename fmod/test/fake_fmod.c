#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_fmod.h"

static long liveAllocations;

long fake_live_allocations(void) {
    return liveAllocations;
}

static FMOD_RESULT F_CALLBACK fake_metadata(FMOD_CODEC_STATE* state, FMOD_TAGTYPE type, char* name, void* data,
                                            unsigned int len, FMOD_TAGDATATYPE datatype, int unique) {
    fake_file* file = (fake_file*)state;
    (void)unique;
    if (type == FMOD_TAGTYPE_FMOD && strcmp(name, "Sample Rate Change") == 0
        && datatype == FMOD_TAGDATATYPE_FLOAT && len == sizeof(float)) {
        memcpy(&file->lastSampleRate, data, sizeof(float));
        file->sampleRateTags++;
    }
    if (type == FMOD_TAGTYPE_USER && strcmp(name, "AMPAAC_LENGTH_MS") == 0
        && datatype == FMOD_TAGDATATYPE_INT && len == sizeof(unsigned int)) {
        memcpy(&file->lengthTagMs, data, sizeof(unsigned int));
        file->lengthTags++;
    }
    return FMOD_OK;
}

static void* F_CALLBACK fake_alloc(unsigned int size, unsigned int align, const char* where, int line) {
    void* ptr = NULL;
    (void)where;
    (void)line;
    if (align < sizeof(void*)) {
        align = sizeof(void*);
    }
    if (posix_memalign(&ptr, align, size) != 0) {
        return NULL;
    }
    liveAllocations++;
    return ptr;
}

static void F_CALLBACK fake_free(void* ptr, const char* where, int line) {
    (void)where;
    (void)line;
    if (ptr) {
        liveAllocations--;
        free(ptr);
    }
}

static void F_CALLBACK fake_log(FMOD_DEBUG_FLAGS level, const char* file, int line, const char* function, const char* format, ...) {
    (void)level;
    (void)file;
    (void)line;
    (void)function;
    (void)format;
}

static FMOD_RESULT F_CALLBACK fake_read(FMOD_CODEC_STATE* state, void* buffer, unsigned int sizebytes, unsigned int* bytesread) {
    fake_file*   file = (fake_file*)state;
    unsigned int want = sizebytes;
    unsigned int left = file->pos < file->size ? file->size - file->pos : 0;

    file->reads++;
    *bytesread = 0;
    if (file->failAtPos != FMOD_OK && file->pos >= file->failPos) {
        return file->failAtPos;
    }
    if (file->maxChunk && want > file->maxChunk) {
        want = file->maxChunk;
    }
    if (want > left) {
        want = left;
    }
    memcpy(buffer, file->data + file->pos, want);
    file->pos += want;
    *bytesread = want;
    if (want < sizebytes && file->pos >= file->size) {
        return FMOD_ERR_FILE_EOF;
    }
    return FMOD_OK;
}

static FMOD_RESULT F_CALLBACK fake_seek(FMOD_CODEC_STATE* state, unsigned int pos, FMOD_CODEC_SEEK_METHOD method) {
    fake_file* file = (fake_file*)state;
    file->seeks++;
    if (method != FMOD_CODEC_SEEK_METHOD_SET) {
        return FMOD_ERR_INVALID_PARAM;
    }
    file->pos = pos > file->size ? file->size : pos;
    return FMOD_OK;
}

static FMOD_RESULT F_CALLBACK fake_tell(FMOD_CODEC_STATE* state, unsigned int* pos) {
    *pos = ((fake_file*)state)->pos;
    return FMOD_OK;
}

static FMOD_RESULT F_CALLBACK fake_size(FMOD_CODEC_STATE* state, unsigned int* size) {
    fake_file* file = (fake_file*)state;
    *size = file->sizeUnknown ? 0 : file->size;
    return FMOD_OK;
}

void fake_file_init(fake_file* file, const unsigned char* data, unsigned int size) {
    memset(file, 0, sizeof(*file));
    file->data               = data;
    file->size               = size;
    file->failAtPos          = FMOD_OK;
    file->functions.metadata = fake_metadata;
    file->functions.alloc    = fake_alloc;
    file->functions.free     = fake_free;
    file->functions.log      = fake_log;
    file->functions.read     = fake_read;
    file->functions.seek     = fake_seek;
    file->functions.tell     = fake_tell;
    file->functions.size     = fake_size;
    file->state.functions    = &file->functions;
}
