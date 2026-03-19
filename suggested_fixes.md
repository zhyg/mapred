# 建议的修复方案

## 高优先级修复

### 修复 1: iostream.c - try_read_more 中的错误处理

**当前代码** (src/iostream.c:52-57):
```c
char* new_ptr = (char*)realloc(s->ptr, s->capacity * 2);
if (!new_ptr) {
    error(EXIT_FAILURE, errno, "not enough memory, realloc buf");
}
s->cur = s->ptr = new_ptr;
s->capacity *= 2;
```

**建议修复**:
```c
char* new_ptr = (char*)realloc(s->ptr, s->capacity * 2);
if (!new_ptr) {
    log("realloc failed: %s\n", strerror(errno));
    return E_ERROR;
}
s->cur = s->ptr = new_ptr;
s->capacity *= 2;
```

---

### 修复 2: os.c - set_noblocking 和 set_cloexec

**当前代码** (src/os.c:27-33):
```c
int set_noblocking(int fd) 
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        error(EXIT_FAILURE, errno, "fcntl error");
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
```

**建议修复**:
```c
int set_noblocking(int fd) 
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        log("fcntl(F_GETFL) failed on fd %d: %s\n", fd, strerror(errno));
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        log("fcntl(F_SETFL) failed on fd %d: %s\n", fd, strerror(errno));
        return -1;
    }
    return 0;
}

int set_cloexec(int fd) 
{
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0) {
        log("fcntl(F_GETFD) failed on fd %d: %s\n", fd, strerror(errno));
        return -1;
    }
    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        log("fcntl(F_SETFD) failed on fd %d: %s\n", fd, strerror(errno));
        return -1;
    }
    return 0;
}
```

---

### 修复 3: os.c - wait_children

**当前代码** (src/os.c:56-81):
```c
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
```

**建议修复**:
```c
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
        log("waitpid error: %s\n", strerror(errno));
        return -1;
    }

    return failed ? -1 : 0;
}
```

---

### 修复 4: thread.c - thread_init 内存泄漏

**当前代码** (src/thread.c:210-236):
```c
int thread_init(int num)
{
    if (cmalloc((void**)&wet, (sizeof(WRITE_EV_THREAD) + num * sizeof(WEVENT_T))) < 0 || wet == NULL) {
        error(EXIT_FAILURE, errno, "not enough memory\n");
        return -1;
    } 
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

    if (cmalloc((void**)&ret, (sizeof(READ_EV_THREAD) + num * sizeof(struct fdev_t))) < 0 || ret == NULL) {
        error(EXIT_FAILURE, errno, "not enough memory\n");
        return -1;
    }
    ret->base = event_init();
    ret->evno = num;
    thread_error = 0;
    return 0;
}
```

**建议修复**:
```c
int thread_init(int num)
{
    if (cmalloc((void**)&wet, (sizeof(WRITE_EV_THREAD) + num * sizeof(WEVENT_T))) < 0 || wet == NULL) {
        log("failed to allocate WRITE_EV_THREAD: %s\n", strerror(errno));
        return -1;
    } 
    
    wet->base = event_init();
    if (!wet->base) {
        log("failed to initialize write event base\n");
        free(wet);
        wet = NULL;
        return -1;
    }
    
    wet->evno = num;
    wet->next_child = 0;
    wet->stdin_eof = 0;
    wet->stdin_paused = 0;
    memset(&wet->ev, 0, num * sizeof(WEVENT_T));
    
    if (create_stream(&wet->stream, 4096 * 1024) < 0) {
        log("failed to create stdin stream\n");
        event_base_free(wet->base);
        free(wet);
        wet = NULL;
        return -1;
    }
    
    wet->stream.fd = STDIN_FILENO;
    event_set(&wet->stdin_ev, STDIN_FILENO, EV_READ | EV_PERSIST, stdin_handler, wet);
    event_base_set(wet->base, &wet->stdin_ev);
    event_add(&wet->stdin_ev, 0);

    if (cmalloc((void**)&ret, (sizeof(READ_EV_THREAD) + num * sizeof(struct fdev_t))) < 0 || ret == NULL) {
        log("failed to allocate READ_EV_THREAD: %s\n", strerror(errno));
        // 清理已分配的资源
        close_stream(&wet->stream);
        event_base_free(wet->base);
        free(wet);
        wet = NULL;
        return -1;
    }
    
    ret->base = event_init();
    if (!ret->base) {
        log("failed to initialize read event base\n");
        free(ret);
        ret = NULL;
        close_stream(&wet->stream);
        event_base_free(wet->base);
        free(wet);
        wet = NULL;
        return -1;
    }
    
    ret->evno = num;
    thread_error = 0;
    return 0;
}
```

