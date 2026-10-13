#include <stdio.h>
#include <string.h>

#include "protocol_adapter.h"

static int check_valid(size_t adapter_index, const uint8_t *frame,
                       size_t frame_len, sensor_type_t type, sensor_id_t id)
{
    sensor_data_t data = { 0 };
    size_t consumed = 0;
    parse_result_t result = protocol_adapters[adapter_index].parse(
        frame, frame_len, &data, &consumed);

    if (result != PARSE_OK || consumed != frame_len || data.type != type ||
        data.id != id) {
        fprintf(stderr, "FAIL %s valid frame\n",
                protocol_adapters[adapter_index].name);
        return 1;
    }
    return 0;
}

static int check_need_more(size_t adapter_index, const uint8_t *frame,
                           size_t partial_len)
{
    sensor_data_t data = { 0 };
    size_t consumed = 99;
    parse_result_t result = protocol_adapters[adapter_index].parse(
        frame, partial_len, &data, &consumed);

    if (result != PARSE_NEED_MORE) {
        fprintf(stderr, "FAIL %s incomplete frame\n",
                protocol_adapters[adapter_index].name);
        return 1;
    }
    return 0;
}

int main(void)
{
    static const uint8_t tbp[] = {
        0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x35, 0x40,
        0x00, 0xB9, 0x55, 0x69, 0x00, 0x00, 0x00, 0x00
    };
    static const uint8_t hlp[] = "HUM 21 54.3 1767225600\n";
    static const uint8_t hlp_partial[] = "HUM 21 54.3";
    static const uint8_t motion[] = { 0x01, 0x06, 0x00, 0x1E,
                                      0x69, 0x55, 0xB9, 0x00 };
    static const uint8_t door[] = { 0x02, 0x07, 0x00, 0x28, 0x01,
                                    0x69, 0x55, 0xB9, 0x00 };
    static const uint8_t sgp[] = { 0x7E, 0x00, 0x32, 0x00, 0x78,
                                   0x69, 0x55, 0xB9, 0x00, 0xAC, 0xF5 };
    static const uint8_t crc_text[] = "123456789";
    static const uint8_t bad_hlp[] = "HUM 21 54.3 nope\n";
    uint8_t bad_sgp[sizeof(sgp)];
    uint8_t reply[2];
    sensor_data_t data = { 0 };
    size_t consumed = 0;
    size_t reply_len;
    int failures = 0;

    failures += check_valid(0, tbp, sizeof(tbp), SENSOR_TEMPERATURE, 10);
    failures += check_valid(1, hlp, sizeof(hlp) - 1, SENSOR_HUMIDITY, 21);
    failures += check_valid(2, motion, sizeof(motion), SENSOR_MOTION, 30);
    failures += check_valid(2, door, sizeof(door), SENSOR_DOOR, 40);
    failures += check_valid(3, sgp, sizeof(sgp), SENSOR_SMOKE_GAS, 50);

    failures += check_need_more(0, tbp, sizeof(tbp) - 1);
    failures += check_need_more(1, hlp_partial, sizeof(hlp_partial) - 1);
    failures += check_need_more(2, motion, 1);
    failures += check_need_more(3, sgp, sizeof(sgp) - 1);

    if (protocol_adapters[1].parse(bad_hlp, sizeof(bad_hlp) - 1, &data,
                                   &consumed) != PARSE_ERROR ||
        consumed != sizeof(bad_hlp) - 1) {
        fprintf(stderr, "FAIL HLP malformed line\n");
        ++failures;
    }
    memcpy(bad_sgp, sgp, sizeof(sgp));
    bad_sgp[10] ^= 1;
    consumed = 0;
    if (protocol_adapters[3].parse(bad_sgp, sizeof(bad_sgp), &data,
                                   &consumed) != PARSE_ERROR ||
        consumed != sizeof(bad_sgp)) {
        fprintf(stderr, "FAIL SGP bad CRC\n");
        ++failures;
    }
    reply_len = protocol_adapters[3].build_reply(PARSE_ERROR, reply,
                                                 sizeof(reply));
    if (reply_len != 1 || reply[0] != 0x15) {
        fprintf(stderr, "FAIL SGP NAK reply\n");
        ++failures;
    }
    reply_len = protocol_adapters[3].build_reply(PARSE_OK, reply,
                                                 sizeof(reply));
    if (reply_len != 1 || reply[0] != 0x06) {
        fprintf(stderr, "FAIL SGP ACK reply\n");
        ++failures;
    }
    if (protocol_crc16_ccitt_false(crc_text, sizeof(crc_text) - 1) != 0x29B1) {
        fprintf(stderr, "FAIL CRC-16/CCITT-FALSE check vector\n");
        ++failures;
    }
    if (failures != 0) {
        fprintf(stderr, "%d protocol adapter check(s) failed\n", failures);
        return 1;
    }
    puts("All protocol adapter checks passed.");
    return 0;
}
