#define _POSIX_C_SOURCE 200809L

#include "connmgr.h"
#include "logger.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * Prefer the system SQLite declarations. Some minimal runtime images ship the
 * SQLite shared library without the development header, so declare the small
 * stable C API subset used here when that header is unavailable.
 */
#if defined(__has_include)
#  if __has_include(<sqlite3.h>)
#    include <sqlite3.h>
#    define HAVE_SQLITE3_HEADER 1
#  endif
#endif
#ifndef HAVE_SQLITE3_HEADER
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef long long sqlite3_int64;
extern int sqlite3_open(const char *, sqlite3 **);
extern int sqlite3_close(sqlite3 *);
extern int sqlite3_busy_timeout(sqlite3 *, int);
extern int sqlite3_prepare_v2(sqlite3 *, const char *, int,
                              sqlite3_stmt **, const char **);
extern int sqlite3_step(sqlite3_stmt *);
extern int sqlite3_finalize(sqlite3_stmt *);
extern int sqlite3_exec(sqlite3 *, const char *,
                        int (*)(void *, int, char **, char **), void *, char **);
extern int sqlite3_bind_int(sqlite3_stmt *, int, int);
extern int sqlite3_bind_double(sqlite3_stmt *, int, double);
extern int sqlite3_bind_int64(sqlite3_stmt *, int, sqlite3_int64);
extern const char *sqlite3_errmsg(sqlite3 *);
#define SQLITE_OK 0
#define SQLITE_ROW 100
#define SQLITE_DONE 101
#endif

#define DATABASE_RETRIES 3
#define DATABASE_RETRY_DELAY_SECONDS 1

static int gateway_is_stopping(gateway_t *gateway)
{
    int stopping;

    (void)pthread_mutex_lock(&gateway->stop_lock);
    stopping = gateway->workers_stop;
    (void)pthread_mutex_unlock(&gateway->stop_lock);
    return stopping;
}

static void log_storage(gateway_t *gateway, const char *message)
{
    if (logger_write(gateway, "STOR", message) != 0) {
        perror("logger_write");
    }
}

static int open_database(gateway_t *gateway, sqlite3 **database)
{
    sqlite3_stmt *statement = NULL;
    int table_exists = 0;
    int result;

    *database = NULL;
    result = sqlite3_open(gateway->database_path, database);
    if (result != SQLITE_OK) {
        if (*database != NULL) {
            (void)sqlite3_close(*database);
            *database = NULL;
        }
        return -1;
    }
    (void)sqlite3_busy_timeout(*database, 1000);
    result = sqlite3_prepare_v2(*database,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='SensorData'",
        -1, &statement, NULL);
    if (result == SQLITE_OK) {
        table_exists = sqlite3_step(statement) == SQLITE_ROW;
        (void)sqlite3_finalize(statement);
    }
    result = sqlite3_exec(*database,
        "CREATE TABLE IF NOT EXISTS SensorData("
        "id INTEGER, type INTEGER, value REAL, ts INTEGER)",
        NULL, NULL, NULL);
    if (result != SQLITE_OK) {
        (void)sqlite3_close(*database);
        *database = NULL;
        return -1;
    }
    log_storage(gateway, "Connection to SQL server established.");
    if (!table_exists) {
        log_storage(gateway, "New table SensorData created.");
    }
    return 0;
}

static int connect_with_retries(gateway_t *gateway, sqlite3 **database)
{
    unsigned int attempt;

    for (attempt = 0; attempt < DATABASE_RETRIES; ++attempt) {
        if (open_database(gateway, database) == 0) {
            return 0;
        }
        log_storage(gateway, "Unable to connect to SQL server.");
        if (attempt + 1 < DATABASE_RETRIES && !gateway_is_stopping(gateway)) {
            (void)sleep(DATABASE_RETRY_DELAY_SECONDS);
        }
        if (gateway_is_stopping(gateway)) {
            return -1;
        }
    }
    return -1;
}

static int insert_measurement(sqlite3 *database, const sensor_data_t *data)
{
    static const char sql[] =
        "INSERT INTO SensorData(id, type, value, ts) VALUES(?, ?, ?, ?)";
    sqlite3_stmt *statement = NULL;
    int result = sqlite3_prepare_v2(database, sql, -1, &statement, NULL);

    if (result == SQLITE_OK) {
        result = sqlite3_bind_int(statement, 1, (int)data->id);
    }
    if (result == SQLITE_OK) {
        result = sqlite3_bind_int(statement, 2, (int)data->type);
    }
    if (result == SQLITE_OK) {
        result = sqlite3_bind_double(statement, 3, data->value);
    }
    if (result == SQLITE_OK) {
        result = sqlite3_bind_int64(statement, 4, (sqlite3_int64)data->ts);
    }
    if (result == SQLITE_OK) {
        result = sqlite3_step(statement);
    }
    (void)sqlite3_finalize(statement);
    return result == SQLITE_DONE ? 0 : -1;
}

static void fail_gateway(gateway_t *gateway)
{
    (void)pthread_mutex_lock(&gateway->stop_lock);
    gateway->storage_failed = 1;
    (void)pthread_mutex_unlock(&gateway->stop_lock);
    /* SIGTERM wakes the main thread from sigsuspend and uses normal cleanup. */
    (void)kill(getpid(), SIGTERM);
}

void *storage_manager_thread(void *argument)
{
    gateway_t *gateway = argument;
    sqlite3 *database = NULL;
    sensor_data_t data;
    uint64_t sequence;

    log_storage(gateway, "Storage manager thread started");
    if (connect_with_retries(gateway, &database) != 0) {
        if (!gateway_is_stopping(gateway)) {
            fail_gateway(gateway);
        }
        return NULL;
    }

    for (;;) {
        int result = sbuffer_read(&gateway->sbuffer, SBUFFER_READER_STORAGE,
                                  &data, &sequence);
        if (result <= 0) {
            break;
        }
        for (;;) {
            if (insert_measurement(database, &data) == 0) {
                (void)sbuffer_ack(&gateway->sbuffer, SBUFFER_READER_STORAGE,
                                  sequence);
                break;
            }
            log_storage(gateway, "Connection to SQL server lost.");
            (void)sqlite3_close(database);
            database = NULL;
            if (connect_with_retries(gateway, &database) != 0) {
                if (!gateway_is_stopping(gateway)) {
                    fail_gateway(gateway);
                }
                return NULL;
            }
            /* Keep the same unacknowledged sbuffer item and retry its INSERT. */
        }
    }

    if (database != NULL) {
        (void)sqlite3_close(database);
    }
    return NULL;
}
