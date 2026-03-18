#ifndef MAPRED_TBUF_H
#define MAPRED_TBUF_H

#include <stddef.h>

typedef struct
{
    char*   ptr;
    char*   cur;
    size_t  size;
    size_t  capacity;
}TBuffer;

int alloc_buffer(TBuffer* b, size_t alloc);
int expand_buffer(TBuffer* b, char* d, size_t size);
int seek_buffer(TBuffer* b, size_t size);
int isempty_buffer(TBuffer* b);
int dealloc_buffer(TBuffer* b);

#endif /* MAPRED_TBUF_H */
