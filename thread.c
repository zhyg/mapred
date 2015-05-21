#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <error.h>
#include <event.h>
#include <stdbool.h>
#include <pthread.h>
#include "iostream.h"
#include "tbuf.h"
#include "util.h"

#if 0
static int total_time = 0;
static struct timeval start, end;
#define PROFILE_START() gettimeofday(&start, NULL)
#define PROFILE_END() \
   do { \
        gettimeofday(&end, NULL); \
        /*fprintf(stderr, "cost time %ld\n", 1000000 * (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec));*/ \
   } while (0)
#endif

enum 
{
    STAT_READ_MORE,
    STAT_WRITE_LINE,
    STAT_LAST_BUF,
    STAT_CLOSE_PIPE
};

typedef struct 
{
    int fd;
    TBuffer buffer;
    struct event e;
} WEVENT_T;

typedef struct
{
    pthread_t id;
    struct event_base* base;
    IOstream stream;
    int st;
    int evno;
    WEVENT_T ev[];
} WRITE_EV_THREAD;

typedef struct 
{
    pthread_t id;
    struct event_base* base;
    int evno;
    struct fdev_t
    {
        int fd;
        struct event ev;
        IOstream stream;
    } evs[];
} READ_EV_THREAD;

static WRITE_EV_THREAD* wet;
static READ_EV_THREAD* ret;
extern int cmalloc(void** ptr, int size);

static int cwrite(int fd, char* buf, size_t size)
{
    int count = 0;
    do { 
        int s = write(fd, buf + count, size - count);
        if (s < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) break;
            fprintf(stderr, "write error occur\n");
            return -1;
        }
        count += s;
    } while (count < size);
    return count;
}

static void update_stat(int stat)
{
    wet->st = stat;
}

void event_handler(int fd, short e, void* args)
{
    char*   line =  NULL;
    int     len  =  0; 
    int     size =  0;

    int st              = wet->st;
    IOstream* stream    = &(wet->stream);
    WEVENT_T* ev        = (WEVENT_T*)args;

    if (!isempty_buffer(&ev->buffer)) {
        size = cwrite(fd, ev->buffer.cur, ev->buffer.size);
        if (size < 0) {
            log("write error, %s\n", strerror(errno));
            update_stat(STAT_CLOSE_PIPE);
        } else {
            seek_buffer(&ev->buffer, size);
            return;
        }
    }

    switch (st) {
    case STAT_READ_MORE:
        if (try_read_more(stream) == E_ERROR) {
            update_stat(STAT_LAST_BUF);
        } else {
            update_stat(STAT_WRITE_LINE);
        }
        break;

    case STAT_WRITE_LINE:
        if (get_line(stream, &line, &len) == E_NEED_MORE || len == 0) {
            update_stat(STAT_READ_MORE);
            break;
        }
    
        size = cwrite(fd, line, len);
        if (size < 0) {
            log("write error, %s\n", strerror(errno));
            update_stat(STAT_CLOSE_PIPE);
            break;
        }

        if (size < len) {
            expand_buffer(&ev->buffer, line + size, len - size);
            break;
        }
        break;

    case STAT_LAST_BUF:
        if (stream->bytes <= 0) {
            update_stat(STAT_CLOSE_PIPE);
            break;
        }
        size = cwrite(fd, stream->cur, stream->bytes);
        if (size < 0) {
            log("write error, %s\n", strerror(errno));
            update_stat(STAT_CLOSE_PIPE);
            break;
        }

        if (size == stream->bytes) {
            stream->bytes = 0;
            update_stat(STAT_CLOSE_PIPE);
            break;
        }

        if (size < stream->bytes) {
            expand_buffer(&ev->buffer, stream->cur+size, stream->bytes-size);
            stream->bytes = 0;
            break;
        }
        break;

    case STAT_CLOSE_PIPE:
        close(fd);
        event_del(&ev->e);
        break;
    } 
}