---

### 修复 5: tbuf.c - 内存分配错误处理

**当前代码** (src/tbuf.c:11-27):
```c
int alloc_buffer(TBuffer* b, size_t alloc)
{
    if (alloc == 0) {
        return -1;
    }

    int rc = cmalloc((void**)&(b->ptr), alloc);
    if (rc < 0 || !b->ptr) {
        error(EXIT_FAILURE, errno, "not enough memory!\n");
        return -1;
    }

    b->cur = b->ptr;
    b->size = 0;
    b->capacity = alloc;
    return 0;
}
```

**建议修复**:
```c
int alloc_buffer(TBuffer* b, size_t alloc)
{
    if (alloc == 0) {
        return -1;
    }

    int rc = cmalloc((void**)&(b->ptr), alloc);
    if (rc < 0 || !b->ptr) {
        log("buffer allocation failed: %s\n", strerror(errno));
        return -1;
    }

    b->cur = b->ptr;
    b->size = 0;
    b->capacity = alloc;
    return 0;
}
```

**expand_buffer 同样修复** (src/tbuf.c:45-49):
```c
char* p = NULL;
int rc = cmalloc((void**)&p, new_cap);
if (p == NULL || rc < 0) {
    log("buffer expansion failed: %s\n", strerror(errno));
    return -1;
}
```

---

### 修复 6: main.c - 僵尸进程问题

**当前代码** (src/main.c:202-207):
```c
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
```

**建议修复**:
```c
if (require_success(set_cloexec(out), "failed to set close-on-exec on child stdin") < 0 ||
    require_success(set_cloexec(in), "failed to set close-on-exec on child stdout") < 0 ||
    require_success(set_noblocking(out), "failed to set child stdin non-blocking") < 0 ||
    require_success(set_noblocking(in), "failed to set child stdout non-blocking") < 0 ||
    thread_add(out, in) < 0) {
    if (pid > 0) {
        kill(pid, SIGTERM);
        // 等待子进程退出以避免僵尸进程
        int status;
        waitpid(pid, &status, 0);
    }
    close_fd_if_open(&out);
    close_fd_if_open(&in);
    goto cleanup;
}
```

---

## 中优先级修复

### 修复 7: thread.c - 线程安全问题

**方案 A: 使用原子操作**

在文件头部添加：
```c
#include <stdatomic.h>
```

修改全局变量：
```c
static _Atomic int thread_error = 0;
```

**方案 B: 每个线程维护自己的错误标志**

修改结构体：
```c
typedef struct {
    pthread_t id;
    struct event_base* base;
    IOstream stream;
    struct event stdin_ev;
    int evno;
    int next_child;
    int stdin_eof;
    int stdin_paused;
    int error_occurred;  // 新增
    WEVENT_T ev[];
} WRITE_EV_THREAD;

typedef struct {
    pthread_t id;
    struct event_base* base;
    int evno;
    int error_occurred;  // 新增
    struct fdev_t {
        int fd;
        struct event ev;
        IOstream stream;
    } evs[];
} READ_EV_THREAD;
```

