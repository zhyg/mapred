# Mapred 代码审查修复报告

**日期**: 2026-03-18

本文档记录了对 mapred 项目代码审查中发现的问题及其修复方案。

---

## 问题清单

### 严重问题 (High)

#### 1. `thread_term()` 中缺少对写线程的 `pthread_join` (thread.c)

**问题描述**: `thread_term()` 只 join 了读线程 (`ret->id`)，但没有 join 写线程 (`wet->id`)。在 `wait_children()` 返回后直接释放 `wet` 的内存和 event_base，写线程可能仍在运行，导致 use-after-free 或崩溃。

**修复方案**: 在释放 `wet` 相关资源前，先调用 `pthread_join(wet->id, &retval)`。

#### 2. `thread_term()` 中释放 buffer 使用了结构体拷贝 (thread.c)

**问题描述**:
```c
WEVENT_T e = wet->ev[i];  // 这是值拷贝
dealloc_buffer(&(e.buffer));
```
这里 `dealloc_buffer` 释放的是栈上拷贝的 `e.buffer.ptr`，虽然指针值相同所以内存确实被释放了，但原始 `wet->ev[i].buffer.ptr` 没有被置 NULL，存在 double-free 风险。

**修复方案**: 直接操作原始结构体 `dealloc_buffer(&(wet->ev[i].buffer))`。

#### 3. `btrace()` 信号处理函数中调用了非 async-signal-safe 函数 (main.c)

**问题描述**: `btrace` 作为 `SIGSEGV` 的处理器，调用了 `malloc`、`backtrace_symbols`、`fprintf`(通过 `log` 宏)、`free`——这些都不是 async-signal-safe 的，可能导致死锁或未定义行为。

**修复方案**: 改用 `backtrace_symbols_fd()` 直接写入 STDERR_FILENO，避免内存分配和格式化输出；使用 `_exit()` 替代 `exit()`。

#### 4. `sig_handler` 中调用 `kill()` 后没有退出 (main.c)

**问题描述**: 收到 SIGQUIT/SIGTERM/SIGINT 后转发信号给子进程，但父进程自身没有退出或清理逻辑，可能导致父进程继续运行在不一致的状态。

**修复方案**: 转发信号后调用 `_exit(128 + signo)` 退出父进程。

---

### 中等问题 (Medium)

#### 5. `cmd` 缓冲区溢出风险 (main.c)

**问题描述**:
```c
memcpy(cmd, optarg, strlen(optarg));
```
如果 `optarg` 长度 >= 4096，会发生缓冲区溢出。

**修复方案**: 添加长度检查：
```c
size_t len = strlen(optarg);
if (len >= sizeof(cmd)) {
    fprintf(stderr, "mapper command too long (max %zu)\n", sizeof(cmd) - 1);
    exit(EXIT_FAILURE);
}
```

#### 6. `count` 无上界校验 (main.c)

**问题描述**: `pids` 数组固定 1024 个元素，但 `count` 由用户通过 `-c` 传入且使用 `atoi` 解析（不做错误检查）。如果用户传入 > 1024 或负数，会导致数组越界。

**修复方案**: 使用 `strtol` 解析并校验范围：
```c
char *endptr = NULL;
long val = strtol(optarg, &endptr, 10);
if (*endptr != '\0' || val < 1 || val > MAX_PROCESSES) {
    fprintf(stderr, "invalid count: must be between 1 and %d\n", MAX_PROCESSES);
    exit(EXIT_FAILURE);
}
```

#### 7. `thread_add()` 中使用 static 变量 `i`，非线程安全 (thread.c)

**问题描述**: `thread_add` 使用 `static int i = 0` 作为计数器，虽然当前只在 `main` 中顺序调用没问题，但设计上脆弱，不可重入。

**修复方案**: 改为模块级变量 `static int thread_add_idx = 0`，并在 `thread_term()` 中重置为 0。

#### 8. `expand_buffer` 中的 `capacity` 增长策略不合理 (tbuf.c)

