#include <stdio.h>
#include <string.h>

#include "datamgr.h"

static int process_value(datamgr_t *manager, sensor_id_t id,
                         sensor_type_t type, double value,
                         datamgr_event_t *event)
{
    sensor_data_t data = {
        .id = id,
        .type = type,
        .value = value,
        .ts = 1767225600
    };
    return datamgr_process(manager, &data, event);
}

static int expect_event(datamgr_t *manager, sensor_id_t id,
                        sensor_type_t type, double value, int expected,
                        const char *message_fragment)
{
    datamgr_event_t event;
    if (process_value(manager, id, type, value, &event) != 0 ||
        event.should_log != (expected != 0) ||
        (message_fragment != NULL &&
         strstr(event.message, message_fragment) == NULL)) {
        fprintf(stderr, "FAIL sensor=%u value=%.2f expected_log=%d got='%s'\n",
                (unsigned int)id, value, expected, event.message);
        return 1;
    }
    return 0;
}

static int send_repeated(datamgr_t *manager, sensor_id_t id,
                         sensor_type_t type, double value, size_t count,
                         size_t log_at, const char *expected_message)
{
    size_t i;
    int failures = 0;
    for (i = 0; i < count; ++i) {
        int should_log = i == log_at;
        failures += expect_event(manager, id, type, value, should_log,
                                 should_log ? expected_message : NULL);
    }
    return failures;
}

int main(void)
{
    datamgr_t manager;
    datamgr_event_t event;
    int failures = 0;

    if (datamgr_load(&manager, "test/room_sensor_test.map") != 0) {
        perror("datamgr_load");
        return 1;
    }
    if (manager.n != 5 || manager.sensors[0].info.room != 501) {
        fprintf(stderr, "FAIL room-sensor catalog load\n");
        ++failures;
    }

    failures += send_repeated(&manager, 1, SENSOR_TEMPERATURE, 25.0,
                              RUN_AVG_LENGTH, RUN_AVG_LENGTH - 1,
                              "reports it's too hot");
    failures += expect_event(&manager, 1, SENSOR_TEMPERATURE, 25.0, 0, NULL);
    failures += send_repeated(&manager, 1, SENSOR_TEMPERATURE, 18.0,
                              RUN_AVG_LENGTH, SIZE_MAX, NULL);
    failures += send_repeated(&manager, 1, SENSOR_TEMPERATURE, 10.0,
                              RUN_AVG_LENGTH, 1, "reports it's too cold");

    failures += send_repeated(&manager, 2, SENSOR_HUMIDITY, 20.0,
                              RUN_AVG_LENGTH, RUN_AVG_LENGTH - 1,
                              "reports it's too dry");
    failures += send_repeated(&manager, 2, SENSOR_HUMIDITY, 50.0,
                              RUN_AVG_LENGTH, SIZE_MAX, NULL);
    failures += send_repeated(&manager, 2, SENSOR_HUMIDITY, 80.0,
                              RUN_AVG_LENGTH, RUN_AVG_LENGTH - 2,
                              "reports it's too humid");

    failures += expect_event(&manager, 5, SENSOR_SMOKE_GAS, 299.0, 0, NULL);
    failures += expect_event(&manager, 5, SENSOR_SMOKE_GAS, 300.0, 1,
                             "concentration = 300.0 ppm");
    failures += expect_event(&manager, 5, SENSOR_SMOKE_GAS, 350.0, 0, NULL);
    failures += expect_event(&manager, 5, SENSOR_SMOKE_GAS, 100.0, 0, NULL);
    failures += expect_event(&manager, 5, SENSOR_SMOKE_GAS, 300.0, 1,
                             "smoke/gas alarm");

    failures += expect_event(&manager, 3, SENSOR_MOTION, 1.0, 1,
                             "detected motion");
    failures += expect_event(&manager, 3, SENSOR_MOTION, 0.0, 0, NULL);
    failures += expect_event(&manager, 4, SENSOR_DOOR, 0.0, 1,
                             "door is closed");
    failures += expect_event(&manager, 4, SENSOR_DOOR, 0.0, 0, NULL);
    failures += expect_event(&manager, 4, SENSOR_DOOR, 1.0, 1,
                             "door is open");
    failures += expect_event(&manager, 4, SENSOR_DOOR, 1.0, 0, NULL);
    failures += expect_event(&manager, 4, SENSOR_DOOR, 0.0, 1,
                             "door is closed");

    failures += expect_event(&manager, 99, SENSOR_TEMPERATURE, 20.0, 1,
                             "invalid sensor node ID 99");
    failures += expect_event(&manager, 1, SENSOR_HUMIDITY, 50.0, 1,
                             "invalid sensor node ID 1");
    if (process_value(&manager, 1, SENSOR_TEMPERATURE, 25.0, &event) != 0 ||
        event.room != 501 || !event.has_running_avg) {
        fprintf(stderr, "FAIL room association or internal running average\n");
        ++failures;
    }

    if (failures != 0) {
        fprintf(stderr, "%d Data manager check(s) failed\n", failures);
        return 1;
    }
    puts("All Data manager checks passed.");
    return 0;
}
