#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define TBP_SIZE 18U
#define SGP_SIZE 11U
#define ACK 0x06U
#define NAK 0x15U

typedef enum {
    NODE_TEMPERATURE,
    NODE_HUMIDITY,
    NODE_MOTION,
    NODE_DOOR,
    NODE_SMOKE_GAS
} node_type_t;

typedef struct {
    int fragment;
    int glue;
    int corrupt;
} node_options_t;

static int parse_unsigned(const char *text, unsigned long maximum,
                          unsigned long *value)
{
    char *end = NULL;
    unsigned long parsed;
    const unsigned char *cursor;

    if (text == NULL || value == NULL || text[0] == '\0') {
        return -1;
    }
    for (cursor = (const unsigned char *)text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return -1;
        }
    }
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0 ||
        parsed > maximum) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static void put_le(uint8_t *output, uint64_t value, size_t size)
{
    size_t i;
    for (i = 0; i < size; ++i) {
        output[i] = (uint8_t)(value >> (8U * i));
    }
}

static void put_be(uint8_t *output, uint64_t value, size_t size)
{
    size_t i;
    for (i = 0; i < size; ++i) {
        output[size - i - 1] = (uint8_t)(value >> (8U * i));
    }
}

static uint16_t crc16_ccitt_false(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xFFFFU;
    size_t i;
    unsigned int bit;

    for (i = 0; i < len; ++i) {
        crc ^= (uint16_t)buf[i] << 8U;
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) != 0 ?
                (uint16_t)((crc << 1U) ^ 0x1021U) :
                (uint16_t)(crc << 1U);
        }
    }
    return crc;
}

static int send_all(int fd, const uint8_t *buffer, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t sent = send(fd, buffer + offset, length - offset, MSG_NOSIGNAL);
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

static int send_fragmented(int fd, const uint8_t *buffer, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        size_t chunk = (offset % 3U) + 1U;
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 5000000L };
        if (chunk > length - offset) {
            chunk = length - offset;
        }
        if (send_all(fd, buffer + offset, chunk) != 0) {
            return -1;
        }
        offset += chunk;
        (void)nanosleep(&pause, NULL);
    }
    return 0;
}

static size_t build_frame(node_type_t type, uint16_t id, uint64_t sequence,
                          int corrupt, int has_override,
                          double override_value, uint8_t *frame,
                          size_t capacity)
{
    int64_t timestamp = (int64_t)time(NULL) + (int64_t)sequence;
    uint32_t timestamp32 = (uint32_t)timestamp;
    size_t length;

    memset(frame, 0, capacity);
    switch (type) {
    case NODE_TEMPERATURE: {
        double value = has_override ? override_value :
                       18.0 + (double)(id % 60U) / 10.0;
        uint64_t bits;
        if (capacity < TBP_SIZE) return 0;
        memcpy(&bits, &value, sizeof(bits));
        put_le(frame, id, 2);
        put_le(frame + 2, bits, 8);
        put_le(frame + 10, (uint64_t)timestamp, 8);
        if (corrupt) frame[0] = frame[1] = 0;
        return TBP_SIZE;
    }
    case NODE_HUMIDITY: {
        int written;
        double value = has_override ? override_value : 54.3;
        written = snprintf((char *)frame, capacity, "HUM %u %.1f %lld\n",
                           (unsigned int)id, value, (long long)timestamp);
        if (written < 0 || (size_t)written >= capacity) return 0;
        if (corrupt) memcpy(frame, "BAD", 3);
        return (size_t)written;
    }
    case NODE_MOTION:
        if (capacity < 8) return 0;
        frame[0] = 0x01;
        frame[1] = 6;
        put_be(frame + 2, corrupt ? 0 : id, 2);
        put_be(frame + 4, timestamp32, 4);
        return 8;
    case NODE_DOOR:
        if (capacity < 9) return 0;
        frame[0] = 0x02;
        frame[1] = 7;
        put_be(frame + 2, id, 2);
        frame[4] = corrupt ? 2 : (uint8_t)(has_override ? override_value :
                                                          sequence % 2U);
        put_be(frame + 5, timestamp32, 4);
        return 9;
    case NODE_SMOKE_GAS:
        if (capacity < SGP_SIZE) return 0;
        frame[0] = 0x7E;
        put_be(frame + 1, id, 2);
        put_be(frame + 3, has_override ? (uint16_t)override_value :
                                         100U + id % 200U, 2);
        put_be(frame + 5, timestamp32, 4);
        put_be(frame + 9, crc16_ccitt_false(frame + 1, 8), 2);
        if (corrupt) frame[10] ^= 0x01;
        return SGP_SIZE;
    }
    length = 0;
    return length;
}

static int await_sgp_reply(int fd)
{
    struct pollfd descriptor = { .fd = fd, .events = POLLIN };
    uint8_t reply;
    int ready;
    ssize_t received;

    do {
        ready = poll(&descriptor, 1, 1000);
    } while (ready < 0 && errno == EINTR);

    if (ready <= 0 || (descriptor.revents & POLLIN) == 0) {
        errno = ready == 0 ? ETIMEDOUT : errno;
        return -1;
    }
    do {
        received = recv(fd, &reply, 1, 0);
    } while (received < 0 && errno == EINTR);
    if (received != 1) {
        return -1;
    }
    return reply == ACK ? 1 : (reply == NAK ? 0 : -1);
}