**问题描述**:
```c
int rc = cmalloc((void**)&p, (int)(b->capacity + size));
b->capacity += size;
```
每次只增加 `size` 大小，如果频繁追加小数据，会导致大量重新分配。

**修复方案**: 采用翻倍增长策略，类似 `iostream.c`：
```c
size_t new_cap = b->capacity * 2;
if (new_cap < b->size + size) {
    new_cap = b->size + size;
}
```

#### 9. `posix_memalign` 返回值语义误用 (os.c)

**问题描述**:
```c
rc = posix_memalign(ptr, 16, size);
if (rc < 0 || *ptr == NULL) {
```
`posix_memalign` 成功返回 0，失败返回正的 errno 值（不是 -1）。检查 `rc < 0` 永远不会为真。

**修复方案**: 改为 `if (rc != 0 || *ptr == NULL)`。

#### 10. `last_component` 返回值可能无效 (os.c)

**问题描述**: 如果 `str` 中不含 '/'，`strrchr` 返回 NULL，然后 `++bp` 对 NULL 指针加 1，这是未定义行为。

**修复方案**:
```c
char* bp = strrchr(str, (int)'/');
if (bp == NULL) {
    return (char*)str;
}
return ++bp;
```

---

### 低等问题 (Low)

#### 11. `ALIGN` 宏重复定义 (util.h 和 os.c)

**问题描述**: 在 `util.h` 和 `os.c` 中都定义了 `ALIGN` 宏，可能导致编译警告或定义冲突。

**修复方案**: 移除 `os.c` 中的定义，改为 `#include "util.h"`。

#### 12. iostream.h 的 include guard 名称 `_BUF_H` 过于通用

**问题描述**: 以下划线开头的标识符是保留给实现的，容易与系统头文件冲突。

**修复方案**: 改为 `MAPRED_IOSTREAM_H`；同时修复 tbuf.h 为 `MAPRED_TBUF_H`。

#### 13. `close_stream` 没有将 `fd` 关闭 (iostream.c)

**问题描述**: `IOstream` 结构体有 `fd` 字段，但 `close_stream` 只释放了 buffer 内存，没有关闭文件描述符。调用方需要自行管理 fd 的关闭，容易遗漏。

**修复方案**: 在 `close_stream` 中添加：
```c
if (stream->fd >= 0) {
    close(stream->fd);
    stream->fd = -1;
}
```
同时 `create_stream` 初始化 `fd = -1`。

#### 14. `signal()` 不可靠，建议使用 `sigaction()` (main.c)

**问题描述**: `signal()` 在不同平台上行为不一致（如 handler 是否会被重置为 SIG_DFL），`sigaction()` 更可靠和可移植。

**修复方案**: 实现 `setup_signal()` 辅助函数，使用 `sigaction()` 替代所有 `signal()` 调用。

#### 15. Makefile 编译选项 `-O0` 不适合生产

**问题描述**: 当前使用 `-g -O0`，仅适用于调试。发布时应提供 Release 配置。

**修复方案**: 添加 `release` 构建目标：
```makefile
CFLAGS_DEBUG = $(CFLAGS_COMMON) -g -O0
CFLAGS_RELEASE = $(CFLAGS_COMMON) -O2 -DNDEBUG
release: CFLAGS = $(CFLAGS_RELEASE)
release: mapred
```

---

## 类型安全改进

- `cmalloc()` 的 `size` 参数从 `int` 改为 `size_t`
- `TBuffer` 的 `size` 和 `capacity` 从 `int` 改为 `size_t`
- `cwrite()` 返回值从 `int` 改为 `ssize_t`
- 相关函数签名同步更新

---

## 编译警告消除

- 添加 `-Wextra` 编译选项
- libevent 回调函数的未使用参数添加 `__attribute__((unused))`
- 移除 `btrace()` 中未使用的变量 `i`

---

## 已知限制

测试脚本 `tests/run_tests.sh` 在使用文件重定向作为输入时会失败，这是由于 Linux 的 epoll 不支持监控普通文件描述符。该问题在修复前已存在，属于设计限制而非本次修复引入。使用管道输入可正常工作。
