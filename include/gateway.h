#ifndef GATEWAY_H
#define GATEWAY_H

#include <signal.h>
#include <stdint.h>

/* Main-process context; later steps add the gateway managers and resources. */
typedef struct {
    uint16_t base_port;
    uint16_t http_port;
    volatile sig_atomic_t stop;
} gateway_t;

#endif /* GATEWAY_H */
