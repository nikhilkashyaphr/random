/* stats.h - statistics reporter thread entry point. */
#ifndef IWFG_STATS_H
#define IWFG_STATS_H

#include "iwfg.h"

struct ring;

struct stats_ctx {
    const struct iwfg_cfg *cfg;
    struct ring *ring;
};

void *stats_thread(void *arg);
void  stats_final(const struct ring *ring);

#endif
