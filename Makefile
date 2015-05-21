all: mapred

mapred: main.o iostream.o os.o thread.o tbuf.o
	gcc -g -O0 -o mapred *.o -pthread libev.a -lm

%.o:%.c
	gcc -pipe -Wall -I./libev -D_GNU_SOURCE -g -O0 -std=gnu99 -c $< -o $@ 

.PHONY:clean
clean:
	rm -rf mapred
	rm -rf *.o
