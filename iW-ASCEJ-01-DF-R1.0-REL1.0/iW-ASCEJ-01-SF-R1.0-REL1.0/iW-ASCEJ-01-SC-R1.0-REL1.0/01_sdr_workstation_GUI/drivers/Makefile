# ===========================================================================
# iwfg out-of-tree kernel module.
#
# Builds with a single `make` (or `make all`) and produces iwfg.ko.
#
# Two-pass build model
# --------------------
# `make` here runs THIS file with the top target `all`, which invokes the
# kernel build system:  make -C $(KDIR) M=<here> modules.  The kernel build
# system then RE-READS this same file, but now with its working directory set
# to the kernel tree ($(CURDIR)/$(PWD) point at /usr/src/linux-headers-...),
# not the driver directory.  Only $(src) — which kbuild sets to the module
# source directory — is correct during that second pass.
#
# The previous Makefile computed its object list from $(PWD), so during the
# kbuild pass the wildcard matched nothing, iwfg-objs came out empty, and the
# build died with "No rule to make target iwfg.o".  A manual terminal `make`
# happened to work only because an interactive shell exports PWD pointing at
# the driver dir; a build spawned by the application (QProcess) inherits a
# different PWD and failed.  Using $(src) fixes both invocations.
# ===========================================================================

ifdef KVERSION
KERNEL_VERS = $(KVERSION)
else
KERNEL_VERS = $(shell uname -r)
endif

KDIR ?= /lib/modules/$(KERNEL_VERS)/build

# --- module object list ----------------------------------------------------
# $(src) is the driver directory in BOTH passes: it equals "." for the top
# make and the absolute module path for the kbuild pass.  Building the list
# from it (rather than $(PWD)) is what makes the module link in every context.
obj-m += iwfg.o

KERNEL_SOURCES := $(wildcard $(src)/iwfg_*.c $(src)/qdma_access/*.c)
iwfg-objs      := $(patsubst $(src)/%.c,%.o,$(KERNEL_SOURCES))

# Include paths and warnings.  -Werror kept off by default so an unrelated
# kernel/compiler-version warning cannot break a customer build; enable it
# with `make WERROR=1` during development.
ccflags-y := -O2 -Wall -I$(src)/qdma_access -I$(src)
ifdef WERROR
ccflags-y += -Werror
endif

ifdef DEBUG
ccflags-y += -DDEBUG -g -O1
$(info Building with DEBUG enabled)
endif

# --- targets ---------------------------------------------------------------
# $(CURDIR) (not $(PWD)) is make's own reliable working directory for the
# TOP-level invocation, which is where these recipe lines run.
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

debug:
	$(MAKE) DEBUG=1 -C $(KDIR) M=$(CURDIR) modules

with-clang:
	$(MAKE) CC=clang -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f *.o.ur-safe ./qdma_access/*.o.ur-safe

install:
	rm -f /lib/modules/$(KERNEL_VERS)/iwfg.ko
	cp iwfg.ko /lib/modules/$(KERNEL_VERS)
	depmod

uninstall:
	rm -f /lib/modules/$(KERNEL_VERS)/iwfg.ko
	depmod

.PHONY: all debug with-clang clean install uninstall
