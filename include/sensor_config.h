#ifndef SENSOR_CONFIG_H
#define SENSOR_CONFIG_H

/* Number of most recent measurements used to calculate the running average. */
#define RUN_AVG_LENGTH 5

/* Temperature thresholds: cold below TEMP_MIN_C and hot above TEMP_MAX_C. */
#define TEMP_MIN_C 15.0
#define TEMP_MAX_C 20.0

/* Humidity thresholds: dry below HUMIDITY_MIN and humid above HUMIDITY_MAX. */
#define HUMIDITY_MIN 30.0
#define HUMIDITY_MAX 70.0

/* Trigger a smoke/gas alarm when the concentration reaches or exceeds this threshold (ppm). */
#define GAS_ALARM_PPM 300.0

#endif /* SENSOR_CONFIG_H */