修改 `thread_failed()`：
```c
int thread_failed() {
    return (wet && wet->error_occurred) || (ret && ret->error_occurred);
}
```

在各个事件处理器中：
```c
void stdin_handler(...) {
    // ...
    if (r == E_ERROR) {
        wet->error_occurred = 1;  // 而不是 thread_error = 1
        // ...
    }
}
```

---

### 修复 8: iostream.c - 整数溢出保护

**当前代码** (src/iostream.c:86-92):
```c
int get_line(IOstream* s, char** line, int* len)
{
    char* bp = (char*)memchr(s->cur, '\n', s->bytes);
    if (bp == NULL) {
        return E_NEED_MORE;
    }
    
    *line = s->cur;
    *len = (int)(++bp - s->cur);
    s->cur = bp;
    s->bytes -= *len;
    return E_OK;
}
```

**建议修复**:
```c
int get_line(IOstream* s, char** line, int* len)
{
    char* bp = (char*)memchr(s->cur, '\n', s->bytes);
    if (bp == NULL) {
        return E_NEED_MORE;
    }
    
    ++bp;
    size_t line_len = (size_t)(bp - s->cur);
    
    // 检查是否超过 int 范围
    if (line_len > INT_MAX) {
        log("line too long: %zu bytes\n", line_len);
        return E_ERROR;
    }
    
    // 检查是否会导致下溢
    if (s->bytes < line_len) {
        log("internal error: bytes=%zu < line_len=%zu\n", s->bytes, line_len);
        return E_ERROR;
    }
    
    *line = s->cur;
    *len = (int)line_len;
    s->cur = bp;
    s->bytes -= line_len;
    return E_OK;
}
```

同样修复 `get_all_lines`。

---

### 修复 9: thread.c - 检查 pthread_join 返回值

**当前代码** (src/thread.c:294-315):
```c
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
```

**建议修复**:
```c
int thread_term()
{
    void* retval = NULL;
    int rc;

    rc = pthread_join(wet->id, &retval);
    if (rc != 0) {
        log("pthread_join(wet) error: %s\n", strerror(rc));
    }
    
    for (int i = 0; i < wet->evno; ++i) {
        dealloc_buffer(&(wet->ev[i].buffer));
    }
    event_base_free(wet->base);
    close_stream(&wet->stream);
    free(wet);
    
    rc = pthread_join(ret->id, &retval);
    if (rc != 0) {
        log("pthread_join(ret) error: %s\n", strerror(rc));
    }
    
    event_base_free(ret->base);
    for (int i = 0; i < ret->evno; ++i) {
        close_stream(&ret->evs[i].stream);
    }
    free(ret);

    thread_add_idx = 0;
    return 0;
}
```

---

## 应用修复后的测试建议

修复后应运行以下测试：

1. **现有测试**:
```bash
EVENT_NOEPOLL=1 bash tests/run_tests.sh
```

2. **子进程失败测试**:
```bash
echo "test" | EVENT_NOEPOLL=1 ./mapred -m "exit 1" -c 2
```

3. **无效命令测试**:
```bash
echo "test" | EVENT_NOEPOLL=1 ./mapred -m "nonexistent_cmd" -c 2
```

4. **大量数据测试**:
```bash
seq 1 100000 | EVENT_NOEPOLL=1 ./mapred -m "wc -l" -c 4
```

5. **EOF 测试**:
```bash
printf "no newline" | EVENT_NOEPOLL=1 ./mapred -m "cat" -c 2
```

6. **内存泄漏检测** (如果有 valgrind):
```bash
echo "test" | valgrind --leak-check=full ./mapred -m "cat" -c 2
```

---

## 总结

应用这些修复后，代码将：
1. ✅ 不在运行时错误时退出整个程序
2. ✅ 正确清理资源，避免内存泄漏
3. ✅ 避免僵尸进程
4. ✅ 线程安全
5. ✅ 更健壮的边界检查

这些修复完全符合 PR 的设计目标。
