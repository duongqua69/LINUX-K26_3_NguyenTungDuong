#ifndef GATEWAY_H
#define GATEWAY_H

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>
#include <limits.h>

#include "datamgr.h"
#include "sbuffer.h"

/* Main-process context; later steps add the gateway managers and resources. */
typedef struct {
    uint16_t base_port;
    uint16_t http_port;
    volatile sig_atomic_t stop;
    pthread_mutex_t stop_lock;
    int workers_stop; /* Protected by stop_lock. */
    int storage_failed; /* Protected by stop_lock. */
    int connection_failed; /* Protected by stop_lock. */
    int log_fd;
    pthread_mutex_t log_lock;
    pid_t log_pid;
    char log_fifo_path[PATH_MAX];
    char database_path[PATH_MAX];
    sbuffer_t sbuffer;
    datamgr_t datamgr;
} gateway_t;

#endif /* GATEWAY_H */
