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
static int wet_thread_started = 0;
static int ret_thread_started = 0;
static int thread_initialized = 0;
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

static void set_thread_error(void)
{
    __atomic_store_n(&thread_error, 1, __ATOMIC_RELAXED);
}

void event_handler(int fd, short e __attribute__((unused)), void* args)
{
    WEVENT_T* ev = (WEVENT_T*)args;

    if (!isempty_buffer(&ev->buffer)) {
        int size = cwrite(fd, ev->buffer.cur, ev->buffer.size);
        if (size < 0) {
            set_thread_error();
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
        set_thread_error();
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
        set_thread_error();
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
            set_thread_error();
            log("STDOUT write error: %s", strerror(errno));
            event_del(&fdev->ev);
            return;
        }
    }
}

int thread_init(int num)
{
    WRITE_EV_THREAD* wet_tmp = NULL;
    READ_EV_THREAD* ret_tmp = NULL;

    if (cmalloc((void**)&wet_tmp, (sizeof(WRITE_EV_THREAD) + num * sizeof(WEVENT_T))) < 0 || wet_tmp == NULL) {
        error(EXIT_FAILURE, errno, "not enough memory\n");
        return -1;
    }

    memset(wet_tmp, 0, sizeof(WRITE_EV_THREAD) + num * sizeof(WEVENT_T));
    wet_tmp->base = event_init();
    if (wet_tmp->base == NULL) {
        free(wet_tmp);
        return -1;
    }

    wet_tmp->evno = num;
    wet_tmp->next_child = 0;
    wet_tmp->stdin_eof = 0;
    wet_tmp->stdin_paused = 0;
    for (int i = 0; i < num; ++i) {
        wet_tmp->ev[i].fd = -1;
    }
    if (create_stream(&wet_tmp->stream, 4096 * 1024) != E_OK) {
        event_base_free(wet_tmp->base);
        free(wet_tmp);
        return -1;
    }
    wet_tmp->stream.fd = STDIN_FILENO;
    event_set(&wet_tmp->stdin_ev, STDIN_FILENO, EV_READ | EV_PERSIST, stdin_handler, wet_tmp);
    if (event_base_set(wet_tmp->base, &wet_tmp->stdin_ev) < 0 ||
        event_add(&wet_tmp->stdin_ev, 0) < 0) {
        close_stream(&wet_tmp->stream);
        event_base_free(wet_tmp->base);
        free(wet_tmp);
        return -1;
    }

    if (cmalloc((void**)&ret_tmp, (sizeof(READ_EV_THREAD) + num * sizeof(struct fdev_t))) < 0 || ret_tmp == NULL) {
        close_stream(&wet_tmp->stream);
        event_base_free(wet_tmp->base);
        free(wet_tmp);
        error(EXIT_FAILURE, errno, "not enough memory\n");
        return -1;
    }

    memset(ret_tmp, 0, sizeof(READ_EV_THREAD) + num * sizeof(struct fdev_t));
    ret_tmp->base = event_init();
    if (ret_tmp->base == NULL) {
        free(ret_tmp);
        close_stream(&wet_tmp->stream);
        event_base_free(wet_tmp->base);
        free(wet_tmp);
        return -1;
    }

    ret_tmp->evno = num;
    for (int i = 0; i < num; ++i) {
        ret_tmp->evs[i].fd = -1;
        ret_tmp->evs[i].stream.fd = -1;
    }

    wet = wet_tmp;
    ret = ret_tmp;
    __atomic_store_n(&thread_error, 0, __ATOMIC_RELAXED);
    wet_thread_started = 0;
    ret_thread_started = 0;
    thread_initialized = 1;
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
    if (alloc_buffer(&wet->ev[i].buffer, 4096) < 0) {
        return -1;
    }
    if (event_base_set(wet->base, &(wet->ev[i].e)) < 0) {
        dealloc_buffer(&wet->ev[i].buffer);
        return -1;
    }
    wet->ev[i].fd = wfd;
    wet->ev[i].write_enabled = 0;
    
    struct fdev_t* evs = ret->evs;
    if (create_stream(&evs[i].stream, 4096) != E_OK) {
        dealloc_buffer(&wet->ev[i].buffer);
        return -1;
    }
    evs[i].stream.fd = rfd;
    event_set(&evs[i].ev, rfd, EV_READ | EV_PERSIST, revent_handler, (void*)&evs[i]);
    if (event_base_set(ret->base, &evs[i].ev) < 0 ||
        event_add(&evs[i].ev, 0) < 0) {
        close_stream(&evs[i].stream);
        dealloc_buffer(&wet->ev[i].buffer);
        wet->ev[i].fd = -1;
        return -1;
    }

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
    wet_thread_started = 1;

    rc = pthread_create(&ret->id, NULL, rthread_proc, (void*)ret);
    if (rc != 0) {
        log("pthread_create error: %s\n", strerror(rc));
        stop_event_loop(wet->base);
        pthread_join(wet->id, NULL);
        wet_thread_started = 0;
        return -1;
    }
    ret_thread_started = 1;
    return 0;
}

int thread_term()
{
    void* retval = NULL;
    int rc = 0;

    if (!thread_initialized) {
        return 0;
    }

    if (wet_thread_started) {
        int join_rc = pthread_join(wet->id, &retval);
        if (join_rc != 0) {
            log("pthread_join error: %s\n", strerror(join_rc));
            rc = -1;
        }
    }
    for (int i = 0; i < wet->evno; ++i) {
        dealloc_buffer(&(wet->ev[i].buffer));
    }
    event_base_free(wet->base);
    close_stream(&wet->stream);
    free(wet);
    wet = NULL;
    
    if (ret_thread_started) {
        int join_rc = pthread_join(ret->id, &retval);
        if (join_rc != 0) {
            log("pthread_join error: %s\n", strerror(join_rc));
            rc = -1;
        }
    }
    event_base_free(ret->base);
    for (int i = 0; i < ret->evno; ++i) {
        close_stream(&ret->evs[i].stream);
    }
    free(ret);
    ret = NULL;

    thread_add_idx = 0;
    wet_thread_started = 0;
    ret_thread_started = 0;
    thread_initialized = 0;
    return rc;
}

int thread_failed()
{
    return __atomic_load_n(&thread_error, __ATOMIC_RELAXED);
}