void revent_handler(int fd, short e, void* args)
{
    char*   line = NULL;
    int     len = 0;
    struct  fdev_t* fdev = (struct fdev_t*)args;
    IOstream* b = &fdev->stream;

    if (try_read_more(b) == E_ERROR) {
        if (b->bytes > 0) {
            cwrite(STDOUT_FILENO, b->cur, b->bytes);
        }
        event_del(&fdev->ev);
        return;
    }

    for (; get_line(b, &line, &len) == E_OK; ) {
        if (cwrite(STDOUT_FILENO, line, len) < 0) {
            log("STDOUT write error: %s", strerror(errno));
            event_del(&fdev->ev);
            return;
        }
    }
}

int thread_init(int num)
{
    if (cmalloc((void**)&wet, (sizeof(WRITE_EV_THREAD) + num * sizeof(WEVENT_T))) < 0 || wet == NULL) {
        error(EXIT_FAILURE, errno, "not enough memory\n");
        return -1;
    } 
    // wet = (WRITE_EV_THREAD*)cmalloc(sizeof(WRITE_EV_THREAD) + num * sizeof(WEVENT_T));
    wet->base = event_init();
    wet->evno = num;
    wet->st = STAT_READ_MORE;
    memset(&wet->ev, 0, num * sizeof(WEVENT_T));
    create_stream(&wet->stream, 4096 * 1024);
    wet->stream.fd = STDIN_FILENO;

    // ret = (READ_EV_THREAD*)cmalloc(sizeof(READ_EV_THREAD) + num * sizeof(struct fdev_t));
    if (cmalloc((void**)&ret, (sizeof(READ_EV_THREAD) + num * sizeof(struct fdev_t))) < 0 || ret == NULL) {
        error(EXIT_FAILURE, errno, "not enough memory\n");
        return -1;
    }
    ret->base = event_init();
    ret->evno = num;
    return 0;
}

int thread_add(int wfd, int rfd) 
{
    static int i = 0;
    if (i >= wet->evno) {
        log("thread add error, out of bouder\n");
        return -1;
    }
    event_set(&(wet->ev[i].e), wfd, EV_WRITE | EV_PERSIST, event_handler, (void*)&(wet->ev[i]));
    event_base_set(wet->base, &(wet->ev[i].e));
    event_add(&(wet->ev[i].e), 0);
    alloc_buffer(&wet->ev[i].buffer, 4096);
    wet->ev[i].fd = wfd;
    
    struct fdev_t* evs = ret->evs;
    event_set(&evs[i].ev, rfd, EV_READ | EV_PERSIST, revent_handler, (void*)&evs[i]);
    event_base_set(ret->base, &evs[i].ev);
    event_add(&evs[i].ev, 0);
    create_stream(&evs[i].stream, 4096);
    evs[i].stream.fd = rfd;

    ++i;
    return 0;
}

static void* wthread_proc(void* args) 
{
    WRITE_EV_THREAD* et = (WRITE_EV_THREAD*)args;
    event_base_loop(et->base, 0);
    return NULL;
}

static void* rthread_proc(void* args)
{
    READ_EV_THREAD* et = (READ_EV_THREAD*)args;
    event_base_loop(et->base, 0);
    return NULL;
}

int thread_start()
{
    if (pthread_create(&wet->id, NULL, wthread_proc, (void*)wet) < 0) {
        log("pthread_create error\n");
        return -1;
    }

    if (pthread_create(&ret->id, NULL, rthread_proc, (void*)ret) < 0) {
        log("pthread_create error\n");
        return -1;
    }
    return 0;
}

int thread_term()
{
    for (int i = 0; i < wet->evno; ++i) {
        WEVENT_T e = wet->ev[i];
        dealloc_buffer(&(e.buffer));
    }
    event_base_free(wet->base);
    close_stream(&wet->stream);
    free(wet);
    
    void* retval = NULL;
    pthread_join(ret->id, &retval);
    event_base_free(ret->base);
    for (int i = 0; i < ret->evno; ++i) {
        close_stream(&ret->evs[i].stream);
    }
    free(ret);
    return 0;
}
