#define _POSIX_C_SOURCE 200809L

#include "logger.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LOG_FILE_PATH "gateway.log"
#define LOG_MESSAGE_MAX 200
#define LOG_SOURCE_MAX 4
#define LOG_EVENT_MAX (LOG_SOURCE_MAX + 1 + LOG_MESSAGE_MAX)
#define LOG_INPUT_BUFFER 512
#define LOG_OUTPUT_BUFFER 512

/* Accept only the source names defined by the assignment's log format. */
static int is_valid_source(const char *source, size_t length)
{
    static const char *const sources[] = {
        "MAIN", "CONN", "DATA", "STOR", "WEB"
    };
    size_t i;

    for (i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i) {
        if (strlen(sources[i]) == length &&
            memcmp(source, sources[i], length) == 0) {
            return 1;
        }
    }
    return 0;
}

static int ensure_log_fifo(const char *fifo_path)
{
    struct stat status;

    if (lstat(fifo_path, &status) == 0) {
        if (!S_ISFIFO(status.st_mode)) {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    if (errno != ENOENT) {
        return -1;
    }

    /* Create the named pipe if it does not exist; tolerate a concurrent creator. */
    if (mkfifo(fifo_path, 0666) != 0 && errno != EEXIST) {
        return -1;
    }
    if (lstat(fifo_path, &status) != 0) {
        return -1;
    }
    if (!S_ISFIFO(status.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int write_all(int fd, const char *buffer, size_t length)
{
    /* Regular-file writes may be partial, so finish writing the whole record. */
    size_t offset = 0;

    while (offset < length) {
        ssize_t written = write(fd, buffer + offset, length - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {
            errno = EIO;
            return -1;
        }
        offset += (size_t)written;
    }
    return 0;
}

static int write_log_record(int log_fd, uint64_t sequence,
                            const char *source, const char *message)
{
    char timestamp[20];
    char output[LOG_OUTPUT_BUFFER];
    struct tm local_time;
    time_t now = time(NULL);
    int output_length;

    if (localtime_r(&now, &local_time) == NULL ||
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S",
                 &local_time) == 0) {
        return -1;
    }

    /* The log process assigns sequence numbers and local timestamps. */
    output_length = snprintf(output, sizeof(output),
                             "%" PRIu64 " %s %s %s\n",
                             sequence, timestamp, source, message);
    if (output_length < 0 || (size_t)output_length >= sizeof(output)) {
        errno = EOVERFLOW;
        return -1;
    }
    return write_all(log_fd, output, (size_t)output_length);
}

static int process_log_line(int log_fd, uint64_t *sequence,
                            char *line, size_t line_length)
{
    char *separator;
    size_t source_length;

    if (line_length == 0 || line_length > LOG_EVENT_MAX) {
        return 0;
    }
    /* Split the FIFO record into its source and message fields. */
    line[line_length] = '\0';
    separator = strchr(line, ' ');
    if (separator == NULL) {
        return 0;
    }
    source_length = (size_t)(separator - line);
    if (!is_valid_source(line, source_length) || separator[1] == '\0' ||
        strlen(separator + 1) > LOG_MESSAGE_MAX) {
        return 0;
    }

    /* A private FIFO record lets the logger exit even if another writer stays open. */
    if (source_length == 4 && memcmp(line, "MAIN", 4) == 0 &&
        strcmp(separator + 1, "__LOGGER_STOP__") == 0) {
        return 1;
    }

    *separator = '\0';
    ++(*sequence);
    if (write_log_record(log_fd, *sequence, line, separator + 1) != 0) {
        return -1;
    }
    return 0;
}

static int log_process_main(int fifo_fd, int log_fd)
{
    char input[LOG_INPUT_BUFFER];
    char line[LOG_EVENT_MAX + 1];
    size_t line_length = 0;
    int discard_line = 0;
    uint64_t sequence = 0;

    /* read() can return partial or multiple newline-delimited events. */
    for (;;) {
        ssize_t bytes_read = read(fifo_fd, input, sizeof(input));
        ssize_t i;

        if (bytes_read == 0) {
            break;
        }
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("log process read");
            return EXIT_FAILURE;
        }

        for (i = 0; i < bytes_read; ++i) {
            char byte = input[i];
            if (byte == '\n') {
                if (!discard_line) {
                    int result = process_log_line(log_fd, &sequence, line,
                                                  line_length);
                    if (result > 0) {
                        return EXIT_SUCCESS;
                    }
                    if (result < 0) {
                        perror("log process write");
                        return EXIT_FAILURE;
                    }
                }
                line_length = 0;
                discard_line = 0;
            } else if (line_length < LOG_EVENT_MAX) {
                line[line_length++] = byte;
            } else {
                discard_line = 1;
            }
        }
    }

    return EXIT_SUCCESS;
}

int logger_start(gateway_t *gateway)
{
    struct sigaction ignore_sigpipe = { 0 };
    pid_t child_pid;
    int log_fd;
    int fifo_fd;

    if (gateway == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (snprintf(gateway->log_fifo_path, sizeof(gateway->log_fifo_path),
                 "logFifo.%ld", (long)getpid()) >=
        (int)sizeof(gateway->log_fifo_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (ensure_log_fifo(gateway->log_fifo_path) != 0) {
        return -1;
    }

    /* Open the output before fork so startup failures are reported to the parent. */
    log_fd = open(LOG_FILE_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND,
                  0644);
    if (log_fd < 0) {
        return -1;
    }

    ignore_sigpipe.sa_handler = SIG_IGN;
    sigemptyset(&ignore_sigpipe.sa_mask);
    if (sigaction(SIGPIPE, &ignore_sigpipe, NULL) != 0) {
        close(log_fd);
        (void)unlink(gateway->log_fifo_path);
        return -1;
    }

    /* Fork before worker threads exist; the child becomes the sole log-file writer. */
    child_pid = fork();
    if (child_pid < 0) {
        close(log_fd);
        (void)unlink(gateway->log_fifo_path);
        return -1;
    }
    if (child_pid == 0) {
        int child_fifo_fd = open(gateway->log_fifo_path, O_RDONLY);
        int child_status;

        if (child_fifo_fd < 0) {
            perror("log process open FIFO");
            close(log_fd);
            _exit(EXIT_FAILURE);
        }
        child_status = log_process_main(child_fifo_fd, log_fd);
        close(child_fifo_fd);
        close(log_fd);
        _exit(child_status);
    }

    gateway->log_pid = child_pid;
    /* Opening the write end pairs with the child's blocking read-end open. */
    fifo_fd = open(gateway->log_fifo_path, O_WRONLY);
    close(log_fd);
    if (fifo_fd < 0) {
        int status;
        (void)waitpid(child_pid, &status, 0);
        gateway->log_pid = -1;
        (void)unlink(gateway->log_fifo_path);
        return -1;
    }
    gateway->log_fd = fifo_fd;
    return 0;
}

int logger_write(gateway_t *gateway, const char *source, const char *message)
{
    char event[LOG_EVENT_MAX + 2];
    size_t source_length;
    size_t message_length;
    size_t i;
    int event_length;
    ssize_t bytes_written;
    int lock_result;

    if (gateway == NULL || source == NULL || message == NULL) {
        errno = EINVAL;
        return -1;
    }
    source_length = strlen(source);
    message_length = strlen(message);
    if (!is_valid_source(source, source_length) || message_length == 0 ||
        message_length > LOG_MESSAGE_MAX) {
        errno = EINVAL;
        return -1;
    }
    /* Keep log messages single-line ASCII so each event has an unambiguous frame. */
    for (i = 0; i < message_length; ++i) {
        unsigned char byte = (unsigned char)message[i];
        if (byte < 0x20 || byte > 0x7e) {
            errno = EINVAL;
            return -1;
        }
    }

    event_length = snprintf(event, sizeof(event), "%s %s\n", source, message);
    if (event_length < 0 || (size_t)event_length > PIPE_BUF ||
        (size_t)event_length >= sizeof(event)) {
        errno = EMSGSIZE;
        return -1;
    }

    /* Serialize threads and send each event, no larger than PIPE_BUF, in one write. */
    lock_result = pthread_mutex_lock(&gateway->log_lock);
    if (lock_result != 0) {
        errno = lock_result;
        return -1;
    }
    do {
        bytes_written = write(gateway->log_fd, event, (size_t)event_length);
    } while (bytes_written < 0 && errno == EINTR);
    lock_result = pthread_mutex_unlock(&gateway->log_lock);
    if (lock_result != 0) {
        errno = lock_result;
        return -1;
    }
    if (bytes_written != event_length) {
        if (bytes_written >= 0) {
            errno = EIO;
        }
        return -1;
    }
    return 0;
}

int logger_stop(gateway_t *gateway)
{
    static const char stop_record[] = "MAIN __LOGGER_STOP__\n";
    int status;
    int result = 0;
    pid_t waited_pid;

    if (gateway == NULL) {
        errno = EINVAL;
        return -1;
    }
    /* Closing the last writer gives the log process EOF so it can exit cleanly. */
    if (gateway->log_fd >= 0) {
        ssize_t bytes_written;

        do {
            bytes_written = write(gateway->log_fd, stop_record,
                                  sizeof(stop_record) - 1);
        } while (bytes_written < 0 && errno == EINTR);
        if (bytes_written != (ssize_t)(sizeof(stop_record) - 1)) {
            result = -1;
        }
        if (close(gateway->log_fd) != 0) {
            result = -1;
        }
        gateway->log_fd = -1;
    }
    if (gateway->log_pid <= 0) {
        return result;
    }

    do {
        waited_pid = waitpid(gateway->log_pid, &status, 0);
    } while (waited_pid < 0 && errno == EINTR);
    gateway->log_pid = -1;
    if (waited_pid < 0) {
        (void)unlink(gateway->log_fifo_path);
        return -1;
    }
    (void)unlink(gateway->log_fifo_path);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
        errno = ECHILD;
        return -1;
    }
    return result;
}
