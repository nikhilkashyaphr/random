#ifndef XIL_IO_H
#define XIL_IO_H
#include "xil_types.h"
/* The "BAR" is a shared file mapping; see emu_test.cpp. */
static inline u32  Xil_In32(UINTPTR a)         { return *(volatile u32 *)a; }
static inline void Xil_Out32(UINTPTR a, u32 v) { *(volatile u32 *)a = v; __sync_synchronize(); }
#endif
