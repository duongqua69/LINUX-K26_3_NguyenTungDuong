#define _POSIX_C_SOURCE 200809L

#include "connmgr.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "logger.h"
#include "protocol_adapter.h"

#define ADAPTER_COUNT 4
#define RX_BUF_SIZE 512
#define MAX_CONSECUTIVE_PARSE_ERRORS 3
#define MAX_CONNECTIONS 64U

#ifdef DEBUG
#define DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINT(...) ((void)0)
#endif

typedef struct {
    int fd;
    const protocol_adapter_t *adapter;
    uint8_t rx[RX_BUF_SIZE];
    size_t rx_len;
    unsigned int consecutive_errors;
    int identified;
    sensor_id_t sensor_id;
    int has_last_sgp_frame;
    sensor_id_t last_sgp_id;
    time_t last_sgp_ts;
} connection_t;

static int stop_requested(gateway_t *gateway)
{
    int stopped;
    int result = pthread_mutex_lock(&gateway->stop_lock);

    if (result != 0) {
        errno = result;
        return -1;
    }
    stopped = gateway->workers_stop;
    result = pthread_mutex_unlock(&gateway->stop_lock);
    if (result != 0) {
        errno = result;
        return -1;
    }
    return stopped;
}

static void report_manager_failure(gateway_t *gateway)
{
    int result = pthread_mutex_lock(&gateway->stop_lock);
    if (result == 0) {
        gateway->connection_failed = 1;
        gateway->workers_stop = 1;
        result = pthread_mutex_unlock(&gateway->stop_lock);
    }
    if (result != 0) {
        fprintf(stderr, "Connection manager mutex failure: %s\n",
                strerror(result));
    }
    /* SIGTERM wakes main, which has it blocked except while waiting in sigsuspend. */
    (void)kill(getpid(), SIGTERM);
}

static int send_all(int fd, const uint8_t *buffer, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        ssize_t sent = send(fd, buffer + offset, length - offset, 0);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (sent == 0) {
            errno = EPIPE;
            return -1;
        }
        offset += (size_t)sent;
    }
    return 0;
}

static void log_sensor_event(gateway_t *gateway, const char *event,
                             sensor_id_t sensor_id)
{
    char message[160];

    (void)snprintf(message, sizeof(message), event, (unsigned int)sensor_id);
    if (logger_write(gateway, "CONN", message) != 0) {
        perror("logger_write");
    }
}

static void log_malformed_frame(gateway_t *gateway,
                                const protocol_adapter_t *adapter,
                                const char *reason)
{
    char message[160];

    (void)snprintf(message, sizeof(message),
                   "Received malformed frame on %s port (%s)",
                   adapter->name, reason);
    if (logger_write(gateway, "CONN", message) != 0) {
        perror("logger_write");
    }
}

static void close_connection(gateway_t *gateway, connection_t *connection)
{
    if (connection->identified) {
        log_sensor_event(gateway,
                         "The sensor node with %u has closed the connection",
                         connection->sensor_id);
    }
    (void)close(connection->fd);
}

static int create_listener(uint16_t port)
{
    struct sockaddr_in address = { 0 };
    int enabled = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0) {
        int saved_errno = errno;
        (void)close(fd);
        errno = saved_errno;
        return -1;
    }
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

static void log_listener_failure(gateway_t *gateway,
                                 const protocol_adapter_t *adapter,
                                 const char *reason)
{
    char message[160];

    (void)snprintf(message, sizeof(message),
                   "Connection attempt failed on %s port (%s)",
                   adapter->name, reason);
    if (logger_write(gateway, "CONN", message) != 0) {
        perror("logger_write");
    }
}

static int append_connection(connection_t **connections, size_t *count,
                             int fd, const protocol_adapter_t *adapter)
{
    connection_t *grown = realloc(*connections,
                                  (*count + 1) * sizeof(**connections));
    if (grown == NULL) {
        return -1;
    }
    *connections = grown;
    (*connections)[*count] = (connection_t){
        .fd = fd,
        .adapter = adapter
    };
    ++(*count);
    return 0;
}

static int send_adapter_reply(connection_t *connection,
                              parse_result_t result)
{
    uint8_t reply[8];
    size_t reply_len = connection->adapter->build_reply(
        result, reply, sizeof(reply));

    if (reply_len == 0) {
        return 0;
    }
    return send_all(connection->fd, reply, reply_len);
}

