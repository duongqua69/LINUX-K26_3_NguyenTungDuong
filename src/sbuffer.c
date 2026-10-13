#define _POSIX_C_SOURCE 200809L

#include "sbuffer.h"

#include <errno.h>
#include <stdlib.h>

int sbuffer_init(sbuffer_t *buffer)
{
    int result;

    if (buffer == NULL) {
        errno = EINVAL;
        return -1;
    }
    *buffer = (sbuffer_t){ 0 };
    result = pthread_mutex_init(&buffer->lock, NULL);
    if (result != 0) {
        errno = result;
        return -1;
    }
    result = pthread_cond_init(&buffer->not_empty, NULL);
    if (result != 0) {
        (void)pthread_mutex_destroy(&buffer->lock);
        errno = result;
        return -1;
    }
    return 0;
}

int sbuffer_insert(sbuffer_t *buffer, const sensor_data_t *data)
{
    sbuffer_node_t *node;

    if (buffer == NULL || data == NULL) {
        errno = EINVAL;
        return -1;
    }
    node = calloc(1, sizeof(*node));
    if (node == NULL) {
        return -1;
    }
    node->data = *data;
    (void)pthread_mutex_lock(&buffer->lock);
    if (buffer->shutdown) {
        (void)pthread_mutex_unlock(&buffer->lock);
        free(node);
        errno = ESHUTDOWN;
        return -1;
    }
    if (buffer->tail == NULL) {
        buffer->head = node;
    } else {
        buffer->tail->next = node;
    }
    buffer->tail = node;
    ++buffer->size;
    (void)pthread_cond_broadcast(&buffer->not_empty);
    (void)pthread_mutex_unlock(&buffer->lock);
    return 0;
}

int sbuffer_read(sbuffer_t *buffer, sbuffer_reader_t reader,
                 sensor_data_t *data)
{
    sbuffer_node_t *node;
    bool *read_flag;

    if (buffer == NULL || data == NULL ||
        (reader != SBUFFER_READER_DATA && reader != SBUFFER_READER_STORAGE)) {
        errno = EINVAL;
        return -1;
    }
    (void)pthread_mutex_lock(&buffer->lock);
    for (;;) {
        for (node = buffer->head; node != NULL; node = node->next) {
            read_flag = reader == SBUFFER_READER_DATA ? &node->read_by_data :
                                                       &node->read_by_storage;
            if (!*read_flag) {
                *data = node->data;
                *read_flag = true;
                while (buffer->head != NULL && buffer->head->read_by_data &&
                       buffer->head->read_by_storage) {
                    sbuffer_node_t *consumed = buffer->head;
                    buffer->head = consumed->next;
                    if (buffer->head == NULL) {
                        buffer->tail = NULL;
                    }
                    --buffer->size;
                    free(consumed);
                }
                (void)pthread_mutex_unlock(&buffer->lock);
                return 1;
            }
        }
        if (buffer->shutdown) {
            (void)pthread_mutex_unlock(&buffer->lock);
            return 0;
        }
        (void)pthread_cond_wait(&buffer->not_empty, &buffer->lock);
    }
}

void sbuffer_shutdown(sbuffer_t *buffer)
{
    (void)pthread_mutex_lock(&buffer->lock);
    buffer->shutdown = true;
    (void)pthread_cond_broadcast(&buffer->not_empty);
    (void)pthread_mutex_unlock(&buffer->lock);
}

void sbuffer_destroy(sbuffer_t *buffer)
{
    sbuffer_node_t *node = buffer->head;

    while (node != NULL) {
        sbuffer_node_t *next = node->next;
        free(node);
        node = next;
    }
    (void)pthread_cond_destroy(&buffer->not_empty);
    (void)pthread_mutex_destroy(&buffer->lock);
}
