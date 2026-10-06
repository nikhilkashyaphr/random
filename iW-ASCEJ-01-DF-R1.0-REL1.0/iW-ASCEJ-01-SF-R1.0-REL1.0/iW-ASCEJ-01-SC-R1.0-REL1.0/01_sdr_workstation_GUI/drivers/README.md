# drivers/ — iwfg PCIe driver

Builds with a single command:

```bash
cd drivers
make          # or: make all
```

producing `iwfg.ko`. No manual edits, no arguments required (the running
kernel is auto-detected; override with `make KVERSION=<ver>` if cross-building).

## Build fix (why it failed before)

The original Makefile computed its object list from `$(PWD)`:

```make
srcdir = $(PWD)
KERNEL_SOURCES := $(wildcard $(srcdir)/*.c ...)
```

An out-of-tree kernel module is built in **two passes**. `make` here runs the
top target, which calls `make -C $(KDIR) M=<dir> modules`; the kernel build
system then **re-reads this same Makefile with its working directory set to
the kernel tree**. During that second pass `$(PWD)` (and `$(CURDIR)`) point at
`/usr/src/linux-headers-.../`, not the driver directory, so the wildcard
matched nothing, `iwfg-objs` was empty, and the build died with:

```
No rule to make target '.../iwfg.o', needed by '.../'.  Stop.
```

A manual terminal `make` sometimes worked only because an interactive shell
exports `PWD` pointing at the driver dir. A build **spawned by the
application** (QProcess) inherits a different `PWD` and always failed — which
is exactly what you saw.

**Fix:** the object list is now built from `$(src)`, the variable kbuild sets
to the module source directory and which is correct in *both* passes. Recipe
lines use `$(CURDIR)` (make's reliable working directory) instead of `$(PWD)`.
The application additionally sets `PWD` for the make subprocess as a
belt-and-suspenders measure, so even a `$(PWD)`-based Makefile would work.

Verified: single-command `make` and `make all` both produce `iwfg.ko` from a
fresh extraction, with zero warnings (even under `-Werror`), on kernel 6.8.
`make clean` removes all artifacts. Module metadata is intact
(`license=Dual BSD/GPL`, so no kernel taint).

## Targets

`make` / `make all` · `make clean` · `make install` (copies to
`/lib/modules/$(uname -r)` + depmod) · `make uninstall` · `make debug`
(adds `-DDEBUG -g`) · `make WERROR=1` (treat warnings as errors, for dev).

## Application integration

On PCIe launch the app checks whether `iwfg` is loaded; if not, it prefers an
existing `iwfg.ko` (no rebuild — your validated binary is authoritative) and
only builds from source when none is present. Insertion is `pkexec insmod`;
on exit the module is removed with `rmmod` **only if this session loaded it**.
No Xilinx memory-controller node is created or expected.
