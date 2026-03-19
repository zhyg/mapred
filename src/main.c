#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <error.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <execinfo.h>
#include "util.h"

#define MAPRED_VERSION "v0.2"
#define MAX_PROCESSES 1024
#define BACKTRACE_DEPTH 10

extern int set_noblocking(int fd);
extern int set_cloexec(int fd);
extern int spawn_process(const char*, int*, int*);
extern int wait_children();

extern int thread_init(int);
extern int thread_add(int, int);
extern int thread_start();
extern int thread_term();
extern int thread_failed();

static char cmd[4096];
static int count = 2;
static int pids[MAX_PROCESSES] = {0};

static void close_fd_if_open(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static int require_success(int rc, const char* msg)
{
    if (rc < 0) {
        fprintf(stderr, "%s\n", msg);
        return -1;
    }
    return 0;
}

static void terminate_children(int signo)
{
    for (int i = 0; i < count; ++i) {
        if (pids[i] > 0) {
            kill(pids[i], signo);
        }
    }
}

static void usage(char* proc)
{
    fprintf(stdout, "%s usage : \n"
            "%s [option] [INPUT] \n"
            "option is :\n"
            " --mapper | -m the command to execute in shell\n"
            " --count  | -c the process num in background\n"
            " --help   | -h \n"
            " --version| -v \n"
            , proc, proc);
}

int getopts(int argc, char** argv)
{
    if (argc == 1) {
        usage(argv[0]);
        exit(0);
    }

    char* short_opt = "m:c:hv";
    struct option long_opt[] = {
        {"mapper", required_argument, NULL, 'm'},
        {"count", required_argument, NULL, 'c'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'v'},
        {0, 0, 0, 0}
    };

    int o = -1;
    while ((o = getopt_long(argc, argv, short_opt, long_opt, NULL)) != -1) {
        switch (o) {
        case 'm': {
            size_t len = strlen(optarg);
            if (len >= sizeof(cmd)) {
                fprintf(stderr, "mapper command too long (max %zu)\n", sizeof(cmd) - 1);
                exit(EXIT_FAILURE);
            }
            memset(cmd, 0, sizeof(cmd));
            memcpy(cmd, optarg, len);
            break;
        }
        case 'c': {
            char *endptr = NULL;
            long val = strtol(optarg, &endptr, 10);
            if (*endptr != '\0' || val < 1 || val > MAX_PROCESSES) {
                fprintf(stderr, "invalid count: must be between 1 and %d\n", MAX_PROCESSES);
                exit(EXIT_FAILURE);
            }
            count = (int)val;
            break;
        }
        case 'h':
            usage(argv[0]);
            exit(0);
            break;
        case 'v':
            fprintf(stdout, "mapred %s\n", MAPRED_VERSION);
            exit(0);
        }
    }

    if (cmd[0] == '\0') {
        fprintf(stderr, "missing required --mapper/-m option\n");
        usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    if (optind < argc) {
        int fd = open(argv[optind++], O_RDONLY);
        if (fd < 0 || dup2(fd, STDIN_FILENO) < 0) {
            error(EXIT_FAILURE, errno, "open file error");
        }
        close(fd);
    }
    return 0;
}

static void sig_handler(int signo)
{
    if (signo == SIGQUIT || signo == SIGTERM 
        || signo == SIGINT) {
        for (int i = 0; i < count; ++i) {
            if (pids[i] > 0) {
                kill(pids[i], signo);
            }
        }
        _exit(128 + signo);
    }
}

static void btrace(int signo)
{
    void *bt_buf[BACKTRACE_DEPTH];
    int bt_size;

    bt_size = backtrace(bt_buf, BACKTRACE_DEPTH);
    backtrace_symbols_fd(bt_buf, bt_size, STDERR_FILENO);
    _exit(128 + signo);
}

static void setup_signal(int signo, void (*handler)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(signo, &sa, NULL) < 0) {
        error(EXIT_FAILURE, errno, "sigaction error");
    }
}

int main(int argc, char* argv[])
{
    int rc = EXIT_FAILURE;
    int in = -1;
    int out = -1;
    int threads_started = 0;

    setup_signal(SIGPIPE, SIG_IGN);
    setup_signal(SIGQUIT, sig_handler);
    setup_signal(SIGINT, sig_handler);
    setup_signal(SIGTERM, sig_handler);
    setup_signal(SIGSEGV, btrace);

    getopts(argc, argv);

    if (require_success(set_noblocking(STDIN_FILENO), "failed to set stdin non-blocking") < 0) {
        goto cleanup;
    }
    if (thread_init(count) < 0) {
        goto cleanup;
    }

    for (int i = 0; i < count; ++i) {
        int pid = spawn_process(cmd, &in, &out);
        if (require_success(set_cloexec(out), "failed to set close-on-exec on child stdin") < 0 ||
            require_success(set_cloexec(in), "failed to set close-on-exec on child stdout") < 0 ||
            require_success(set_noblocking(out), "failed to set child stdin non-blocking") < 0 ||
            require_success(set_noblocking(in), "failed to set child stdout non-blocking") < 0 ||
            thread_add(out, in) < 0) {
            if (pid > 0) {
                kill(pid, SIGTERM);
            }
            close_fd_if_open(&out);
            close_fd_if_open(&in);
            goto cleanup;
        }
        pids[i] = pid;
        in = -1;
        out = -1;
    }

    if (thread_start() < 0) {
        goto cleanup;
    }
    threads_started = 1;
    if (wait_children() < 0) {
        goto cleanup;
    }
    thread_term();
    threads_started = 0;
    if (thread_failed()) {
        goto cleanup;
    }
    rc = EXIT_SUCCESS;

cleanup:
    close_fd_if_open(&in);
    close_fd_if_open(&out);
    if (rc != EXIT_SUCCESS) {
        terminate_children(SIGTERM);
        wait_children();
    }
    if (threads_started) {
        thread_term();
    }
    return rc;
}
