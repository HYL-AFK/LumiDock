#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define STC_PROTOCOL_FRAME_LEN_REQUEST 12U
#define STC_PROTOCOL_FRAME_LEN_RESPONSE 21U
#define STC_PROTOCOL_HEADER_BYTE_0 0x53U
#define STC_PROTOCOL_HEADER_BYTE_1 0x54U
#define STC_PROTOCOL_TAIL_BYTE_0 0x43U
#define STC_PROTOCOL_TAIL_BYTE_1 0x21U
#define STC_PROTOCOL_REQUEST_LENGTH_BYTE 0x0CU
#define STC_PROTOCOL_RESPONSE_LENGTH_BYTE 0x15U

typedef struct {
    uint32_t timestamp_ms;
    uint8_t sw_protocol[3];
    uint8_t fault_code;
    uint16_t crc_calc;
    uint16_t crc_rx;
} stc_response_t;

uint16_t stc_protocol_crc16_modbus(const uint8_t *data, size_t len);
esp_err_t stc_protocol_build_request(uint32_t timestamp_ms,
                                     uint8_t frame[STC_PROTOCOL_FRAME_LEN_REQUEST]);
esp_err_t stc_protocol_parse_response(const uint8_t *frame, size_t len,
                                      stc_response_t *out);
