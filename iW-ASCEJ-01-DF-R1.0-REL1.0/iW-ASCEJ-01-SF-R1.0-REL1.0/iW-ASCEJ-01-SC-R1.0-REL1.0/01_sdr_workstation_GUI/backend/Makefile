# ---------------------------------------------------------------------------
# Backend helper applications used by the workstation's Live mode.
#
#   c2h_stream  - C2H capture: DMA from the card into a FIFO the GUI reads.
#   iwfg_h2c    - H2C playback: FIFO -> card (used by the H2C_C2H loopback).
#
# Built exactly as documented in each source file's header comment; the
# sources themselves are used unmodified.
#
#   make            build both into ./bin
#   make clean
# ---------------------------------------------------------------------------

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
BIN      = bin

all: $(BIN)/c2h_stream $(BIN)/iwfg_h2c

$(BIN):
	mkdir -p $(BIN)

# fifo_pipe.c: single-threaded C2H -> FIFO pump.
$(BIN)/c2h_stream: C2H/fifo_pipe.c C2H/iwfg_user.h | $(BIN)
	$(CC) $(CFLAGS) -I C2H -o $@ C2H/fifo_pipe.c

# iwfg_h2c.c documents: gcc -O2 -Wall -Wextra -pthread -o iwfg_h2c iwfg_h2c.c
$(BIN)/iwfg_h2c: H2C/iwfg_h2c.c H2C/iwfg_user.h | $(BIN)
	$(CC) $(CFLAGS) -pthread -I H2C -o $@ H2C/iwfg_h2c.c

clean:
	rm -rf $(BIN)

.PHONY: all clean
