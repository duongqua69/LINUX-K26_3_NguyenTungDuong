#ifndef GATEWAY_H
#define GATEWAY_H

#include <pthread.h>
#include <signal.h>
#include <stdint.h>

/* Main-process context; later steps add the gateway managers and resources. */
typedef struct {
    uint16_t base_port;
    uint16_t http_port;
    volatile sig_atomic_t stop;
    pthread_mutex_t stop_lock;
    pthread_cond_t stop_cond;
    int workers_stop; /* Protected by stop_lock. */
} gateway_t;

#endif /* GATEWAY_H */
