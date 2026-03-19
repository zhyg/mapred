# 详细代码分析

## error(EXIT_FAILURE) 调用分析

在整个代码库中找到 16 处 `error(EXIT_FAILURE, ...)` 调用。分类如下：

### ✅ 合理的调用（在子进程或初始化阶段）

| 文件 | 行号 | 函数 | 上下文 | 合理性 |
|------|------|------|--------|--------|
| src/os.c | 87 | `spawn_process` | `pipe()` 失败 | ✅ 初始化阶段，合理 |
| src/os.c | 92 | `spawn_process` | `fork()` 失败 | ✅ 初始化阶段，合理 |
| src/os.c | 99 | `spawn_process` | `dup2()` 失败（子进程中） | ✅ 子进程可以退出 |
| src/os.c | 103 | `spawn_process` | `dup2()` 失败（子进程中） | ✅ 子进程可以退出 |
| src/os.c | 109 | `spawn_process` | `execl()` 失败（子进程中） | ✅ 子进程可以退出 |
| src/main.c | 131 | `getopts` | 打开输入文件失败 | ✅ 启动阶段，合理 |
| src/main.c | 169 | `setup_signal` | `sigaction()` 失败 | ✅ 启动阶段，合理 |

### ❌ 有问题的调用（与 PR 目标冲突）

| 文件 | 行号 | 函数 | 问题 | 建议修复 |
|------|------|------|------|----------|
| src/os.c | 31 | `set_noblocking` | `fcntl(F_GETFL)` 失败时退出 | 返回 -1 |
| src/os.c | 40 | `set_cloexec` | `fcntl(F_GETFD)` 失败时退出 | 返回 -1 |
| src/os.c | 77 | `wait_children` | `waitpid()` 失败时退出 | 记录错误，返回 -1 |
| src/iostream.c | 54 | `try_read_more` | `realloc()` 失败时退出 | 返回 E_ERROR |
| src/thread.c | 211 | `thread_init` | `cmalloc()` 失败时退出 | 返回 -1 |
| src/thread.c | 229 | `thread_init` | `cmalloc()` 失败时退出 | 清理 wet，返回 -1 |
| src/tbuf.c | 19 | `alloc_buffer` | `cmalloc()` 失败时退出 | 返回 -1 |
| src/tbuf.c | 47 | `expand_buffer` | `cmalloc()` 失败时退出 | 返回 -1 |

---

## 调用链分析

### 场景 1: 运行时错误路径（需要修复）

```
stdin_handler (事件循环中)
  └─> try_read_more
      └─> realloc 失败
          └─> error(EXIT_FAILURE) ❌ 整个程序退出
```

**影响**: 如果输入数据导致缓冲区需要扩展但系统内存不足，整个 `mapred` 进程会退出，而不是优雅地处理错误。

### 场景 2: 初始化失败路径（需要修复）

```
main
  └─> thread_init
      └─> cmalloc (第一次) 成功
      └─> cmalloc (第二次) 失败
          └─> error(EXIT_FAILURE) ❌ 退出，且泄漏第一次分配的内存
```

**影响**: 
1. 内存泄漏
2. 不符合"返回错误让 main 处理"的设计模式

### 场景 3: 设置文件描述符失败（需要修复）

```
main
  └─> spawn_process 成功
  └─> set_noblocking 失败
      └─> fcntl 失败
          └─> error(EXIT_FAILURE) ❌ 整个程序退出
```

**影响**: 虽然 `main.c` 使用 `require_success` 检查返回值，但 `set_noblocking` 在失败前就已经调用了 `error(EXIT_FAILURE)`。

---

## 资源泄漏路径分析

### 泄漏路径 1: thread_init 部分失败

**代码流程**:
```c
// 第一次分配成功
cmalloc((void**)&wet, ...) // ✓ 成功
wet->base = event_init();   // ✓ 成功
create_stream(&wet->stream, 4096 * 1024); // ✓ 成功

// 第二次分配失败
cmalloc((void**)&ret, ...) // ✗ 失败
error(EXIT_FAILURE, ...)   // 程序退出
```

**泄漏资源**:
- `wet` 指向的内存
- `wet->base` (event_base)
- `wet->stream.ptr` (malloc 的缓冲区)

### 泄漏路径 2: main 中 spawn_process 后失败

**代码流程**:
```c
int pid = spawn_process(cmd, &in, &out); // 子进程已创建
if (set_cloexec(out) < 0) { // fcntl 内部调用 error(EXIT_FAILURE)
    // 这里不会到达
    kill(pid, SIGTERM);
}
```

**问题**: 如果 `fcntl` 失败，程序直接退出，子进程变成孤儿进程（会被 init 收养，但不是预期行为）。

---

## 线程安全详细分析

### thread_error 的竞态条件

**写入位置**:
1. `event_handler` (wet 线程) - line 95
2. `stdin_handler` (wet 线程) - line 136
3. `revent_handler` (ret 线程) - line 184, 201

**读取位置**:
1. `main` - line 223

**时序问题示例**:
```
时间 T1: wet 线程检测到错误，准备写 thread_error = 1
时间 T2: ret 线程检测到错误，准备写 thread_error = 1
时间 T3: 两个线程同时写入（虽然都写 1，但理论上存在竞态）
时间 T4: main 读取 thread_error
```

