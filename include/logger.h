#ifndef LOGGER_H
#define LOGGER_H

#include "gateway.h"

int logger_start(gateway_t *gateway);
int logger_write(gateway_t *gateway, const char *source, const char *message);
int logger_stop(gateway_t *gateway);

#endif /* LOGGER_H */