static int connect_sensor(const char *host, uint16_t port)
{
    struct addrinfo hints = { 0 };
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    int fd = -1;
    int result;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    result = getaddrinfo(host, NULL, &hints, &addresses);
    if (result != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(result));
        return -1;
    }
    for (address = addresses; address != NULL; address = address->ai_next) {
        struct sockaddr_storage server_address;
        socklen_t address_length = (socklen_t)address->ai_addrlen;
        memcpy(&server_address, address->ai_addr, address->ai_addrlen);
        if (address->ai_family == AF_INET) {
            ((struct sockaddr_in *)&server_address)->sin_port = htons(port);
        } else if (address->ai_family == AF_INET6) {
            ((struct sockaddr_in6 *)&server_address)->sin6_port = htons(port);
        }
        fd = socket(address->ai_family, address->ai_socktype,
                    address->ai_protocol);
        if (fd >= 0 && connect(fd, (struct sockaddr *)&server_address,
                              address_length) == 0) {
            break;
        }
        if (fd >= 0) {
            (void)close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(addresses);
    return fd;
}

int main(int argc, char *argv[])
{
    static const char *const type_names[] = {
        "temperature", "humidity", "motion", "door", "smoke_gas"
    };
    static const uint16_t port_offsets[] = { 0, 1, 2, 2, 3 };
    node_type_t type;
    node_options_t options = { 0 };
    unsigned long id;
    unsigned long base_port;
    unsigned long cycle_ms;
    double override_value = 0.0;
    int has_override = 0;
    int fd;
    int i;
    uint64_t sequence = 0;

    if (argc < 6) {
        fprintf(stderr, "Usage: %s <type> <id> <host> <base-port> <cycle-ms> "
                        "[--fragment] [--glue] [--corrupt] [--value number]\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    for (i = 0; i < 5; ++i) {
        if (strcmp(argv[1], type_names[i]) == 0) break;
    }
    if (i == 5 || parse_unsigned(argv[2], UINT16_MAX, &id) != 0 ||
        parse_unsigned(argv[4], UINT16_MAX - 3U, &base_port) != 0 ||
        parse_unsigned(argv[5], UINT32_MAX, &cycle_ms) != 0) {
        fprintf(stderr, "Invalid sensor type, ID, port, or cycle.\n");
        return EXIT_FAILURE;
    }
    type = (node_type_t)i;
    for (i = 6; i < argc; ++i) {
        if (strcmp(argv[i], "--fragment") == 0) options.fragment = 1;
        else if (strcmp(argv[i], "--glue") == 0) options.glue = 1;
        else if (strcmp(argv[i], "--corrupt") == 0) options.corrupt = 1;
        else if (strcmp(argv[i], "--value") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            override_value = strtod(argv[++i], &end);
            if (errno != 0 || end == argv[i] || *end != '\0' ||
                !isfinite(override_value)) {
                fprintf(stderr, "Invalid --value argument.\n");
                return EXIT_FAILURE;
            }
            has_override = 1;
        }
        else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return EXIT_FAILURE;
        }
    }
    if (has_override &&
        ((type == NODE_TEMPERATURE && override_value < -273.15) ||
         (type == NODE_HUMIDITY &&
          (override_value < 0.0 || override_value > 100.0)) ||
         ((type == NODE_MOTION || type == NODE_DOOR) &&
          override_value != 0.0 && override_value != 1.0) ||
         (type == NODE_SMOKE_GAS &&
          (override_value < 0.0 || override_value > UINT16_MAX ||
           override_value != (double)(uint16_t)override_value)))) {
        fprintf(stderr, "--value is out of range for this sensor type.\n");
        return EXIT_FAILURE;
    }
    fd = connect_sensor(argv[3], (uint16_t)(base_port + port_offsets[type]));
    if (fd < 0) {
        perror("connect");
        return EXIT_FAILURE;
    }

    for (;;) {
        uint8_t batch[256];
        size_t frame_lengths[2] = { 0, 0 };
        size_t frame_count = options.glue ? 2 : 1;
        size_t batch_length = 0;
        size_t frame_index;
        struct timespec delay = {
            .tv_sec = (time_t)(cycle_ms / 1000U),
            .tv_nsec = (long)(cycle_ms % 1000U) * 1000000L
        };

        for (frame_index = 0; frame_index < frame_count; ++frame_index) {
            int corrupt = options.corrupt && sequence % 5U == 4U;
            frame_lengths[frame_index] = build_frame(
                type, (uint16_t)id, sequence,
                corrupt && type != NODE_SMOKE_GAS, has_override,
                override_value, batch + batch_length,
                sizeof(batch) - batch_length);
            if (frame_lengths[frame_index] == 0) {
                (void)close(fd);
                return EXIT_FAILURE;
            }
            if (corrupt && type == NODE_SMOKE_GAS) {
                batch[batch_length + frame_lengths[frame_index] - 1] ^= 0x01;
            }
            batch_length += frame_lengths[frame_index];
            ++sequence;
        }

        if (options.fragment && !options.glue) {
            if (send_fragmented(fd, batch, batch_length) != 0) break;
        } else if (send_all(fd, batch, batch_length) != 0) {
            break;
        }

        if (type == NODE_SMOKE_GAS) {
            size_t offset = 0;
            for (frame_index = 0; frame_index < frame_count; ++frame_index) {
                int attempt;
                for (attempt = 0; attempt < 3; ++attempt) {
                    size_t frame_length = frame_lengths[frame_index];
                    int reply;
                    if (attempt > 0 && send_all(fd, batch + offset,
                                               frame_length) != 0) {
                        goto finished;
                    }
                    reply = await_sgp_reply(fd);
                    if (reply == 1) break;
                    if (reply < 0 && errno != ETIMEDOUT) goto finished;
                }
                if (attempt == 3) goto finished;
                offset += frame_lengths[frame_index];
            }
        }
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
        }
    }

finished:
    (void)close(fd);
    return EXIT_FAILURE;
}
