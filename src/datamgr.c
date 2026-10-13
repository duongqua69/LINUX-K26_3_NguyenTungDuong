#define _POSIX_C_SOURCE 200809L

#include "datamgr.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*sensor_handler_t)(datamgr_sensor_t *, const sensor_data_t *,
                                 datamgr_event_t *);

enum {
    ALERT_NONE,
    ALERT_TOO_COLD,
    ALERT_TOO_HOT,
    ALERT_TOO_DRY,
    ALERT_TOO_HUMID,
    ALERT_GAS
};

static int parse_u16(const char *text, uint16_t *value)
{
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL || text[0] == '\0' || text[0] == '-' || text[0] == '+') {
        return -1;
    }
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0 ||
        parsed > UINT16_MAX) {
        return -1;
    }
    *value = (uint16_t)parsed;
    return 0;
}

static int parse_sensor_type(const char *text, sensor_type_t *type)
{
    static const struct {
        const char *name;
        sensor_type_t type;
    } types[] = {
        { "temperature", SENSOR_TEMPERATURE },
        { "humidity", SENSOR_HUMIDITY },
        { "motion", SENSOR_MOTION },
        { "door", SENSOR_DOOR },
        { "smoke_gas", SENSOR_SMOKE_GAS }
    };
    size_t i;

    for (i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        if (strcmp(text, types[i].name) == 0) {
            *type = types[i].type;
            return 0;
        }
    }
    return -1;
}

int datamgr_load(datamgr_t *manager, const char *path)
{
    FILE *file;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t line_length;
    int status = 0;

    if (manager == NULL || path == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(manager, 0, sizeof(*manager));
    file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }
    for (;;) {
        char *save = NULL;
        char *room_text;
        char *id_text;
        char *type_text;
        char *extra;
        uint16_t room;
        uint16_t id;
        sensor_type_t type;
        size_t i;

        errno = 0;
        line_length = getline(&line, &capacity, file);
        if (line_length < 0) {
            if (ferror(file) || errno == ENOMEM) {
                status = -1;
            }
            break;
        }
        room_text = strtok_r(line, " \t\r\n", &save);
        if (room_text == NULL || room_text[0] == '#') {
            continue;
        }
        id_text = strtok_r(NULL, " \t\r\n", &save);
        type_text = strtok_r(NULL, " \t\r\n", &save);
        extra = strtok_r(NULL, " \t\r\n", &save);
        if (id_text == NULL || type_text == NULL ||
            (extra != NULL && extra[0] != '#') ||
            parse_u16(room_text, &room) != 0 ||
            parse_u16(id_text, &id) != 0 ||
            parse_sensor_type(type_text, &type) != 0 ||
            manager->n >= MAX_SENSORS) {
            status = -1;
            errno = EINVAL;
            break;
        }
        for (i = 0; i < manager->n; ++i) {
            if (manager->sensors[i].info.id == id) {
                status = -1;
                errno = EEXIST;
                break;
            }
        }
        if (status != 0) {
            break;
        }
        manager->sensors[manager->n].info = (sensor_info_t) {
            .room = room,
            .id = id,
            .type = type
        };
        manager->sensors[manager->n].state =
            (type == SENSOR_TEMPERATURE || type == SENSOR_HUMIDITY) ?
                STATE_COLLECTING : STATE_NORMAL;
        ++manager->n;
    }
    free(line);
    if (fclose(file) != 0) {
        status = -1;
    }
    if (status == 0 && manager->n == 0) {
        errno = EINVAL;
        status = -1;
    }
    return status;
}

static void add_window_sample(datamgr_sensor_t *sensor, double value)
{
    double total = 0.0;
    size_t i;

    sensor->window[sensor->next] = value;
    sensor->next = (sensor->next + 1U) % RUN_AVG_LENGTH;
    if (sensor->count < RUN_AVG_LENGTH) {
        ++sensor->count;
    }
    for (i = 0; i < sensor->count; ++i) {
        total += sensor->window[i];
    }
    sensor->running_avg = total / (double)sensor->count;
}

static void set_transition_alert(datamgr_sensor_t *sensor,
                                datamgr_event_t *event, int alert_kind,
                                const sensor_data_t *data)
{
    if (sensor->state != STATE_ALERT) {
        switch (alert_kind) {
        case ALERT_TOO_COLD:
            (void)snprintf(event->message, sizeof(event->message),
                           "The sensor node with %u reports it's too cold "
                           "(running avg temperature = %.1f)",
                           (unsigned int)data->id, sensor->running_avg);
            break;
        case ALERT_TOO_HOT:
            (void)snprintf(event->message, sizeof(event->message),
                           "The sensor node with %u reports it's too hot "
                           "(running avg temperature = %.1f)",
                           (unsigned int)data->id, sensor->running_avg);
            break;
        case ALERT_TOO_DRY:
            (void)snprintf(event->message, sizeof(event->message),
                           "The sensor node with %u reports it's too dry "
                           "(running avg humidity = %.1f)",
                           (unsigned int)data->id, sensor->running_avg);
            break;
        case ALERT_TOO_HUMID:
            (void)snprintf(event->message, sizeof(event->message),
                           "The sensor node with %u reports it's too humid "
                           "(running avg humidity = %.1f)",
                           (unsigned int)data->id, sensor->running_avg);
            break;
        case ALERT_GAS:
            (void)snprintf(event->message, sizeof(event->message),
                           "The sensor node with %u reports a smoke/gas alarm "
                           "(concentration = %.1f ppm)",
                           (unsigned int)data->id, data->value);
            break;
        default:
            return;
        }
        event->should_log = true;
    }
    sensor->state = STATE_ALERT;
    sensor->alert_kind = alert_kind;
}

