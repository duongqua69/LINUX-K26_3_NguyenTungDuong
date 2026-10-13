#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define TBP_FRAME_SIZE 18

static int parse_unsigned(const char *text, unsigned long maximum,
                          unsigned long *value)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0 ||
        parsed > maximum) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static void encode_little_endian(uint8_t *output, uint64_t value, size_t size)
{
    size_t i;

    for (i = 0; i < size; ++i) {
        output[i] = (uint8_t)(value >> (8U * i));
    }
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

int main(int argc, char *argv[])
{
    struct addrinfo hints = { 0 };
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    unsigned long sensor_id;
    unsigned long base_port;
    unsigned long cycle_ms;
    int fd = -1;
    int result;

    if (argc != 6 || strcmp(argv[1], "temperature") != 0 ||
        parse_unsigned(argv[2], UINT16_MAX, &sensor_id) != 0 ||
        parse_unsigned(argv[4], UINT16_MAX - 3U, &base_port) != 0 ||
        parse_unsigned(argv[5], UINT32_MAX, &cycle_ms) != 0) {
        fprintf(stderr,
                "Usage: %s temperature <id> <host> <base-port> <cycle-ms>\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    result = getaddrinfo(argv[3], NULL, &hints, &addresses);
    if (result != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(result));
        return EXIT_FAILURE;
    }
    for (address = addresses; address != NULL; address = address->ai_next) {
        struct sockaddr_storage server_address;
        socklen_t address_length = (socklen_t)address->ai_addrlen;

        memcpy(&server_address, address->ai_addr, address->ai_addrlen);
        if (address->ai_family == AF_INET) {
            ((struct sockaddr_in *)&server_address)->sin_port =
                htons((uint16_t)base_port);
        } else if (address->ai_family == AF_INET6) {
            ((struct sockaddr_in6 *)&server_address)->sin6_port =
                htons((uint16_t)base_port);
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
    if (fd < 0) {
        perror("connect");
        return EXIT_FAILURE;
    }

    for (;;) {
        uint8_t frame[TBP_FRAME_SIZE] = { 0 };
        double temperature = 18.0 + (double)(sensor_id % 60U) / 10.0;
        int64_t timestamp = (int64_t)time(NULL);
        uint64_t value_bits;
        struct timespec delay = {
            .tv_sec = (time_t)(cycle_ms / 1000U),
            .tv_nsec = (long)(cycle_ms % 1000U) * 1000000L
        };

        memcpy(&value_bits, &temperature, sizeof(value_bits));
        encode_little_endian(frame, sensor_id, 2);
        encode_little_endian(frame + 2, value_bits, 8);
        encode_little_endian(frame + 10, (uint64_t)timestamp, 8);
        if (send_all(fd, frame, sizeof(frame)) != 0) {
            perror("send");
            (void)close(fd);
            return EXIT_FAILURE;
        }
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
        }
    }
}
