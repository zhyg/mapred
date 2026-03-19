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

typedef struct 
{
    int fd;
    TBuffer buffer;
    struct event e;
    int write_enabled;
} WEVENT_T;

typedef struct
{
    pthread_t id;
    struct event_base* base;
    IOstream stream;
    struct event stdin_ev;
    int evno;
    int next_child;
    int stdin_eof;
    int stdin_paused;
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
static int thread_add_idx = 0;
static int thread_error = 0;
extern int cmalloc(void** ptr, size_t size);

static ssize_t cwrite(int fd, char* buf, size_t size)
{
    size_t count = 0;
    do { 
        ssize_t s = write(fd, buf + count, size - count);
        if (s < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) break;
            fprintf(stderr, "write error occur\n");
            return -1;
        }
        count += (size_t)s;
    } while (count < size);
    return (ssize_t)count;
}

static void stop_event_loop(struct event_base* base)
{
    if (base != NULL) {
        event_base_loopbreak(base);
    }
}

void event_handler(int fd, short e __attribute__((unused)), void* args)
{
    WEVENT_T* ev = (WEVENT_T*)args;

    if (!isempty_buffer(&ev->buffer)) {
        int size = cwrite(fd, ev->buffer.cur, ev->buffer.size);
        if (size < 0) {
            thread_error = 1;
            close(fd);
            event_del(&ev->e);
            ev->write_enabled = 0;
            return;
        }
        seek_buffer(&ev->buffer, size);
    }

    if (wet->stdin_paused && ev->buffer.size < (1 * 1024 * 1024)) {
        int any_high = 0;
        for (int i = 0; i < wet->evno; ++i) {
            if (wet->ev[i].buffer.size >= (4 * 1024 * 1024)) {
                any_high = 1;
                break;
            }
        }
        if (!any_high) {
            event_add(&wet->stdin_ev, 0);
            wet->stdin_paused = 0;
        }
    }

    if (isempty_buffer(&ev->buffer)) {
        event_del(&ev->e);
        ev->write_enabled = 0;
        if (wet->stdin_eof) {
            close(fd);
        }
    }
}

void stdin_handler(int fd __attribute__((unused)), short e __attribute__((unused)), void* args)
{
    WRITE_EV_THREAD* et = (WRITE_EV_THREAD*)args;
    IOstream* stream = &(et->stream);
    char* line = NULL;
    int len = 0;

    int r = try_read_more(stream);
    if (r == E_ERROR) {
        thread_error = 1;
        log("stdin read error\n");
        et->stdin_eof = 1;
        event_del(&et->stdin_ev);
    } else if (r == E_EOF) {
        et->stdin_eof = 1;
        event_del(&et->stdin_ev);

        // Distribute remaining partial line if any
        if (stream->bytes > 0) {
            int child = et->next_child;
            et->next_child = (et->next_child + 1) % et->evno;
            expand_buffer(&et->ev[child].buffer, stream->cur, stream->bytes);
            stream->bytes = 0;
        }
    }

    while (get_line(stream, &line, &len) == E_OK) {
        int child = et->next_child;
        et->next_child = (et->next_child + 1) % et->evno;
        expand_buffer(&et->ev[child].buffer, line, len);

        if (et->ev[child].buffer.size > (4 * 1024 * 1024)) {
            event_del(&et->stdin_ev);
            et->stdin_paused = 1;
            break; // Pause reading, let children drain
        }
    }

    for (int i = 0; i < et->evno; ++i) {
        if (!isempty_buffer(&et->ev[i].buffer) && !et->ev[i].write_enabled) {
            event_add(&et->ev[i].e, 0);
            et->ev[i].write_enabled = 1;
        } else if (et->stdin_eof && isempty_buffer(&et->ev[i].buffer) && !et->ev[i].write_enabled) {
            close(et->ev[i].fd);
        }
    }
}

