# i225nvm - native NVM tool for Intel i225/i226 on ARM (Raspberry Pi) / x86.
#
# Native build on the Pi:      make
# Cross-compile from x86 host:  make CROSS=aarch64-linux-gnu-

CROSS   ?=
CC      := $(CROSS)gcc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wshadow -D_GNU_SOURCE
LDFLAGS ?=

SRC := src/pci.c src/nvm.c src/flash.c src/image.c src/main.c
OBJ := $(SRC:.c=.o)
BIN := i225nvm

.PHONY: all clean
all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)
