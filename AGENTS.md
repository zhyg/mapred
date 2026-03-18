# AGENTS.md

## Cursor Cloud specific instructions

### Overview

`mapred` is a standalone C CLI tool that parallelizes text-stream processing using a MapReduce-style approach. It reads lines from stdin (or a file), distributes them across child processes running a user-specified shell command, and merges their output to stdout.

### Build

```
make          # debug build (default)
make release  # optimized build
make clean    # remove artifacts
```

### Test

```
EVENT_NOEPOLL=1 bash tests/run_tests.sh
```

### Important: `EVENT_NOEPOLL=1` required in Cloud Agent VMs

The Cloud Agent VM kernel does not fully support epoll on regular file descriptors (a known Linux limitation). When `mapred` opens a file argument and dup2's it to stdin, libevent's epoll backend fails with `Epoll ADD on fd 0 failed: Operation not permitted` and the process hangs.

**Workaround:** Prefix all `mapred` invocations that use file arguments with `EVENT_NOEPOLL=1` to force libevent to use the `poll` backend instead. Pipe-based input (`cat file | ./mapred ...`) works without this workaround since pipes are epoll-compatible.

### Dependencies

- **System:** `gcc`, `make`, `libevent-dev` (installed via `apt-get`)
- **No runtime services** (databases, containers, etc.) are required.
