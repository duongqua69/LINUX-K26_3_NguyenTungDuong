#ifndef SBUFFER_H
#define SBUFFER_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensor_types.h"

/* Bound memory use when storage is slower than incoming sensor traffic. */
#define SBUFFER_MAX_ITEMS 1024U

typedef enum {
    SBUFFER_READER_DATA,
    SBUFFER_READER_STORAGE
} sbuffer_reader_t;

typedef struct sbuffer_node {
    sensor_data_t data;
    bool read_by_data;
    bool read_by_storage;
    bool pending_data;
    bool pending_storage;
    uint64_t sequence;
    struct sbuffer_node *next;
} sbuffer_node_t;

typedef struct {
    sbuffer_node_t *head;
    sbuffer_node_t *tail;
    size_t size;
    uint64_t next_sequence;
    bool shutdown;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
} sbuffer_t;

int sbuffer_init(sbuffer_t *buffer);
int sbuffer_insert(sbuffer_t *buffer, const sensor_data_t *data);
/* Blocks until this reader has an unread item, or shutdown and queue drained. */
int sbuffer_read(sbuffer_t *buffer, sbuffer_reader_t reader,
                 sensor_data_t *data, uint64_t *sequence);
/* Mark a copied item consumed only after its manager has finished processing it. */
int sbuffer_ack(sbuffer_t *buffer, sbuffer_reader_t reader, uint64_t sequence);
int sbuffer_shutdown(sbuffer_t *buffer);
/* Call only after all threads using the buffer have exited. */
int sbuffer_destroy(sbuffer_t *buffer);

#endif
