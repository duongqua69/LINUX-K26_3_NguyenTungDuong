#define _POSIX_C_SOURCE 200809L

#include "connmgr.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "logger.h"

#define TBP_FRAME_SIZE 18

#ifdef DEBUG
#define DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINT(...) ((void)0)
#endif

typedef struct {
    int fd;
    uint8_t rx[TBP_FRAME_SIZE];
    size_t rx_len;
    int identified;
    sensor_id_t sensor_id;
} connection_t;

static int stop_requested(gateway_t *gateway)
{
    int stopped;

    (void)pthread_mutex_lock(&gateway->stop_lock);
    stopped = gateway->workers_stop;
    (void)pthread_mutex_unlock(&gateway->stop_lock);
    return stopped;
}

static void log_connection(gateway_t *gateway, const char *event,
                           sensor_id_t sensor_id)
{
    char message[128];

    (void)snprintf(message, sizeof(message), event, (unsigned int)sensor_id);
    if (logger_write(gateway, "CONN", message) != 0) {
        perror("logger_write");
    }
}

static void close_connection(gateway_t *gateway, connection_t *connection)
{
    if (connection->identified) {
        log_connection(gateway,
                       "The sensor node with %u has closed the connection",
                       connection->sensor_id);
    }
    (void)close(connection->fd);
    connection->fd = -1;
    connection->rx_len = 0;
}

static uint64_t decode_little_endian(const uint8_t *bytes, size_t length)
{
    uint64_t value = 0;
    size_t i;

    for (i = 0; i < length; ++i) {
        value |= (uint64_t)bytes[i] << (8U * i);
    }
    return value;
}

static int process_frame(gateway_t *gateway, connection_t *connection)
{
    uint64_t value_bits = decode_little_endian(&connection->rx[2], 8);
    uint64_t timestamp_bits = decode_little_endian(&connection->rx[10], 8);
    sensor_data_t data = { 0 };
    int64_t timestamp;

    data.id = (sensor_id_t)decode_little_endian(connection->rx, 2);
    memcpy(&data.value, &value_bits, sizeof(data.value));
    memcpy(&timestamp, &timestamp_bits, sizeof(timestamp));
    if (data.id == 0 || !isfinite(data.value) || timestamp < 0) {
        return -1;
    }
    data.type = SENSOR_TEMPERATURE;
    data.ts = (time_t)timestamp;
    if (sbuffer_insert(&gateway->sbuffer, &data) != 0) {
        return -1;
    }
    if (!connection->identified) {
        connection->identified = 1;
        connection->sensor_id = data.id;
        log_connection(gateway,
                       "A sensor node with %u has opened a new connection",
                       data.id);
    }
    return 0;
}

static int create_listener(uint16_t port)
{
    struct sockaddr_in address = { 0 };
    int enabled = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        return -1;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, SOMAXCONN) != 0) {
        int saved_errno = errno;
        (void)close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

static void log_attempt_failure(gateway_t *gateway, const char *reason)
{
    char message[160];

    (void)snprintf(message, sizeof(message),
                   "Connection attempt failed on TBP port (%s)", reason);
    if (logger_write(gateway, "CONN", message) != 0) {
        perror("logger_write");
    }
}

void *connection_manager_thread(void *argument)
{
    gateway_t *gateway = argument;
    connection_t *connections = NULL;
    size_t count = 0;
    int listener = create_listener(gateway->base_port);

    if (logger_write(gateway, "CONN", "Connection manager thread started") != 0) {
        perror("logger_write");
    }

    if (listener < 0) {
        log_attempt_failure(gateway, strerror(errno));
        return NULL;
    }
    while (!stop_requested(gateway)) {
        size_t polled_count = count;
        struct pollfd *pollfds = calloc(polled_count + 1, sizeof(*pollfds));
        size_t i;
        int ready;

        if (pollfds == NULL) {
            log_attempt_failure(gateway, "out of memory");
            break;
        }
        pollfds[0].fd = listener;
        pollfds[0].events = POLLIN;
        for (i = 0; i < polled_count; ++i) {
            pollfds[i + 1].fd = connections[i].fd;
            pollfds[i + 1].events = POLLIN;
        }
        ready = poll(pollfds, polled_count + 1, 200);
        if (ready < 0 && errno != EINTR) {
            free(pollfds);
            log_attempt_failure(gateway, strerror(errno));
            break;
        }
        if (ready > 0 && (pollfds[0].revents & POLLIN) != 0) {
            for (;;) {
                int client = accept(listener, NULL, NULL);
                connection_t *grown;
                if (client < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        log_attempt_failure(gateway, strerror(errno));
                    }
                    break;
                }
                grown = realloc(connections, (count + 1) * sizeof(*connections));
                if (grown == NULL) {
                    log_attempt_failure(gateway, "out of memory");
                    (void)close(client);
                    break;
                }
                connections = grown;
                connections[count++] = (connection_t){ .fd = client };
                /* Accept one connection per readiness notification. */
                break;
            }
        }
        for (i = polled_count; i > 0; --i) {
            size_t index = i - 1;
            short events = pollfds[index + 1].revents;
            connection_t *connection = &connections[index];
            int remove = (events & (POLLERR | POLLNVAL)) != 0;

            if (!remove && (events & POLLIN) != 0) {
                ssize_t received = recv(connection->fd,
                                        connection->rx + connection->rx_len,
                                        TBP_FRAME_SIZE - connection->rx_len, 0);
                if (received <= 0) {
                    remove = 1;
                } else {
                    connection->rx_len += (size_t)received;
                    if (connection->rx_len == TBP_FRAME_SIZE) {
                        if (process_frame(gateway, connection) != 0) {
                            log_attempt_failure(gateway, "invalid TBP frame");
                            remove = 1;
                        } else {
                            connection->rx_len = 0;
                        }
                    }
                }
            }
            if ((events & POLLHUP) != 0) {
                remove = 1;
            }
            if (remove) {
                close_connection(gateway, connection);
                if (index + 1 < count) {
                    memmove(&connections[index], &connections[index + 1],
                            (count - index - 1) * sizeof(*connections));
                }
                --count;
            }
        }
        free(pollfds);
    }
    (void)close(listener);
    while (count > 0) {
        close_connection(gateway, &connections[--count]);
    }
    free(connections);
    return NULL;
}

static void *reader_thread(gateway_t *gateway, sbuffer_reader_t reader,
                           const char *label)
{
    sensor_data_t data;
    char start_message[96];
    const char *source = reader == SBUFFER_READER_DATA ? "DATA" : "STOR";

    (void)snprintf(start_message, sizeof(start_message),
                   "%s thread started", label);
    if (logger_write(gateway, source, start_message) != 0) {
        perror("logger_write");
    }

    for (;;) {
        int result = sbuffer_read(&gateway->sbuffer, reader, &data);
        if (result <= 0) {
            break;
        }
        DEBUG_PRINT("DEBUG %s: sensor_id=%u type=%d value=%.2f ts=%lld\n",
                    label, (unsigned int)data.id, (int)data.type, data.value,
                    (long long)data.ts);
    }
    return NULL;
}

void *data_manager_thread(void *argument)
{
    return reader_thread(argument, SBUFFER_READER_DATA, "Data manager");
}

void *storage_manager_thread(void *argument)
{
    return reader_thread(argument, SBUFFER_READER_STORAGE, "Storage manager");
}
