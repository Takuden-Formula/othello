CC     ?= gcc
CFLAGS ?= -O3 -march=native -Wall

# PC level (large learned evaluation, ~100 MB of RAM)
othello: main.c engine.c engine.h patterns.h weights.c mpc_sigma.h
	$(CC) $(CFLAGS) -o $@ main.c engine.c weights.c

# microcontroller level on a PC (the same AI as the ESP32 version, for comparison)
othello_mcu: main.c engine.c engine.h patterns.h weights_mcu.c mpc_sigma_mcu.h
	$(CC) $(CFLAGS) -DOTH_PATTERNS=2 -DOTH_TT_BITS=13 -o $@ main.c engine.c weights_mcu.c

clean:
	rm -f othello othello.exe othello_mcu othello_mcu.exe
