#include "stc_protocol.h"

#include <string.h>

uint16_t stc_protocol_crc16_modbus(const uint8_t *data, size_t len)
{
    if (data == NULL && len != 0U) return 0U;
    uint16_t crc = 0xFFFFU;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8U; ++bit) {
            crc = (crc & 1U) ? (uint16_t)((crc >> 1U) ^ 0xA001U) : (uint16_t)(crc >> 1U);
        }
    }
    return crc;
}

static uint32_t read_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24U) | ((uint32_t)p[1] << 16U) |
           ((uint32_t)p[2] << 8U) | (uint32_t)p[3];
}

static uint16_t read_u16_be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8U) | (uint16_t)p[1]);
}

esp_err_t stc_protocol_build_request(uint32_t timestamp_ms,
                                     uint8_t esp32_status,
                                     uint8_t frame[STC_PROTOCOL_FRAME_LEN_REQUEST])
{
    if (frame == NULL) return ESP_ERR_INVALID_ARG;
    memset(frame, 0, STC_PROTOCOL_FRAME_LEN_REQUEST);
    frame[0] = STC_PROTOCOL_HEADER_BYTE_0;
    frame[1] = STC_PROTOCOL_HEADER_BYTE_1;
    frame[2] = STC_PROTOCOL_REQUEST_LENGTH_BYTE;
    frame[3] = (uint8_t)(timestamp_ms >> 24U);
    frame[4] = (uint8_t)(timestamp_ms >> 16U);
    frame[5] = (uint8_t)(timestamp_ms >> 8U);
    frame[6] = (uint8_t)timestamp_ms;
    frame[7] = esp32_status;
    uint16_t crc = stc_protocol_crc16_modbus(frame, 8U);
    frame[8] = (uint8_t)crc;
    frame[9] = (uint8_t)(crc >> 8U);
    frame[10] = STC_PROTOCOL_TAIL_BYTE_0;
    frame[11] = STC_PROTOCOL_TAIL_BYTE_1;
    return ESP_OK;
}

esp_err_t stc_protocol_parse_response(const uint8_t *frame, size_t len,
                                      stc_response_t *out)
{
    if (frame == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    if (len != STC_PROTOCOL_FRAME_LEN_RESPONSE) return ESP_ERR_INVALID_SIZE;
    if (frame[0] != STC_PROTOCOL_HEADER_BYTE_0 || frame[1] != STC_PROTOCOL_HEADER_BYTE_1 ||
        frame[2] != STC_PROTOCOL_RESPONSE_LENGTH_BYTE ||
        frame[STC_PROTOCOL_FRAME_LEN_RESPONSE - 2U] != STC_PROTOCOL_TAIL_BYTE_0 ||
        frame[STC_PROTOCOL_FRAME_LEN_RESPONSE - 1U] != STC_PROTOCOL_TAIL_BYTE_1) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint16_t crc_rx = (uint16_t)frame[STC_PROTOCOL_RESPONSE_CRC_OFFSET] |
                      ((uint16_t)frame[STC_PROTOCOL_RESPONSE_CRC_OFFSET + 1U] << 8U);
    uint16_t crc_calc = stc_protocol_crc16_modbus(frame, STC_PROTOCOL_RESPONSE_CRC_OFFSET);
    if (crc_calc != crc_rx) return ESP_ERR_INVALID_CRC;
    memset(out, 0, sizeof(*out));
    out->timestamp_ms = read_u32_be(&frame[3]);
    for (uint8_t idx = 0U; idx < STC_PROTOCOL_CHANNEL_COUNT; ++idx) {
        size_t base = STC_PROTOCOL_CHANNEL_BASE + (size_t)idx * STC_PROTOCOL_CHANNEL_STRIDE;
        stc_channel_t *ch = &out->channels[idx];
        ch->vout_mv = read_u16_be(&frame[base + 0U]);
        ch->iout_c_ma = read_u16_be(&frame[base + 2U]);
        ch->iout_a_ma = read_u16_be(&frame[base + 4U]);
        ch->temperature_tenths = (int16_t)read_u16_be(&frame[base + 6U]);
        ch->status = frame[base + 8U];
        /* V2.2 每路第 10 字节：SW3518S REG 0x06 的协议码原值。 */
        ch->protocol_code = frame[base + 9U];
    }
    out->crc_calc = crc_calc;
    out->crc_rx = crc_rx;
    return ESP_OK;
}
