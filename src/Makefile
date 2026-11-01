CC     ?= gcc
CFLAGS ?= -O3 -march=native -Wall

othello: main.c engine.c engine.h patterns.h weights.c
	$(CC) $(CFLAGS) -o $@ main.c engine.c weights.c

clean:
	rm -f othello othello.exe
