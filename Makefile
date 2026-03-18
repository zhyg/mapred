# ============================================================
# 编译选项
# ============================================================

# 通用编译标志：启用管道传输、全部警告、GNU 扩展、C99 标准
CFLAGS_COMMON = -pipe -Wall -Wextra -D_GNU_SOURCE -std=gnu99
# 调试模式：附带调试信息，禁用优化
CFLAGS_DEBUG = $(CFLAGS_COMMON) -g -O0
# 发布模式：开启 O2 优化，禁用断言
CFLAGS_RELEASE = $(CFLAGS_COMMON) -O2 -DNDEBUG
# 链接标志：多线程 + libevent + 数学库
LDFLAGS = -pthread -levent -lm

# 默认使用调试模式编译（可通过 make CFLAGS=... 覆盖）
CFLAGS ?= $(CFLAGS_DEBUG)

# ============================================================
# 源文件与目标文件
# ============================================================

SRCDIR = src
# 自动收集 src/ 下所有 .c 源文件
SRCS = $(wildcard $(SRCDIR)/*.c)
# 将 .c 替换为 .o 生成目标文件列表
OBJS = $(SRCS:.c=.o)

# ============================================================
# 构建目标
# ============================================================

# 默认目标：构建 mapred 可执行文件（调试模式）
all: mapred

# 发布构建：使用优化编译选项
release: CFLAGS = $(CFLAGS_RELEASE)
release: mapred

# 链接所有目标文件生成最终可执行文件
mapred: $(OBJS)
	gcc $(CFLAGS) -o mapred $(OBJS) $(LDFLAGS)

# 模式规则：将单个 .c 编译为 .o
$(SRCDIR)/%.o: $(SRCDIR)/%.c
	gcc $(CFLAGS) -c $< -o $@

# ============================================================
# 清理
# ============================================================

.PHONY: all release clean
# 删除可执行文件和所有目标文件
clean:
	rm -f mapred
	rm -f $(SRCDIR)/*.o
