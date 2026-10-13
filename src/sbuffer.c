#define _POSIX_C_SOURCE 200809L

#include "sbuffer.h"

#include <errno.h>
#include <stdlib.h>

static int mutex_lock(pthread_mutex_t *lock)
{
    int result = pthread_mutex_lock(lock);
    if (result != 0) {
        errno = result;
        return -1;
    }
    return 0;
}

static int mutex_unlock(pthread_mutex_t *lock)
{
    int result = pthread_mutex_unlock(lock);
    if (result != 0) {
        errno = result;
        return -1;
    }
    return 0;
}

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
    if (mutex_lock(&buffer->lock) != 0) {
        free(node);
        return -1;
    }
    if (buffer->shutdown) {
        (void)mutex_unlock(&buffer->lock);
        free(node);
        errno = ESHUTDOWN;
        return -1;
    }
    if (buffer->size >= SBUFFER_MAX_ITEMS) {
        (void)mutex_unlock(&buffer->lock);
        free(node);
        errno = ENOBUFS;
        return -1;
    }
    if (buffer->next_sequence == UINT64_MAX) {
        (void)mutex_unlock(&buffer->lock);
        free(node);
        errno = EOVERFLOW;
        return -1;
    }
    node->sequence = ++buffer->next_sequence;
    if (buffer->tail == NULL) {
        buffer->head = node;
    } else {
        buffer->tail->next = node;
    }
    buffer->tail = node;
    ++buffer->size;
    {
        int result = pthread_cond_broadcast(&buffer->not_empty);
        int unlock_result = mutex_unlock(&buffer->lock);
        if (result != 0) {
            errno = result;
            return -1;
        }
        if (unlock_result != 0) {
            return -1;
        }
    }
    return 0;
}

int sbuffer_read(sbuffer_t *buffer, sbuffer_reader_t reader,
                 sensor_data_t *data, uint64_t *sequence)
{
    sbuffer_node_t *node;
    bool *read_flag;

    if (buffer == NULL || data == NULL || sequence == NULL ||
        (reader != SBUFFER_READER_DATA && reader != SBUFFER_READER_STORAGE)) {
        errno = EINVAL;
        return -1;
    }
    if (mutex_lock(&buffer->lock) != 0) {
        return -1;
    }
    for (;;) {
        for (node = buffer->head; node != NULL; node = node->next) {
            read_flag = reader == SBUFFER_READER_DATA ? &node->read_by_data :
                                                       &node->read_by_storage;
            bool *pending_flag = reader == SBUFFER_READER_DATA ?
                                     &node->pending_data : &node->pending_storage;
            if (!*read_flag && !*pending_flag) {
                *data = node->data;
                *sequence = node->sequence;
                *pending_flag = true;
                (void)mutex_unlock(&buffer->lock);
                return 1;
            }
        }
        if (buffer->shutdown) {
            (void)mutex_unlock(&buffer->lock);
            return 0;
        }
        {
            int result = pthread_cond_wait(&buffer->not_empty, &buffer->lock);
            if (result != 0) {
                (void)mutex_unlock(&buffer->lock);
                errno = result;
                return -1;
            }
        }
    }
}

int sbuffer_ack(sbuffer_t *buffer, sbuffer_reader_t reader, uint64_t sequence)
{
    sbuffer_node_t *node;
    bool *read_flag;

    if (buffer == NULL || sequence == 0 ||
        (reader != SBUFFER_READER_DATA && reader != SBUFFER_READER_STORAGE)) {
        errno = EINVAL;
        return -1;
    }
    if (mutex_lock(&buffer->lock) != 0) {
        return -1;
    }
    for (node = buffer->head; node != NULL; node = node->next) {
        if (node->sequence == sequence) {
            read_flag = reader == SBUFFER_READER_DATA ? &node->read_by_data :
                                                       &node->read_by_storage;
            bool *pending_flag = reader == SBUFFER_READER_DATA ?
                                     &node->pending_data : &node->pending_storage;
            if (*read_flag) {
                (void)mutex_unlock(&buffer->lock);
                errno = EALREADY;
                return -1;
            }
            if (!*pending_flag) {
                (void)mutex_unlock(&buffer->lock);
                errno = EPERM;
                return -1;
            }
            *read_flag = true;
            if (reader == SBUFFER_READER_DATA) {
                node->pending_data = false;
            } else {
                node->pending_storage = false;
            }
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
            {
                int result = pthread_cond_broadcast(&buffer->not_empty);
                int unlock_result = mutex_unlock(&buffer->lock);
                if (result != 0) {
                    errno = result;
                    return -1;
                }
                if (unlock_result != 0) {
                    return -1;
                }
            }
            return 0;
        }
    }
    (void)mutex_unlock(&buffer->lock);
    errno = ENOENT;
    return -1;
}

int sbuffer_shutdown(sbuffer_t *buffer)
{
    int result;
    int unlock_result;

    if (buffer == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (mutex_lock(&buffer->lock) != 0) {
        return -1;
    }
    buffer->shutdown = true;
    result = pthread_cond_broadcast(&buffer->not_empty);
    unlock_result = mutex_unlock(&buffer->lock);
    if (result != 0) {
        errno = result;
        return -1;
    }
    return unlock_result;
}

int sbuffer_destroy(sbuffer_t *buffer)
{
    sbuffer_node_t *node;
    int result;

    if (buffer == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (mutex_lock(&buffer->lock) != 0) {
        return -1;
    }
    buffer->shutdown = true;
    result = pthread_cond_broadcast(&buffer->not_empty);
    if (mutex_unlock(&buffer->lock) != 0) {
        return -1;
    }
    if (result != 0) {
        errno = result;
        return -1;
    }
    result = pthread_cond_destroy(&buffer->not_empty);
    if (result != 0) {
        errno = result;
        return -1;
    }
    result = pthread_mutex_destroy(&buffer->lock);
    if (result != 0) {
        errno = result;
        return -1;
    }
    node = buffer->head;
    while (node != NULL) {
        sbuffer_node_t *next = node->next;
        free(node);
        node = next;
    }
    return 0;
}
