# iwfg_c2h - UDP payload -> FIFO streamer
CC      ?= gcc
CSTD     = -std=c11
OPT      = -O3 -march=native -flto
WARN     = -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion
DEFS     = -D_GNU_SOURCE
CFLAGS  ?= $(CSTD) $(OPT) $(WARN) $(DEFS) -Iinclude -pthread
LDFLAGS ?= -pthread -flto

SRC  = src/main.c src/rx.c src/writer.c src/stats.c src/ring.c src/util.c
OBJ  = $(SRC:.c=.o)
BIN  = iwfg_c2h

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) -o $@

%.o: %.c include/*.h
	$(CC) $(CFLAGS) -c $< -o $@

debug: OPT = -O1 -g -fsanitize=thread
debug: LDFLAGS += -fsanitize=thread
debug: clean $(BIN)

asan: OPT = -O1 -g -fsanitize=address,undefined
asan: LDFLAGS += -fsanitize=address,undefined
asan: clean $(BIN)

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean debug asan
