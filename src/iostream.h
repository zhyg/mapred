#ifndef MAPRED_IOSTREAM_H
#define MAPRED_IOSTREAM_H

#include <stddef.h>

enum 
{
    E_OK = 0,
    E_ERROR,
    E_NEED_MORE,
    E_EOF
};

typedef struct
{
    int     fd;
    char*   ptr;
    char*   cur;
    size_t  bytes;
    size_t  capacity;
}IOstream;

int create_stream(IOstream* stream, int bufsize);
int try_read_more(IOstream* stream);
int get_line(IOstream* stream, char** line, int* size);
int get_all_lines(IOstream* stream, char** line, int* len);
int close_stream(IOstream* stream);

#endif /* MAPRED_IOSTREAM_H */