static int process_buffer(gateway_t *gateway, connection_t *connection)
{
    while (connection->rx_len > 0) {
        sensor_data_t data = { 0 };
        size_t consumed = 0;
        parse_result_t result = connection->adapter->parse(
            connection->rx, connection->rx_len, &data, &consumed);

        if (result == PARSE_NEED_MORE) {
            return connection->rx_len == sizeof(connection->rx) ? -1 : 0;
        }
        if (consumed == 0 || consumed > connection->rx_len) {
            log_malformed_frame(gateway, connection->adapter,
                                "parser returned invalid consumed length");
            return -1;
        }
        if (result == PARSE_ERROR) {
            log_malformed_frame(gateway, connection->adapter, "invalid frame");
            ++connection->consecutive_errors;
            if (send_adapter_reply(connection, PARSE_ERROR) != 0) {
                return -1;
            }
            memmove(connection->rx, connection->rx + consumed,
                    connection->rx_len - consumed);
            connection->rx_len -= consumed;
            if (connection->consecutive_errors >= MAX_CONSECUTIVE_PARSE_ERRORS) {
                log_malformed_frame(gateway, connection->adapter,
                                    "three consecutive errors; closing connection");
                return -1;
            }
            continue;
        }
        if (result != PARSE_OK) {
            log_malformed_frame(gateway, connection->adapter,
                                "parser returned unknown result");
            return -1;
        }

        connection->consecutive_errors = 0;
        if (!connection->identified) {
            connection->identified = 1;
            connection->sensor_id = data.id;
            log_sensor_event(gateway,
                             "A sensor node with %u has opened a new connection",
                             data.id);
        }

        /* SGP retries may repeat an acknowledged measurement after a lost ACK. */
        if (connection->adapter->port_offset == 3 &&
            connection->has_last_sgp_frame &&
            connection->last_sgp_id == data.id &&
            connection->last_sgp_ts == data.ts) {
            DEBUG_PRINT("Ignoring duplicate SGP frame: sensor_id=%u ts=%lld\n",
                        (unsigned int)data.id, (long long)data.ts);
        } else if (sbuffer_insert(&gateway->sbuffer, &data) != 0) {
            log_listener_failure(gateway, connection->adapter, strerror(errno));
            return -1;
        }
        if (connection->adapter->port_offset == 3) {
            connection->has_last_sgp_frame = 1;
            connection->last_sgp_id = data.id;
            connection->last_sgp_ts = data.ts;
        }
        if (send_adapter_reply(connection, PARSE_OK) != 0) {
            return -1;
        }

        memmove(connection->rx, connection->rx + consumed,
                connection->rx_len - consumed);
        connection->rx_len -= consumed;
    }
    return 0;
}