**当前影响**: 由于只写入常量 1，实际影响较小，但不符合线程安全最佳实践。

**正确做法**:
```c
#include <stdatomic.h>
static _Atomic int thread_error = 0;
```

或者：
```c
// 在 WRITE_EV_THREAD 和 READ_EV_THREAD 中各维护一个错误标志
typedef struct {
    pthread_t id;
    struct event_base* base;
    int error_flag;  // 各自维护
    // ...
} WRITE_EV_THREAD;

// thread_failed() 检查两个标志
int thread_failed() {
    return (wet && wet->error_flag) || (ret && ret->error_flag);
}
```

---

## 僵尸进程详细分析

**问题代码** (main.c:202-207):
```c
for (int i = 0; i < count; ++i) {
    int pid = spawn_process(cmd, &in, &out);
    if (require_success(set_cloexec(out), ...) < 0 ||
        require_success(set_cloexec(in), ...) < 0 ||
        require_success(set_noblocking(out), ...) < 0 ||
        require_success(set_noblocking(in), ...) < 0 ||
        thread_add(out, in) < 0) {
        if (pid > 0) {
            kill(pid, SIGTERM);  // ❌ 没有 waitpid
        }
        close_fd_if_open(&out);
        close_fd_if_open(&in);
        goto cleanup;
    }
    pids[i] = pid;
    // ...
}
```

**问题场景**:
1. 第 3 个子进程创建成功
2. `set_noblocking` 失败
3. 发送 `SIGTERM` 给第 3 个子进程
4. 跳转到 cleanup，没有 `waitpid`
5. 第 3 个子进程变成僵尸进程（直到 `mapred` 退出）

**修复建议**:
```c
if (pid > 0) {
    kill(pid, SIGTERM);
    int status;
    if (waitpid(pid, &status, WNOHANG) == 0) {
        // 如果子进程还没退出，等待一小段时间
        usleep(10000); // 10ms
        waitpid(pid, &status, WNOHANG);
    }
}
```

或者更简单：
```c
if (pid > 0) {
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0); // 阻塞等待
}
```

---

## 整数溢出详细分析

**问题代码** (iostream.c:88):
```c
int get_line(IOstream* s, char** line, int* len) {
    char* bp = (char*)memchr(s->cur, '\n', s->bytes);
    if (bp == NULL) {
        return E_NEED_MORE;
    }
    *line = s->cur;
    *len = (int)(++bp - s->cur);  // ❌ 潜在溢出
    s->cur = bp;
    s->bytes -= *len;              // ❌ 类型不匹配
    return E_OK;
}
```

**问题分析**:
1. `s->bytes` 是 `size_t` (通常 64 位)
2. `*len` 是 `int` (通常 32 位)
3. 如果一行超过 2GB，转换会溢出

**实际风险评估**:
- 在 `try_read_more` 中有 64MB 限制 (line 48-51)
- 所以单行不会超过 64MB，溢出风险较低
- 但仍然不够健壮

**建议修复**:
```c
size_t line_len = (size_t)(bp - s->cur);
if (line_len > INT_MAX) {
    log("line too long: %zu bytes\n", line_len);
    return E_ERROR;
}
*len = (int)line_len;
if (s->bytes < line_len) {
    log("internal error: bytes underflow\n");
    return E_ERROR;
}
s->bytes -= line_len;
```

---

## 测试覆盖率分析

### 当前测试覆盖

✅ **已覆盖**:
- 正常的 grep 场景
- 正常的 wc 场景
- 子进程正常退出

❌ **未覆盖**:
- 子进程异常退出（已在我的测试中验证）
- 子进程被信号杀死
- 内存分配失败
- 文件描述符操作失败
- 超大输入（接近 64MB 限制）
- EOF 处理
- 部分行（没有换行符）

### 建议的额外测试

1. **子进程失败测试** (已通过):
```bash
echo "test" | ./mapred -m "exit 1" -c 2
```

2. **超大行测试**:
```bash
python3 -c "print('x' * 10000000)" | ./mapred -m "wc -c" -c 2
```

3. **EOF 处理测试**:
```bash
printf "no newline at end" | ./mapred -m "cat" -c 2
```

4. **信号测试**:
```bash
# 启动 mapred 并在运行时发送 SIGTERM
```

---

## 代码质量观察

### 优点
1. ✅ 使用 `goto cleanup` 模式统一错误处理
2. ✅ 辅助函数 `close_fd_if_open`, `require_success` 提高可读性
3. ✅ 事件驱动架构使用 libevent，高效
4. ✅ 流控制机制（stdin_paused）防止内存过度使用
5. ✅ 编译时无警告

### 改进空间
1. ❌ 错误处理不一致（部分 exit，部分返回错误）
2. ❌ 缺少单元测试
3. ❌ 缺少内存泄漏检测
4. ❌ 线程安全问题
5. ❌ 缺少详细的错误日志（某些错误路径）

---

## 总结

这个 PR 在正确的方向上迈出了重要一步，但仍有关键问题需要解决才能完全实现其目标。建议在合并前至少解决标记为"高优先级"的问题。
