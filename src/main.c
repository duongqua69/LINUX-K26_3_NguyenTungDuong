#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gateway.h"
#include "connmgr.h"
#include "logger.h"

#ifdef DEBUG
#define DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINT(...) ((void)0)
#endif

#define MANAGER_COUNT 3
#define LOG_MESSAGE_BUFFER_SIZE 200

static gateway_t gateway = {
    .stop_lock = PTHREAD_MUTEX_INITIALIZER,
    .log_fd = -1,
    .log_lock = PTHREAD_MUTEX_INITIALIZER,
    .log_pid = -1
};

/* Parse a decimal TCP port and reject empty, trailing, zero, or out-of-range input. */
static int parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || text[0] == '\0' || port == NULL) {
        return -1;
    }

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 ||
        value > UINT16_MAX) {
        return -1;
    }

    *port = (uint16_t)value;
    return 0;
}

static void handle_termination_signal(int signal_number)
{
    (void)signal_number;
    /* Defer thread coordination and cleanup to the main thread. */
    gateway.stop = 1;
}

static void wake_manager_threads(int request_stop)
{
    /* workers_stop is accessed only while stop_lock is held. */
    (void)pthread_mutex_lock(&gateway.stop_lock);
    if (request_stop != 0) {
        gateway.stop = 1;
    }
    gateway.workers_stop = 1;
    (void)pthread_mutex_unlock(&gateway.stop_lock);
}

int main(int argc, char *argv[])
{
    pthread_t threads[MANAGER_COUNT];
    void *(*thread_functions[MANAGER_COUNT])(void *) = {
        connection_manager_thread, data_manager_thread, storage_manager_thread
    };
    const char *thread_names[MANAGER_COUNT] = {
        "Connection manager",
        "Data manager",
        "Storage manager"
    };
    struct sigaction action = { 0 };
    sigset_t termination_signals;
    sigset_t previous_mask;
    char log_message[LOG_MESSAGE_BUFFER_SIZE];
    size_t threads_created = 0;
    size_t i;
    int exit_status = EXIT_SUCCESS;

    /* Step 3 accepts a base port alone; preserve the earlier optional HTTP port. */
    if ((argc != 2 && argc != 3) ||
        parse_port(argv[1], &gateway.base_port) != 0 ||
        (argc == 3 && parse_port(argv[2], &gateway.http_port) != 0) ||
        gateway.base_port > UINT16_MAX - 3 ||
        (argc == 3 && gateway.http_port >= gateway.base_port &&
         gateway.http_port <= gateway.base_port + 3)) {
        fprintf(stderr, "Usage: %s <base-port> [http-port]\n", argv[0]);
        fprintf(stderr,
                "Ports must be in 1..65535; base-port through base-port+3 "
                "must be in range.\n");
        return EXIT_FAILURE;
    }
    if (argc == 2) {
        gateway.http_port = 8080;
    }
    if (sbuffer_init(&gateway.sbuffer) != 0) {
        perror("sbuffer_init");
        return EXIT_FAILURE;
    }

    /* Block termination signals so only the main thread handles them in sigsuspend. */
    sigemptyset(&termination_signals);
    sigaddset(&termination_signals, SIGINT);
    sigaddset(&termination_signals, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &termination_signals, &previous_mask) != 0) {
        perror("sigprocmask");
        sbuffer_destroy(&gateway.sbuffer);
        return EXIT_FAILURE;
    }

    action.sa_handler = handle_termination_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        perror("sigaction");
        (void)sigprocmask(SIG_SETMASK, &previous_mask, NULL);
        sbuffer_destroy(&gateway.sbuffer);
        return EXIT_FAILURE;
    }

    /* Start the log process before any thread can emit an event. */
    if (logger_start(&gateway) != 0) {
        perror("logger_start");
        (void)sigprocmask(SIG_SETMASK, &previous_mask, NULL);
        sbuffer_destroy(&gateway.sbuffer);
        return EXIT_FAILURE;
    }

    (void)snprintf(log_message, sizeof(log_message),
                   "Gateway started (TCP base port %u, HTTP port %u)",
                   (unsigned int)gateway.base_port,
                   (unsigned int)gateway.http_port);
    if (logger_write(&gateway, "MAIN", log_message) != 0) {
        perror("logger_write");
        (void)logger_stop(&gateway);
        (void)sigprocmask(SIG_SETMASK, &previous_mask, NULL);
        sbuffer_destroy(&gateway.sbuffer);
        return EXIT_FAILURE;
    }

    printf("Gateway started …\n");
    DEBUG_PRINT("Base port: %u, HTTP port: %u\n",
                (unsigned int)gateway.base_port,
                (unsigned int)gateway.http_port);

    /* Start the connection manager and the two independent sbuffer readers. */
    for (i = 0; i < MANAGER_COUNT; ++i) {
        int result = pthread_create(&threads[i], NULL, thread_functions[i],
                                   &gateway);
        if (result != 0) {
            fprintf(stderr, "Could not start %s thread: %s\n",
                    thread_names[i], strerror(result));
            wake_manager_threads(1);
            break;
        }
        ++threads_created;
    }

    if (threads_created == MANAGER_COUNT) {
        /* sigsuspend atomically waits while temporarily unblocking the signals. */
        while (gateway.stop == 0) {
            (void)sigsuspend(&previous_mask);
        }
        wake_manager_threads(0);
    }
    sbuffer_shutdown(&gateway.sbuffer);

    /* Join workers before writing the final event and closing the FIFO. */
    for (i = 0; i < threads_created; ++i) {
        int result = pthread_join(threads[i], NULL);
        if (result != 0) {
            fprintf(stderr, "Could not join %s thread: %s\n",
                    thread_names[i], strerror(result));
            exit_status = EXIT_FAILURE;
        }
    }

    (void)snprintf(log_message, sizeof(log_message),
                   "Gateway shutting down (%s)",
                   threads_created == MANAGER_COUNT ? "signal received" :
                                                      "thread creation failure");
    if (logger_write(&gateway, "MAIN", log_message) != 0) {
        perror("logger_write");
        exit_status = EXIT_FAILURE;
    }
    if (logger_stop(&gateway) != 0) {
        perror("logger_stop");
        exit_status = EXIT_FAILURE;
    }
    (void)sigprocmask(SIG_SETMASK, &previous_mask, NULL);

    printf("Gateway shutting down …\n");
    if (threads_created != MANAGER_COUNT) {
        exit_status = EXIT_FAILURE;
    }
    sbuffer_destroy(&gateway.sbuffer);
    return exit_status;
}
