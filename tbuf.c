#include <string.h>
#include <stdlib.h>
#include <error.h>
#include <errno.h>
#include <stdio.h>
#include "tbuf.h"
#include "util.h"

extern int cmalloc(void** ptr, int size);

int alloc_buffer(TBuffer* b, int alloc)
{
    int rc = -1;
    if (alloc < 0) {
        return -1;
    }

    rc = cmalloc((void**)&(b->ptr), alloc);
    if (rc < 0 || !b->ptr) {
        error(EXIT_FAILURE, errno, "not enough memory!\n");
        return -1;
    }

    b->cur = b->ptr;
    b->size = 0;
    b->capacity = alloc;
    return 0;
}

int expand_buffer(TBuffer* b, char* d, int size)
{
    while (1) {
        int remain = (b->ptr - b->cur) + b->capacity - b->size - size;
        if (remain >= 0) {
            memcpy(b->cur+b->size, d, size);
            b->size += size;
            return 0;
        }

        char* p = NULL;
        int rc = cmalloc((void**)&p, (int)(b->capacity + size));
        if (p == NULL || rc < 0) {
            error(EXIT_FAILURE, errno, "no memory\n");
            return -1;
        }
        memcpy(p, b->cur, b->size);
        free(b->ptr);
        b->ptr = p;
        b->cur = b->ptr;
        b->capacity += size;
    }
    return 0;
}

int seek_buffer(TBuffer* b, int size) 
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
    }
    return 0;
}
