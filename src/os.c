#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <string.h>
#include "util.h"

static char* get_shell()
{
    char* shell = getenv("SHELL");
    return shell ? shell : "/bin/sh";
}

static char* last_component(const char* str)
{
    char* bp = strrchr(str, (int)'/');
    if (bp == NULL) {
        return (char*)str;
    }
    return ++bp;
}

int set_noblocking(int fd) 
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        error(EXIT_FAILURE, errno, "fcntl error");
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int set_cloexec(int fd) 
{
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0) {
        error(EXIT_FAILURE, errno, "fcntl error");
    }
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

int cmalloc(void** ptr, size_t size) 
{
    size = ALIGN(size, 16);
    int rc = posix_memalign(ptr, 16, size);
    if (rc != 0 || *ptr == NULL) {
        fprintf(stderr, "alloc error\n");
        return -1;
    }
    return 0;
}

int wait_children()
{
    int status = -1;
    int pid = -1;
    int failed = 0;

    while ( (pid = waitpid(-1, &status, 0)) > 0 ) {
        if (WIFSIGNALED(status)) {
            int signo = WTERMSIG(status);
            fprintf(stderr, "child [%d] exit by signal %d\n", pid, signo);
            failed = 1;
        } else {
            int code = WEXITSTATUS(status);
            fprintf(stderr, "child [%d] exit with status %d\n", pid, code);
            if (code != 0) {
                failed = 1;
            }
        }
    }

    if (pid < 0 && errno != ECHILD) {
        error(EXIT_FAILURE, errno, "waitpid error");
    }

    return failed ? -1 : 0;
}

int spawn_process(const char* cmd, int* in, int* out)
{
    int in_fds[2], out_fds[2];
    if (pipe(in_fds) < 0 || pipe(out_fds) < 0) {
        error(EXIT_FAILURE, errno, "pipe error");
    }

    int pid = fork();
    if (pid < 0) {
        error(EXIT_FAILURE, errno, "fork error");
    }

    if (pid == 0) {
        close(in_fds[0]);
        close(out_fds[1]);
        if (dup2(out_fds[0], STDIN_FILENO) < 0) {
            error(EXIT_FAILURE, errno, "dup2 error");
        }

        if (dup2(in_fds[1], STDOUT_FILENO) < 0) {
            error(EXIT_FAILURE, errno, "dup2 error");
        }

        close(out_fds[0]);
        close(in_fds[1]);
        char* shell = get_shell();
        execl(shell, last_component(shell), "-c", cmd, NULL);
        error(EXIT_FAILURE, errno, "execl error");
    }
    
    close(in_fds[1]);
    close(out_fds[0]);

    *in = in_fds[0];
    *out = out_fds[1];
    return pid;
}