void revent_handler(int fd __attribute__((unused)), short e __attribute__((unused)), void* args)
{
    char*   line = NULL;
    int     len = 0;
    struct  fdev_t* fdev = (struct fdev_t*)args;
    IOstream* b = &fdev->stream;

    int rc = try_read_more(b);
    if (rc == E_ERROR) {
        thread_error = 1;
        log("child output read error\n");
        event_del(&fdev->ev);
        return;
    }

    if (rc == E_EOF) {
        if (b->bytes > 0) {
            cwrite(STDOUT_FILENO, b->cur, b->bytes);
        }
        event_del(&fdev->ev);
        return;
    }

    for (; get_line(b, &line, &len) == E_OK; ) {
        if (cwrite(STDOUT_FILENO, line, len) < 0) {
            thread_error = 1;
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
    wet->next_child = 0;
    wet->stdin_eof = 0;
    wet->stdin_paused = 0;
    memset(&wet->ev, 0, num * sizeof(WEVENT_T));
    create_stream(&wet->stream, 4096 * 1024);
    wet->stream.fd = STDIN_FILENO;
    event_set(&wet->stdin_ev, STDIN_FILENO, EV_READ | EV_PERSIST, stdin_handler, wet);
    event_base_set(wet->base, &wet->stdin_ev);
    event_add(&wet->stdin_ev, 0);

    // ret = (READ_EV_THREAD*)cmalloc(sizeof(READ_EV_THREAD) + num * sizeof(struct fdev_t));
    if (cmalloc((void**)&ret, (sizeof(READ_EV_THREAD) + num * sizeof(struct fdev_t))) < 0 || ret == NULL) {
        error(EXIT_FAILURE, errno, "not enough memory\n");
        return -1;
    }
    ret->base = event_init();
    ret->evno = num;
    thread_error = 0;
    return 0;
}

int thread_add(int wfd, int rfd) 
{
    if (thread_add_idx >= wet->evno) {
        log("thread add error, out of bound\n");
        return -1;
    }
    int i = thread_add_idx;
    event_set(&(wet->ev[i].e), wfd, EV_WRITE | EV_PERSIST, event_handler, (void*)&(wet->ev[i]));
    event_base_set(wet->base, &(wet->ev[i].e));
    alloc_buffer(&wet->ev[i].buffer, 4096);
    wet->ev[i].fd = wfd;
    wet->ev[i].write_enabled = 0;
    
    struct fdev_t* evs = ret->evs;
    event_set(&evs[i].ev, rfd, EV_READ | EV_PERSIST, revent_handler, (void*)&evs[i]);
    event_base_set(ret->base, &evs[i].ev);
    event_add(&evs[i].ev, 0);
    create_stream(&evs[i].stream, 4096);
    evs[i].stream.fd = rfd;

    ++thread_add_idx;
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
    int rc = pthread_create(&wet->id, NULL, wthread_proc, (void*)wet);
    if (rc != 0) {
        log("pthread_create error: %s\n", strerror(rc));
        return -1;
    }

    rc = pthread_create(&ret->id, NULL, rthread_proc, (void*)ret);
    if (rc != 0) {
        log("pthread_create error: %s\n", strerror(rc));
        stop_event_loop(wet->base);
        pthread_join(wet->id, NULL);
        return -1;
    }
    return 0;
}

int thread_term()
{
    void* retval = NULL;

    pthread_join(wet->id, &retval);
    for (int i = 0; i < wet->evno; ++i) {
        dealloc_buffer(&(wet->ev[i].buffer));
    }
    event_base_free(wet->base);
    close_stream(&wet->stream);
    free(wet);
    
    pthread_join(ret->id, &retval);
    event_base_free(ret->base);
    for (int i = 0; i < ret->evno; ++i) {
        close_stream(&ret->evs[i].stream);
    }
    free(ret);

    thread_add_idx = 0;
    return 0;
}

int thread_failed()
{
    return thread_error;
}
