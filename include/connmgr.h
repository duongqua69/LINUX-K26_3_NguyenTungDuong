#ifndef CONNMGR_H
#define CONNMGR_H

#include "gateway.h"

void *connection_manager_thread(void *argument);
void *data_manager_thread(void *argument);
void *storage_manager_thread(void *argument);

#endif
