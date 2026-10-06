CC      = gcc
CFLAGS  = -Wall -Wextra -O3 -pthread
LDFLAGS = -pthread -lm

all: iwfg_h2c iwfg_c2h

iwfg_h2c: iwfg_h2c.c iwfg_user.h
	$(CC) $(CFLAGS) -o $@ iwfg_h2c.c $(LDFLAGS)

iwfg_c2h: iwfg_c2h.c iwfg_user.h
	$(CC) $(CFLAGS) -o $@ iwfg_c2h.c $(LDFLAGS)

clean:
	rm -f iwfg_h2c iwfg_c2h
