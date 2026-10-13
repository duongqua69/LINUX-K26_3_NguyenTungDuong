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

#ifdef DEBUG
#define DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINT(...) ((void)0)
#endif

#define MANAGER_COUNT 3

typedef struct {
    gateway_t *gateway;
    const char *name;
} manager_thread_arg_t;

static gateway_t gateway = {
    .stop_lock = PTHREAD_MUTEX_INITIALIZER,
    .stop_cond = PTHREAD_COND_INITIALIZER
};

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
    gateway.stop = 1;
}

static void *manager_thread(void *argument)
{
    const manager_thread_arg_t *thread_arg = argument;

    DEBUG_PRINT("%s thread started\n", thread_arg->name);

    (void)pthread_mutex_lock(&thread_arg->gateway->stop_lock);
    while (thread_arg->gateway->workers_stop == 0) {
        (void)pthread_cond_wait(&thread_arg->gateway->stop_cond,
                                &thread_arg->gateway->stop_lock);
    }
    (void)pthread_mutex_unlock(&thread_arg->gateway->stop_lock);

    DEBUG_PRINT("%s thread stopped\n", thread_arg->name);
    return NULL;
}

static void wake_manager_threads(int request_stop)
{
    (void)pthread_mutex_lock(&gateway.stop_lock);
    if (request_stop != 0) {
        gateway.stop = 1;
    }
    gateway.workers_stop = 1;
    (void)pthread_cond_broadcast(&gateway.stop_cond);
    (void)pthread_mutex_unlock(&gateway.stop_lock);
}

int main(int argc, char *argv[])
{
    pthread_t threads[MANAGER_COUNT];
    manager_thread_arg_t thread_args[MANAGER_COUNT] = {
        { &gateway, "Connection manager" },
        { &gateway, "Data manager" },
        { &gateway, "Storage manager" }
    };
    const char *thread_names[MANAGER_COUNT] = {
        "Connection manager",
        "Data manager",
        "Storage manager"
    };
    struct sigaction action = { 0 };
    sigset_t termination_signals;
    sigset_t previous_mask;
    size_t threads_created = 0;
    size_t i;
    int exit_status = EXIT_SUCCESS;

    if (argc != 3 || parse_port(argv[1], &gateway.base_port) != 0 ||
        parse_port(argv[2], &gateway.http_port) != 0 ||
        gateway.base_port > UINT16_MAX - 3 ||
        (gateway.http_port >= gateway.base_port &&
         gateway.http_port <= gateway.base_port + 3)) {
        fprintf(stderr, "Usage: %s <base-port> <http-port>\n", argv[0]);
        fprintf(stderr,
                "Ports must be in 1..65535; base-port through base-port+3 "
                "must be in range and must not include http-port.\n");
        return EXIT_FAILURE;
    }

    sigemptyset(&termination_signals);
    sigaddset(&termination_signals, SIGINT);
    sigaddset(&termination_signals, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &termination_signals, &previous_mask) != 0) {
        perror("sigprocmask");
        return EXIT_FAILURE;
    }

    action.sa_handler = handle_termination_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        perror("sigaction");
        (void)sigprocmask(SIG_SETMASK, &previous_mask, NULL);
        return EXIT_FAILURE;
    }

    printf("Gateway started …\n");
    DEBUG_PRINT("Base port: %u, HTTP port: %u\n",
                (unsigned int)gateway.base_port,
                (unsigned int)gateway.http_port);

    for (i = 0; i < MANAGER_COUNT; ++i) {
        int result = pthread_create(&threads[i], NULL, manager_thread,
                                   &thread_args[i]);
        if (result != 0) {
            fprintf(stderr, "Could not start %s thread: %s\n",
                    thread_names[i], strerror(result));
            wake_manager_threads(1);
            break;
        }
        ++threads_created;
    }

    if (threads_created == MANAGER_COUNT) {
        while (gateway.stop == 0) {
            (void)sigsuspend(&previous_mask);
        }
        wake_manager_threads(0);
    }

    (void)sigprocmask(SIG_SETMASK, &previous_mask, NULL);
    for (i = 0; i < threads_created; ++i) {
        int result = pthread_join(threads[i], NULL);
        if (result != 0) {
            fprintf(stderr, "Could not join %s thread: %s\n",
                    thread_names[i], strerror(result));
            exit_status = EXIT_FAILURE;
        }
    }

    printf("Gateway shutting down …\n");
    if (threads_created != MANAGER_COUNT) {
        exit_status = EXIT_FAILURE;
    }
    return exit_status;
}
