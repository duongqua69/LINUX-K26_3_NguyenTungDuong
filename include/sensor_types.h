#ifndef SENSOR_TYPES_H
#define SENSOR_TYPES_H

#include <stdint.h>
#include <time.h>

/* Sensor ID, unique across the system. */
typedef uint16_t sensor_id_t;

/* Sensor type and the meaning/unit of its measurement value. */
typedef enum {
    SENSOR_TEMPERATURE = 1, /* °C */
    SENSOR_HUMIDITY = 2, /* %RH */
    SENSOR_MOTION = 3, /* 1 = motion detected */
    SENSOR_DOOR = 4, /* 0 = closed, 1 = open */
    SENSOR_SMOKE_GAS = 5 /* ppm */
} sensor_type_t;

/*
 * Normalized measurement data shared by all protocols and stored in each
 * sbuffer element.
 */
typedef struct {
    sensor_id_t id; /* Unique within the system. */
    sensor_type_t type; /* Sensor type. */
    double value; /* In the unit used by this sensor type. */
    time_t ts; /* Measurement time in seconds, UTC. */
} sensor_data_t;

#endif /* SENSOR_TYPES_H */
