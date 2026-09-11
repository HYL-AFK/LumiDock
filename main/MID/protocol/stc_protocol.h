#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* 协议版本 V2.2：响应帧由 38 字节扩到 41 字节，每路遥测由 9 字节扩到 10 字节，
   新增的第 10 字节是 SW3518S REG 0x06 的协议码原值。帧布局：
     [0..1] 帧头 0x53 0x54   [2] 长度 0x29   [3..6] 时间戳
     [7..16] slot0  [17..26] slot1  [27..36] slot2
     [37..38] CRC16-MODBUS（覆盖 [0..36]）  [39..40] 帧尾 0x43 0x21 */
#define STC_PROTOCOL_FRAME_LEN_REQUEST 12U
#define STC_PROTOCOL_FRAME_LEN_RESPONSE 41U
#define STC_PROTOCOL_HEADER_BYTE_0 0x53U
#define STC_PROTOCOL_HEADER_BYTE_1 0x54U
#define STC_PROTOCOL_TAIL_BYTE_0 0x43U
#define STC_PROTOCOL_TAIL_BYTE_1 0x21U
#define STC_PROTOCOL_REQUEST_LENGTH_BYTE 0x0CU
#define STC_PROTOCOL_RESPONSE_LENGTH_BYTE 0x29U
#define STC_PROTOCOL_CHANNEL_COUNT 3U
#define STC_PROTOCOL_CHANNEL_BASE 7U
#define STC_PROTOCOL_CHANNEL_STRIDE 10U
#define STC_PROTOCOL_RESPONSE_CRC_OFFSET 37U

/* 请求帧偏移[7]：ESP32 状态码（不再是命令码）。
   电压档位的升降档判断已完全交给 STC 本地温度保护，STC 与 ESP32 之间只做通信：
   ESP32 每秒上报一次自身状态，STC 上报三路遥测。ESP32 不再下发任何档位指令。
   取值必须与 STC 端 stc_protocol_v1_fault_valid() 的白名单一致：
   0x00~0x06 表示 ESP32 正常，0xFF 表示 ESP32 自身故障；其它值会被 STC 整帧丢弃。 */
#define STC_PROTOCOL_ESP32_STATUS_OK 0x00U
#define STC_PROTOCOL_ESP32_STATUS_FAULT 0xFFU

/* 每路第 10 字节：SW3518S REG 0x06 原值，即该路当前协商到的快充协议。
   bit7    快充 LED（1 = 点亮）
   bit5-4  PD 版本（1 = PD2.0，2 = PD3.0）
   bit3-0  协议类型，取值见下面的 STC_PROTOCOL_TYPE_* */
#define STC_PROTOCOL_CODE_FAST_LED 0x80U
#define STC_PROTOCOL_CODE_PD_VER_SHIFT 4U
#define STC_PROTOCOL_CODE_PD_VER_MASK 0x03U
#define STC_PROTOCOL_CODE_TYPE_MASK 0x0FU

#define STC_PROTOCOL_PD_VER_NONE 0x00U
#define STC_PROTOCOL_PD_VER_2_0 0x01U
#define STC_PROTOCOL_PD_VER_3_0 0x02U

/* bit3-0 的协议类型。status 里那个“快充”位只是有无快充的简化布尔，
   要区分具体协议请用这个字节，不要再拿它当类型用。 */
#define STC_PROTOCOL_TYPE_NONE 0x00U
#define STC_PROTOCOL_TYPE_QC2_0 0x01U
#define STC_PROTOCOL_TYPE_QC3_0 0x02U
#define STC_PROTOCOL_TYPE_FCP 0x03U
#define STC_PROTOCOL_TYPE_SCP 0x04U
#define STC_PROTOCOL_TYPE_PD_FIX 0x05U
#define STC_PROTOCOL_TYPE_PD_PPS 0x06U
#define STC_PROTOCOL_TYPE_PE1_1 0x07U
#define STC_PROTOCOL_TYPE_PE2_0 0x08U
#define STC_PROTOCOL_TYPE_VOOC 0x09U
#define STC_PROTOCOL_TYPE_SFCP 0x0AU
#define STC_PROTOCOL_TYPE_AFC 0x0BU

typedef struct {
    uint16_t vout_mv;
    uint16_t iout_c_ma;
    uint16_t iout_a_ma;
    int16_t temperature_tenths;
    uint8_t status;
    /* V2.2 新增：该路协商到的快充协议码，按 STC_PROTOCOL_TYPE_* 解释。 */
    uint8_t protocol_code;
} stc_channel_t;

typedef struct {
    uint32_t timestamp_ms;
    stc_channel_t channels[STC_PROTOCOL_CHANNEL_COUNT];
    uint16_t crc_calc;
    uint16_t crc_rx;
} stc_response_t;

uint16_t stc_protocol_crc16_modbus(const uint8_t *data, size_t len);
esp_err_t stc_protocol_build_request(uint32_t timestamp_ms,
                                     uint8_t esp32_status,
                                     uint8_t frame[STC_PROTOCOL_FRAME_LEN_REQUEST]);
esp_err_t stc_protocol_parse_response(const uint8_t *frame, size_t len,
                                      stc_response_t *out);
