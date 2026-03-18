CFLAGS_COMMON = -pipe -Wall -Wextra -D_GNU_SOURCE -std=gnu99
CFLAGS_DEBUG = $(CFLAGS_COMMON) -g -O0
CFLAGS_RELEASE = $(CFLAGS_COMMON) -O2 -DNDEBUG
LDFLAGS = -pthread -levent -lm

CFLAGS ?= $(CFLAGS_DEBUG)

SRCS = main.c iostream.c os.c thread.c tbuf.c
OBJS = $(SRCS:.c=.o)

all: mapred

release: CFLAGS = $(CFLAGS_RELEASE)
release: mapred

mapred: $(OBJS)
	gcc $(CFLAGS) -o mapred $(OBJS) $(LDFLAGS)

%.o:%.c
	gcc $(CFLAGS) -c $< -o $@

.PHONY: all release clean
clean:
	rm -f mapred
	rm -f *.o
