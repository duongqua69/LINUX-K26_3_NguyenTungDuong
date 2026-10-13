#ifndef PROTOCOL_ADAPTER_H
#define PROTOCOL_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#include "sensor_types.h"

/* Result of decoding a frame at the start of the buffer. */
typedef enum {
    PARSE_ERROR = -1,     /* Invalid frame; consumed gives the number of bytes to discard for resynchronization. */
    PARSE_NEED_MORE = 0,  /* Not enough bytes yet; preserve the data and wait for more. */
    PARSE_OK = 1           /* One frame decoded; populate out and consumed. */
} parse_result_t;

/*
 * Converts a protocol frame into normalized sensor_data_t data.
 * The listening port is the base port plus port_offset. build_reply creates
 * a response when required by the protocol, or returns 0 if none is needed.
 */
typedef struct {
    const char *name;       /* Protocol name, for example TBP, HLP, EVP, or SGP. */
    uint16_t port_offset;   /* Port offset from the base port. */
    /* Decode at most one frame from the start of buf. */
    parse_result_t (*parse)(const uint8_t *buf, size_t len,
                            sensor_data_t *out, size_t *consumed);
    /* Write the response to reply and return its length in bytes. */
    size_t (*build_reply)(parse_result_t result, uint8_t *reply, size_t cap);
} protocol_adapter_t;

#endif /* PROTOCOL_ADAPTER_H */
