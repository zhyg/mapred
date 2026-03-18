#include <string.h>
#include <stdlib.h>
#include <error.h>
#include <errno.h>
#include <stdio.h>
#include "tbuf.h"
#include "util.h"

extern int cmalloc(void** ptr, size_t size);

int alloc_buffer(TBuffer* b, size_t alloc)
{
    if (alloc == 0) {
        return -1;
    }

    int rc = cmalloc((void**)&(b->ptr), alloc);
    if (rc < 0 || !b->ptr) {
        error(EXIT_FAILURE, errno, "not enough memory!\n");
        return -1;
    }

    b->cur = b->ptr;
    b->size = 0;
    b->capacity = alloc;
    return 0;
}

int expand_buffer(TBuffer* b, char* d, size_t size)
{
    while (1) {
        size_t used = (size_t)(b->cur - b->ptr) + b->size;
        if (used + size <= b->capacity) {
            memcpy(b->cur + b->size, d, size);
            b->size += size;
            return 0;
        }

        size_t new_cap = b->capacity * 2;
        if (new_cap < b->size + size) {
            new_cap = b->size + size;
        }

        char* p = NULL;
        int rc = cmalloc((void**)&p, new_cap);
        if (p == NULL || rc < 0) {
            error(EXIT_FAILURE, errno, "no memory\n");
            return -1;
        }
        memcpy(p, b->cur, b->size);
        free(b->ptr);
        b->ptr = p;
        b->cur = b->ptr;
        b->capacity = new_cap;
    }
    return 0;
}

int seek_buffer(TBuffer* b, size_t size) 
{
    if (b->size <= size) {
        b->cur = b->ptr;
        b->size = 0;
        return 0;
    }
    
    b->size -= size;
    b->cur += size;
    return 0;
}

int isempty_buffer(TBuffer* b)
{
    return b->size == 0 ? 1 : 0;
}

int dealloc_buffer(TBuffer* b)
{
    if (b->ptr != NULL) {
        free(b->ptr);
        b->ptr = NULL;
        b->cur = NULL;
        b->size = 0;
        b->capacity = 0;
    }
    return 0;
}
