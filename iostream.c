#include <unistd.h>
#include <stdio.h>
#include <error.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "iostream.h"
#include "util.h"

int create_stream(IOstream* stream, int bufsize)
{
    stream->capacity = bufsize;
    stream->bytes = 0;
    stream->ptr = (char*)malloc(stream->capacity);
    if (!stream->ptr) {
        return E_ERROR;
    }
    stream->cur = stream->ptr;
    return E_OK;
}

int close_stream(IOstream* stream)
{
    if (stream->ptr) {
        free(stream->ptr);
    }
    stream->ptr = stream->cur = NULL;
    stream->capacity = stream->bytes = 0;
    return E_OK;
}

int try_read_more(IOstream* s)
{
    if (s->cur != s->ptr) {
        if (s->bytes != 0) {
            memmove(s->ptr, s->cur, s->bytes);
        }
        s->cur = s->ptr;
    }

    if (s->bytes >= s->capacity) {
        char* new_ptr = (char*)realloc(s->ptr, s->capacity * 2);
        if (!new_ptr) {
            error(EXIT_FAILURE, errno, "not enough memory, realloc buf");
        }
        s->cur = s->ptr = new_ptr;
        s->capacity *= 2;
    }

    int avail = s->capacity - s->bytes;
    int nread = read(s->fd, s->cur + s->bytes, avail);
    if (nread > 0) {
        s->bytes += nread;
    }
    
    if (nread == 0) return E_ERROR;
    if (nread < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return E_OK;
        }
        log("read error, %s\n", strerror(errno));
        return E_ERROR;
    }

    return E_OK;
}

int get_line(IOstream* s, char** line, int* len)
{
    char* bp = (char*)memchr(s->cur, '\n', s->bytes);
    if (bp == NULL) {
        return E_NEED_MORE;
    }
    
    *line = s->cur;
    *len = ++bp - s->cur;
    s->cur = bp;
    s->bytes -= *len;
    return E_OK;
}

int get_all_lines(IOstream* s, char** line, int* len)
{
    char* bp = (char*)memrchr((void*)(s->cur), '\n', s->bytes);
    if (bp == NULL) {
        return E_NEED_MORE;
    }
    
    *line = s->cur;
    *len = ++bp - s->cur;
    s->cur = bp;
    s->bytes -= *len;
    return E_OK;
}
