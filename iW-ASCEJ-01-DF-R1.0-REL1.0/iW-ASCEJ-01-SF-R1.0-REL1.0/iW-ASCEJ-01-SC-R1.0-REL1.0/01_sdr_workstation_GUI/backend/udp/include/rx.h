/* rx.h - RX thread entry point. */
#ifndef IWFG_RX_H
#define IWFG_RX_H

#include "iwfg.h"

struct ring;

struct rx_ctx {
    const struct iwfg_cfg *cfg;
    struct ring *ring;
    int sock_fd;              /* set by the thread once open */
};

void *rx_thread(void *arg);

#endif
