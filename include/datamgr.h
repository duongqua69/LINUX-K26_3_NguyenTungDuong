#ifndef DATAMGR_H
#define DATAMGR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensor_config.h"
#include "sensor_types.h"

#define MAX_SENSORS 64

typedef enum {
    STATE_COLLECTING,
    STATE_NORMAL,
    STATE_ALERT
} sensor_state_t;

typedef struct {
    uint16_t room;
    sensor_id_t id;
    sensor_type_t type;
} sensor_info_t;

typedef struct {
    sensor_info_t info;
    double window[RUN_AVG_LENGTH];
    size_t count;
    size_t next;
    double running_avg;
    sensor_state_t state;
    int alert_kind;
    int has_door_state;
    int door_state;
} datamgr_sensor_t;

typedef struct {
    datamgr_sensor_t sensors[MAX_SENSORS];
    size_t n;
} datamgr_t;

typedef struct {
    bool should_log;
    char message[200];
    uint16_t room;
    double running_avg;
    bool has_running_avg;
} datamgr_event_t;

/* Load the room, sensor ID, and type catalog into memory. */
int datamgr_load(datamgr_t *manager, const char *path);
/* Validate one measurement and run its type-specific handler. */
int datamgr_process(datamgr_t *manager, const sensor_data_t *data,
                    datamgr_event_t *event);

#endif /* DATAMGR_H */
