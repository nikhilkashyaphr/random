# Host PCIe control tool for the ZU47DR RFSoC
#
#   make            build ./rfdc_ctl
#   make clean
#   sudo make install    -> /usr/local/bin
#
# No external dependencies: POSIX + libc only.

CC      ?= gcc
CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Wpedantic -Wshadow \
           -Wconversion -Wno-sign-conversion -D_GNU_SOURCE
LDFLAGS ?=
PREFIX  ?= /usr/local

TARGET  := rfdc_ctl
SRCS    := rfdc_ctl.c pcie_access.c
OBJS    := $(SRCS:.c=.o)
DEPS    := pcie_access.h pcie_regs.h

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
