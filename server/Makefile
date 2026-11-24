.PHONY: clean

default: all

all: aesdsocket

aesdsocket: aesdsocket.c
	$(CROSS_COMPILE)$(CC) -o $@ $^

clean:
	rm -f aesdsocket *.o
