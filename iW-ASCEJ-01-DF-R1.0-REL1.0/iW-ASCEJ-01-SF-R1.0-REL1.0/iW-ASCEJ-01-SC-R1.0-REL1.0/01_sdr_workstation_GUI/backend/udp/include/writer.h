/* writer.h - FIFO writer thread entry point. */
#ifndef IWFG_WRITER_H
#define IWFG_WRITER_H

#include "iwfg.h"

struct ring;

struct writer_ctx {
    const struct iwfg_cfg *cfg;
    struct ring *ring;
};

void *writer_thread(void *arg);

#endif
