#define _POSIX_C_SOURCE 200809L

#include "protocol_adapter.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TBP_SIZE 18U
#define HLP_MAX_SIZE 64U
#define SGP_SIZE 11U
#define SGP_ACK 0x06U
#define SGP_NAK 0x15U

static uint64_t read_le(const uint8_t *bytes, size_t length)
{
    uint64_t value = 0;
    size_t i;
    for (i = 0; i < length; ++i) {
        value |= (uint64_t)bytes[i] << (8U * i);
    }
    return value;
}

static uint64_t read_be(const uint8_t *bytes, size_t length)
{
    uint64_t value = 0;
    size_t i;
    for (i = 0; i < length; ++i) {
        value = (value << 8U) | bytes[i];
    }
    return value;
}

static int decode_double(uint64_t bits, double *value)
{
    memcpy(value, &bits, sizeof(*value));
    return isfinite(*value) ? 0 : -1;
}

static parse_result_t parse_tbp(const uint8_t *buf, size_t len,
                                sensor_data_t *out, size_t *consumed)
{
    uint64_t value_bits;
    int64_t timestamp;

    if (len < TBP_SIZE) {
        return PARSE_NEED_MORE;
    }
    *consumed = TBP_SIZE;
    out->id = (sensor_id_t)read_le(buf, 2);
    value_bits = read_le(buf + 2, 8);
    if (out->id == 0 || read_le(buf + 10, 8) > INT64_MAX ||
        decode_double(value_bits, &out->value) != 0) {
        return PARSE_ERROR;
    }
    timestamp = (int64_t)read_le(buf + 10, 8);
    out->type = SENSOR_TEMPERATURE;
    out->ts = (time_t)timestamp;
    return PARSE_OK;
}

static int parse_decimal_u64(const char *text, uint64_t max, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    if (text == NULL || text[0] == '\0' || text[0] == '-' || text[0] == '+') {
        return -1;
    }
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > max) {
        return -1;
    }
    *value = (uint64_t)parsed;
    return 0;
}

static parse_result_t parse_hlp(const uint8_t *buf, size_t len,
                                sensor_data_t *out, size_t *consumed)
{
    char line[HLP_MAX_SIZE + 1];
    char *fields[4];
    char *newline = memchr(buf, '\n', len);
    size_t line_size;
    size_t i;
    size_t field_count = 1;
    uint64_t id;
    uint64_t timestamp;
    char *value_end = NULL;

    if (newline == NULL) {
        if (len < HLP_MAX_SIZE) {
            return PARSE_NEED_MORE;
        }
        *consumed = HLP_MAX_SIZE;
        return PARSE_ERROR;
    }
    line_size = (size_t)(newline - (const char *)buf) + 1;
    *consumed = line_size;
    if (line_size > HLP_MAX_SIZE || line_size < 2 ||
        memchr(buf, '\r', line_size) != NULL) {
        return PARSE_ERROR;
    }
    memcpy(line, buf, line_size - 1);
    line[line_size - 1] = '\0';
    fields[0] = line;
    for (i = 0; i < line_size - 1; ++i) {
        if (line[i] == ' ') {
            if (i == 0 || line[i + 1] == '\0' || line[i + 1] == ' ' ||
                field_count >= 4) {
                return PARSE_ERROR;
            }
            line[i] = '\0';
            fields[field_count++] = line + i + 1;
        }
    }
    if (field_count != 4) {
        return PARSE_ERROR;
    }
    if (strcmp(fields[0], "HUM") != 0 ||
        parse_decimal_u64(fields[1], UINT16_MAX, &id) != 0 || id == 0 ||
        parse_decimal_u64(fields[3], INT64_MAX, &timestamp) != 0) {
        return PARSE_ERROR;
    }
    errno = 0;
    out->value = strtod(fields[2], &value_end);
    if (errno != 0 || value_end == fields[2] || *value_end != '\0' ||
        !isfinite(out->value)) {
        return PARSE_ERROR;
    }
    out->id = (sensor_id_t)id;
    out->type = SENSOR_HUMIDITY;
    out->ts = (time_t)timestamp;
    return PARSE_OK;
}

static parse_result_t parse_evp(const uint8_t *buf, size_t len,
                                sensor_data_t *out, size_t *consumed)
{
    uint8_t type;
    uint8_t payload_length;
    uint32_t timestamp;

    if (len < 2) {
        return PARSE_NEED_MORE;
    }
    type = buf[0];
    payload_length = buf[1];
    if ((type != 0x01 && type != 0x02) ||
        payload_length != (type == 0x01 ? 6 : 7)) {
        *consumed = 1;
        return PARSE_ERROR;
    }
    if (len < (size_t)payload_length + 2) {
        return PARSE_NEED_MORE;
    }
    *consumed = (size_t)payload_length + 2;
    out->id = (sensor_id_t)read_be(buf + 2, 2);
    if (out->id == 0) {
        return PARSE_ERROR;
    }
    if (type == 0x01) {
        out->type = SENSOR_MOTION;
        out->value = 1.0;
        timestamp = (uint32_t)read_be(buf + 4, 4);
    } else {
        if (buf[4] > 1) {
            return PARSE_ERROR;
        }
        out->type = SENSOR_DOOR;
        out->value = (double)buf[4];
        timestamp = (uint32_t)read_be(buf + 5, 4);
    }
    out->ts = (time_t)timestamp;
    return PARSE_OK;
}

uint16_t protocol_crc16_ccitt_false(const uint8_t *buf, size_t len)
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

static parse_result_t parse_sgp(const uint8_t *buf, size_t len,
                                sensor_data_t *out, size_t *consumed)
{
    uint16_t expected_crc;

    if (len == 0) {
        return PARSE_NEED_MORE;
    }
    if (buf[0] != 0x7E) {
        *consumed = 1;
        return PARSE_ERROR;
    }
    if (len < SGP_SIZE) {
        return PARSE_NEED_MORE;
    }
    *consumed = SGP_SIZE;
    expected_crc = protocol_crc16_ccitt_false(buf + 1, 8);
    if (expected_crc != read_be(buf + 9, 2)) {
        return PARSE_ERROR;
    }
    out->id = (sensor_id_t)read_be(buf + 1, 2);
    out->value = (double)read_be(buf + 3, 2);
    out->ts = (time_t)read_be(buf + 5, 4);
    if (out->id == 0) {
        return PARSE_ERROR;
    }
    out->type = SENSOR_SMOKE_GAS;
    return PARSE_OK;
}

static size_t no_reply(parse_result_t result, uint8_t *reply, size_t cap)
{
    (void)result;
    (void)reply;
    (void)cap;
    return 0;
}

static size_t sgp_reply(parse_result_t result, uint8_t *reply, size_t cap)
{
    if (cap < 1) {
        return 0;
    }
    reply[0] = result == PARSE_OK ? 0x06 : 0x15;
    return 1;
}

const protocol_adapter_t protocol_adapters[4] = {
    { "TBP", 0, parse_tbp, no_reply },
    { "HLP", 1, parse_hlp, no_reply },
    { "EVP", 2, parse_evp, no_reply },
    { "SGP", 3, parse_sgp, sgp_reply }
};
