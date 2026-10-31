CC     ?= gcc
CFLAGS ?= -O3 -march=native -Wall

othello: main.c engine.c engine.h
	$(CC) $(CFLAGS) -o $@ main.c engine.c

clean:
	rm -f othello othello.exe
