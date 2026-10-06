/* util.c - logging, CPU pinning, clocks. */
#include "iwfg.h"

#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

void iwfg_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    fprintf(stderr, "[%02d:%02d:%02d.%03ld] %s\n",
            tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000, buf);
}

void iwfg_fatal(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "FATAL: %s\n", buf);
    exit(EXIT_FAILURE);
}

int iwfg_pin_thread(int cpu, const char *name)
{
    if (name)
        (void)pthread_setname_np(pthread_self(), name);
    if (cpu < 0)
        return 0;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET((unsigned)cpu, &set);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (rc != 0) {
        iwfg_log("warn: pinning %s to CPU %d failed: %s",
                 name ? name : "thread", cpu, strerror(rc));
        return -rc;
    }
    iwfg_log("%s pinned to CPU %d", name ? name : "thread", cpu);
    return 0;
}

uint64_t iwfg_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
