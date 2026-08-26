#ifndef BMS_PROTOCOL_H
#define BMS_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#include "bms_service.h"

#define BMS_PROTOCOL_REQUEST_TEXT "ESCOREDATA"
#define BMS_PROTOCOL_HEADER_TEXT "AAA0AAA"
#define BMS_PROTOCOL_HEADER_LEN 7
#define BMS_PROTOCOL_FRAME_LEN_128 128
#define BMS_PROTOCOL_FRAME_LEN_14S 130
#define BMS_PROTOCOL_FRAME_LEN_132 132
#define BMS_PROTOCOL_MAX_FRAME_LEN BMS_PROTOCOL_FRAME_LEN_132

typedef struct {
    uint16_t calc_full;
    uint16_t calc_body;
    uint16_t rx_le;
    uint16_t rx_be;
} bms_crc_result_t;

const char *bms_material_name(bms_material_t material);
size_t bms_protocol_expected_len_from_header(const uint8_t *frame, size_t len);
uint16_t bms_protocol_crc16_modbus(const uint8_t *data, size_t len);
bool bms_protocol_crc_is_valid(const uint8_t *frame, size_t len, bms_crc_result_t *out_result);
esp_err_t bms_protocol_parse(const uint8_t *frame, size_t len, bms_info_t *out_info);

#endif