void *connection_manager_thread(void *argument)
{
    gateway_t *gateway = argument;
    int listeners[ADAPTER_COUNT] = { -1, -1, -1, -1 };
    connection_t *connections = NULL;
    size_t count = 0;
    size_t i;
    int fatal_error = 0;

    if (gateway == NULL) {
        errno = EINVAL;
        return NULL;
    }

    if (logger_write(gateway, "CONN", "Connection manager thread started") != 0) {
        perror("logger_write");
    }
    for (i = 0; i < ADAPTER_COUNT; ++i) {
        uint16_t port = (uint16_t)(gateway->base_port +
                                   protocol_adapters[i].port_offset);
        listeners[i] = create_listener(port);
        if (listeners[i] < 0) {
            log_listener_failure(gateway, &protocol_adapters[i], strerror(errno));
            while (i > 0) {
                (void)close(listeners[--i]);
            }
            report_manager_failure(gateway);
            return NULL;
        }
    }

    for (;;) {
        int stopping = stop_requested(gateway);
        size_t polled_count = count;
        struct pollfd *pollfds = calloc(ADAPTER_COUNT + polled_count,
                                       sizeof(*pollfds));
        int ready;

        if (stopping != 0) {
            if (stopping < 0) {
                fatal_error = 1;
            }
            free(pollfds);
            break;
        }

        if (pollfds == NULL) {
            log_listener_failure(gateway, &protocol_adapters[0], "out of memory");
            fatal_error = 1;
            break;
        }
        for (i = 0; i < ADAPTER_COUNT; ++i) {
            pollfds[i].fd = listeners[i];
            pollfds[i].events = POLLIN;
        }
        for (i = 0; i < polled_count; ++i) {
            pollfds[ADAPTER_COUNT + i].fd = connections[i].fd;
            pollfds[ADAPTER_COUNT + i].events = POLLIN;
        }
        ready = poll(pollfds, ADAPTER_COUNT + polled_count, 200);
        if (ready < 0 && errno != EINTR) {
            log_listener_failure(gateway, &protocol_adapters[0], strerror(errno));
            free(pollfds);
            fatal_error = 1;
            break;
        }

        for (i = 0; i < ADAPTER_COUNT; ++i) {
            if ((pollfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                log_listener_failure(gateway, &protocol_adapters[i],
                                     "listener poll failure");
                fatal_error = 1;
                break;
            }
        }
        if (fatal_error) {
            free(pollfds);
            break;
        }

        for (i = 0; i < ADAPTER_COUNT && ready > 0; ++i) {
            if ((pollfds[i].revents & POLLIN) != 0) {
                int client = accept(listeners[i], NULL, NULL);
                if (client >= 0) {
                    if (count >= MAX_CONNECTIONS) {
                        log_listener_failure(gateway, &protocol_adapters[i],
                                             "maximum connection count reached");
                        (void)close(client);
                    } else if (append_connection(&connections, &count, client,
                                          &protocol_adapters[i]) != 0) {
                        log_listener_failure(gateway, &protocol_adapters[i],
                                             "out of memory");
                        (void)close(client);
                    }
                } else if (errno != EINTR && errno != EAGAIN &&
                           errno != EWOULDBLOCK && errno != ECONNABORTED) {
                    log_listener_failure(gateway, &protocol_adapters[i],
                                         strerror(errno));
                    fatal_error = 1;
                }
            }
        }

        if (fatal_error) {
            free(pollfds);
            break;
        }

        for (i = polled_count; i > 0; --i) {
            size_t index = i - 1;
            short events = pollfds[ADAPTER_COUNT + index].revents;
            connection_t *connection = &connections[index];
            int remove = (events & (POLLERR | POLLNVAL)) != 0;

            if (!remove && (events & POLLIN) != 0) {
                ssize_t received;
                if (connection->rx_len == sizeof(connection->rx) &&
                    process_buffer(gateway, connection) != 0) {
                    remove = 1;
                }
                if (!remove) {
                    received = recv(connection->fd,
                                    connection->rx + connection->rx_len,
                                    sizeof(connection->rx) - connection->rx_len,
                                    0);
                    if (received == 0) {
                        remove = 1;
                    } else if (received < 0) {
                        if (errno == EINTR || errno == EAGAIN ||
                            errno == EWOULDBLOCK) {
                            continue;
                        }
                        remove = 1;
                    } else {
                        connection->rx_len += (size_t)received;
                        if (process_buffer(gateway, connection) != 0) {
                            remove = 1;
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

    for (i = 0; i < ADAPTER_COUNT; ++i) {
        (void)close(listeners[i]);
    }
    while (count > 0) {
        close_connection(gateway, &connections[--count]);
    }
    free(connections);
    if (fatal_error) {
        report_manager_failure(gateway);
    }
    return NULL;
}

void *data_manager_thread(void *argument)
{
    gateway_t *gateway = argument;
    sensor_data_t data;
    uint64_t sequence;

    if (gateway == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (logger_write(gateway, "DATA", "Data manager thread started") != 0) {
        perror("logger_write");
    }
    for (;;) {
        datamgr_event_t event;
        int result = sbuffer_read(&gateway->sbuffer, SBUFFER_READER_DATA,
                                  &data, &sequence);
        if (result <= 0) {
            break;
        }
        if (datamgr_process(&gateway->datamgr, &data, &event) != 0) {
            perror("datamgr_process");
            (void)sbuffer_ack(&gateway->sbuffer, SBUFFER_READER_DATA, sequence);
            continue;
        }
        if (event.should_log &&
            logger_write(gateway, "DATA", event.message) != 0) {
            perror("logger_write");
        }
        DEBUG_PRINT("DEBUG Data manager: sensor_id=%u type=%d value=%.2f "
                    "room=%u avg=%s%.2f\n",
                    (unsigned int)data.id, (int)data.type, data.value,
                    (unsigned int)event.room,
                    event.has_running_avg ? "" : "n/a ",
                    event.running_avg);
        (void)sbuffer_ack(&gateway->sbuffer, SBUFFER_READER_DATA, sequence);
    }
    return NULL;
}