static void handle_temperature(datamgr_sensor_t *sensor,
                               const sensor_data_t *data,
                               datamgr_event_t *event)
{
    int alert_kind = ALERT_NONE;

    add_window_sample(sensor, data->value);
    event->running_avg = sensor->running_avg;
    event->has_running_avg = true;
    if (sensor->count < RUN_AVG_LENGTH) {
        sensor->state = STATE_COLLECTING;
        return;
    }
    if (sensor->running_avg < TEMP_MIN_C) {
        alert_kind = ALERT_TOO_COLD;
    } else if (sensor->running_avg > TEMP_MAX_C) {
        alert_kind = ALERT_TOO_HOT;
    }
    if (alert_kind == ALERT_NONE) {
        sensor->state = STATE_NORMAL;
        sensor->alert_kind = ALERT_NONE;
    } else {
        set_transition_alert(sensor, event, alert_kind, data);
    }
}

static void handle_humidity(datamgr_sensor_t *sensor,
                            const sensor_data_t *data,
                            datamgr_event_t *event)
{
    int alert_kind = ALERT_NONE;

    add_window_sample(sensor, data->value);
    event->running_avg = sensor->running_avg;
    event->has_running_avg = true;
    if (sensor->count < RUN_AVG_LENGTH) {
        sensor->state = STATE_COLLECTING;
        return;
    }
    if (sensor->running_avg < HUMIDITY_MIN) {
        alert_kind = ALERT_TOO_DRY;
    } else if (sensor->running_avg > HUMIDITY_MAX) {
        alert_kind = ALERT_TOO_HUMID;
    }
    if (alert_kind == ALERT_NONE) {
        sensor->state = STATE_NORMAL;
        sensor->alert_kind = ALERT_NONE;
    } else {
        set_transition_alert(sensor, event, alert_kind, data);
    }
}

static void handle_motion(datamgr_sensor_t *sensor,
                          const sensor_data_t *data,
                          datamgr_event_t *event)
{
    (void)sensor;
    if (data->value != 0.0) {
        (void)snprintf(event->message, sizeof(event->message),
                       "The sensor node with %u detected motion",
                       (unsigned int)data->id);
        event->should_log = true;
    }
}

static void handle_door(datamgr_sensor_t *sensor, const sensor_data_t *data,
                        datamgr_event_t *event)
{
    int state = data->value != 0.0;

    if (!sensor->has_door_state || sensor->door_state != state) {
        (void)snprintf(event->message, sizeof(event->message),
                       "The sensor node with %u reports the door is %s",
                       (unsigned int)data->id, state ? "open" : "closed");
        event->should_log = true;
        sensor->door_state = state;
        sensor->has_door_state = 1;
    }
}

static void handle_smoke_gas(datamgr_sensor_t *sensor,
                             const sensor_data_t *data,
                             datamgr_event_t *event)
{
    if (data->value >= GAS_ALARM_PPM) {
        set_transition_alert(sensor, event, ALERT_GAS, data);
    } else {
        sensor->state = STATE_NORMAL;
        sensor->alert_kind = ALERT_NONE;
    }
}

static const sensor_handler_t handlers[] = {
    handle_temperature,
    handle_humidity,
    handle_motion,
    handle_door,
    handle_smoke_gas
};

int datamgr_process(datamgr_t *manager, const sensor_data_t *data,
                    datamgr_event_t *event)
{
    datamgr_sensor_t *sensor = NULL;
    size_t i;

    if (manager == NULL || data == NULL || event == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(event, 0, sizeof(*event));
    if (data->id == 0 || data->type < SENSOR_TEMPERATURE ||
        data->type > SENSOR_SMOKE_GAS || !isfinite(data->value) ||
        data->ts < 0) {
        errno = EINVAL;
        return -1;
    }
    if (manager->n > MAX_SENSORS) {
        errno = EINVAL;
        return -1;
    }
    /* Enforce physical domains that are unambiguous across sensor models. */
    if ((data->type == SENSOR_TEMPERATURE && data->value < -273.15) ||
        (data->type == SENSOR_HUMIDITY &&
         (data->value < 0.0 || data->value > 100.0)) ||
        ((data->type == SENSOR_MOTION || data->type == SENSOR_DOOR) &&
         data->value != 0.0 && data->value != 1.0) ||
        (data->type == SENSOR_SMOKE_GAS && data->value < 0.0)) {
        errno = ERANGE;
        return -1;
    }
    for (i = 0; i < manager->n; ++i) {
        if (manager->sensors[i].info.id == data->id) {
            sensor = &manager->sensors[i];
            break;
        }
    }
    if (sensor == NULL || sensor->info.type != data->type) {
        (void)snprintf(event->message, sizeof(event->message),
                       "Received sensor data with invalid sensor node ID %u",
                       (unsigned int)data->id);
        event->should_log = true;
        return 0;
    }
    event->room = sensor->info.room;
    handlers[data->type - SENSOR_TEMPERATURE](sensor, data, event);
    return 0;
}
