#ifndef _T_BUFFER_H
#define _T_BUFFER_H

typedef struct
{
    char*   ptr;
    char*   cur;
    int     size;
    int     capacity;
}TBuffer;

int alloc_buffer(TBuffer* b, int alloc);
int expand_buffer(TBuffer* b, char* d, int size);
int seek_buffer(TBuffer* b, int size);
int isempty_buffer(TBuffer* b);
int dealloc_buffer(TBuffer* b);

#endif /* _T_BUFFER_H */
