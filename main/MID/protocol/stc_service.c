#include "stc_service.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "stc_protocol.h"
#include "watchdog_service.h"

#define UART_BAUD_RATE 9600
#define STC_RX_BUF_SIZE 256
#define STC_FRAME_BUFFER_SIZE 64
#define STC_REQUEST_INTERVAL_MS 1000U
#define STC_TIMEOUT_MS 5000U

/* 电压档位的升降档判断完全归 STC：本服务只做“查询 + 上报”。
   - 请求帧偏移[7] 只会填 ESP32 自身状态码（正常 0x00），不再携带任何档位指令。
   - 温度阈值/滞回/散热档电压都在 STC 端 stc_protocol.c 的 thermal_guard 里。
   - 这样排查时分得清：串口有帧 = 通信问题；档位不切 = STC 策略问题。 */
static const char *TAG = "stc_service";
static stc_snapshot_t s_snapshot;
static uint8_t s_rx_frame[STC_FRAME_BUFFER_SIZE];
static size_t s_rx_len;
static TaskHandle_t s_task;

/* 协议码 -> UI 短名。全 ASCII，落在 Montserrat 的 0x20-0x7E 覆盖区间内，
   所以状态栏(montserrat_14)和充电页(montserrat_20)都能直接渲染，
   不需要额外的 CJK 字形。 */
static void protocol_label(uint8_t code, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) return;
    const uint8_t type = code & STC_PROTOCOL_CODE_TYPE_MASK;
    const uint8_t pd_ver =
        (uint8_t)((code >> STC_PROTOCOL_CODE_PD_VER_SHIFT) & STC_PROTOCOL_CODE_PD_VER_MASK);
    const char *name = "NONE";
    switch (type) {
    case STC_PROTOCOL_TYPE_QC2_0: name = "QC2.0"; break;
    case STC_PROTOCOL_TYPE_QC3_0: name = "QC3.0"; break;
    case STC_PROTOCOL_TYPE_FCP: name = "FCP"; break;
    case STC_PROTOCOL_TYPE_SCP: name = "SCP"; break;
    case STC_PROTOCOL_TYPE_PD_FIX:
        /* PD 固压没有独立名字，用协商到的版本区分 PD2.0 / PD3.0。 */
        name = (pd_ver == STC_PROTOCOL_PD_VER_2_0) ? "PD2.0" : "PD3.0";
        break;
    case STC_PROTOCOL_TYPE_PD_PPS: name = "PD PPS"; break;
    case STC_PROTOCOL_TYPE_PE1_1: name = "PE1.1"; break;
    case STC_PROTOCOL_TYPE_PE2_0: name = "PE2.0"; break;
    case STC_PROTOCOL_TYPE_VOOC: name = "VOOC"; break;
    case STC_PROTOCOL_TYPE_SFCP: name = "SFCP"; break;
    case STC_PROTOCOL_TYPE_AFC: name = "AFC"; break;
    default: break;
    }
    snprintf(out, out_size, "%s", name);
}

static void log_hex(const char *label, const uint8_t *data, size_t len)
{
    char buf[128];
    size_t pos = 0U;

    for (size_t i = 0U; i < len && pos + 2U < sizeof(buf); ++i) {
        int n = snprintf(buf + pos, sizeof(buf) - pos, "%02X ", data[i]);
        if (n <= 0)
            break;
        pos += (size_t)n;
    }
    ESP_LOGI(TAG, "%s: %s", label, buf);
}

static esp_err_t stc_uart_init(void)
{
    const uart_config_t config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(LUMIDOCK_STC_UART_PORT, STC_RX_BUF_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    if ((err = uart_param_config(LUMIDOCK_STC_UART_PORT, &config)) != ESP_OK) return err;
    return uart_set_pin(LUMIDOCK_STC_UART_PORT, LUMIDOCK_STC_TX_GPIO, LUMIDOCK_STC_RX_GPIO,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

static void consume(size_t count)
{
    if (count >= s_rx_len) { s_rx_len = 0U; return; }
    memmove(s_rx_frame, s_rx_frame + count, s_rx_len - count);
    s_rx_len -= count;
}

/* 充电页与状态栏的协议标签读的是 charging 快照，而 STC 是唯一知道各路
   协商结果的数据源，所以由这里填充。只展示第一路协商到快充的口：多口同时
   快充时固定选序号最小的那路，状态栏内容才不会来回跳。
   注意 charging 的电压/电流等字段仍留空，避免在此处臆测 slot 与物理口的
   对应关系；什么时候确认了映射，什么时候再填。 */
static void publish_charging(void)
{
    charging_snapshot_t charging = {0};
    charging.valid = s_snapshot.online && s_snapshot.parsed_valid;
    int pick = -1;
    for (size_t idx = 0U; idx < (size_t)STC_PROTOCOL_CHANNEL_COUNT; ++idx) {
        if (!s_snapshot.channels[idx].valid) continue;
        if ((s_snapshot.channels[idx].protocol_code & STC_PROTOCOL_CODE_TYPE_MASK) ==
            STC_PROTOCOL_TYPE_NONE)
            continue;
        pick = (int)idx;
        break;
    }
    if (pick >= 0) {
        const uint8_t code = s_snapshot.channels[pick].protocol_code;
        protocol_label(code, charging.name, sizeof(charging.name));
        charging.pd_version = (uint8_t)((code >> STC_PROTOCOL_CODE_PD_VER_SHIFT) &
                                        STC_PROTOCOL_CODE_PD_VER_MASK);
    } else {
        /* 没有任何一路协商到快充：显示 NONE，让“只是 5V 慢充”和“快充失效”
         * 在屏幕上一眼可辨。 */
        protocol_label(STC_PROTOCOL_TYPE_NONE, charging.name, sizeof(charging.name));
    }
    mid_snapshot_set_charging(&charging);
}

static void publish(void)
{
    mid_snapshot_set_stc(&s_snapshot);
    publish_charging();
}

static void parse_frames(void)
{
    while (s_rx_len >= STC_PROTOCOL_FRAME_LEN_RESPONSE) {
        size_t header = 0U;
        while (header + 1U < s_rx_len &&
               (s_rx_frame[header] != STC_PROTOCOL_HEADER_BYTE_0 ||
                s_rx_frame[header + 1U] != STC_PROTOCOL_HEADER_BYTE_1)) {
            ++header;
        }
        if (header > 0U) { consume(header); continue; }
        stc_response_t response;
        s_snapshot.rx_frames++;
        esp_err_t err = stc_protocol_parse_response(s_rx_frame,
                                                    STC_PROTOCOL_FRAME_LEN_RESPONSE,
                                                    &response);
        if (err == ESP_OK) {
            s_snapshot.online = true;
            s_snapshot.parsed_valid = true;
            s_snapshot.timestamp_ms = response.timestamp_ms;
            for (uint8_t idx = 0U; idx < STC_PROTOCOL_CHANNEL_COUNT; ++idx) {
                stc_channel_snapshot_t *dst = &s_snapshot.channels[idx];
                dst->vout_mv = response.channels[idx].vout_mv;
                dst->iout_c_ma = response.channels[idx].iout_c_ma;
                dst->iout_a_ma = response.channels[idx].iout_a_ma;
                dst->temperature_tenths = response.channels[idx].temperature_tenths;
                dst->status = response.channels[idx].status;
                dst->protocol_code = response.channels[idx].protocol_code;
                dst->valid = (response.channels[idx].status & 0x08U) == 0U;
            }
            s_snapshot.parsed_frames++;
            ESP_LOGI(TAG, "STC parsed ts=%u", (unsigned)response.timestamp_ms);
            for (uint8_t idx = 0U; idx < STC_PROTOCOL_CHANNEL_COUNT; ++idx) {
                int16_t t = s_snapshot.channels[idx].temperature_tenths;
                int t_int = t / 10;
                int t_frac = t % 10;
                if (t_frac < 0) t_frac = -t_frac;
                const uint8_t code = s_snapshot.channels[idx].protocol_code;
                ESP_LOGI(TAG,
                         " slot%u valid=%d Vout=%u IoutC=%u IoutA=%u temp=%d.%dC status=0x%02X"
                         " proto=0x%02X type=0x%X pd=%u led=%u",
                         idx,
                         s_snapshot.channels[idx].valid ? 1 : 0,
                         (unsigned)s_snapshot.channels[idx].vout_mv,
                         (unsigned)s_snapshot.channels[idx].iout_c_ma,
                         (unsigned)s_snapshot.channels[idx].iout_a_ma,
                         t_int, t_frac,
                         (unsigned)s_snapshot.channels[idx].status,
                         (unsigned)code,
                         (unsigned)(code & STC_PROTOCOL_CODE_TYPE_MASK),
                         (unsigned)((code >> STC_PROTOCOL_CODE_PD_VER_SHIFT) &
                                    STC_PROTOCOL_CODE_PD_VER_MASK),
                         (unsigned)((code & STC_PROTOCOL_CODE_FAST_LED) ? 1U : 0U));
            }
        } else if (err == ESP_ERR_INVALID_CRC) {
            s_snapshot.crc_error_count++;
            ESP_LOGW(TAG, "STC CRC mismatch rx=%04X calc=%04X",
                     (unsigned)response.crc_rx, (unsigned)response.crc_calc);
        } else {
            s_snapshot.parse_error_count++;
            ESP_LOGW(TAG, "STC parse error (%d) len=%u lenbyte=0x%02X",
                     (int)err, (unsigned)s_rx_len,
                     (unsigned)s_rx_frame[2]);
        }
        consume(STC_PROTOCOL_FRAME_LEN_RESPONSE);
        publish();
    }
}

static void stc_task(void *arg)
{
    (void)arg;
    (void)watchdog_service_subscribe_current_task();
    uint8_t chunk[32];
    int64_t last_request_us = 0;
    int64_t last_valid_us = esp_timer_get_time();
    for (;;) {
        watchdog_service_feed();
        int64_t now = esp_timer_get_time();
        if (last_request_us == 0 || now - last_request_us >= (int64_t)STC_REQUEST_INTERVAL_MS * 1000) {
            /* 每秒一次纯查询：只带 ESP32 自身状态码，不携带任何档位指令。
               降档与否由 STC 依本地温度自行决定，ESP32 只接收结果。 */
            uint8_t request[STC_PROTOCOL_FRAME_LEN_REQUEST];
            if (stc_protocol_build_request((uint32_t)(now / 1000),
                                           STC_PROTOCOL_ESP32_STATUS_OK,
                                           request) == ESP_OK) {
                (void)uart_write_bytes(LUMIDOCK_STC_UART_PORT, request, sizeof(request));
                (void)uart_wait_tx_done(LUMIDOCK_STC_UART_PORT, pdMS_TO_TICKS(100));
                log_hex("TX req", request, sizeof(request));
            }
            last_request_us = now;
        }
        int length = uart_read_bytes(LUMIDOCK_STC_UART_PORT, chunk, sizeof(chunk), pdMS_TO_TICKS(100));
        if (length > 0) {
            log_hex("RX raw", chunk, (size_t)length);
            if (s_rx_len + (size_t)length > sizeof(s_rx_frame)) s_rx_len = 0U;
            memcpy(s_rx_frame + s_rx_len, chunk, (size_t)length);
            s_rx_len += (size_t)length;
            uint32_t parsed_before = s_snapshot.parsed_frames;
            parse_frames();
            if (s_snapshot.parsed_frames != parsed_before) last_valid_us = esp_timer_get_time();
        }
        if (esp_timer_get_time() - last_valid_us >= (int64_t)STC_TIMEOUT_MS * 1000) {
            s_snapshot.online = false;
            s_snapshot.parsed_valid = false;
            s_snapshot.timeout_count++;
            last_valid_us = esp_timer_get_time();
            publish();
        }
    }
}

esp_err_t stc_service_init(void)
{
    if (s_task != NULL) return ESP_OK;
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_rx_len = 0U;
    esp_err_t err = stc_uart_init();
    if (err != ESP_OK) return err;
    publish();
    if (xTaskCreatePinnedToCoreWithCaps(stc_task, "stc_poll", 4096, NULL, 5,
                                        &s_task, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "STC UART0 ready TX=%d RX=%d 9600 8N1", LUMIDOCK_STC_TX_GPIO, LUMIDOCK_STC_RX_GPIO);
    return ESP_OK;
}

void stc_service_poll(void) {}

bool stc_service_get_snapshot(stc_snapshot_t *out)
{
    if (out == NULL) return false;
    *out = s_snapshot;
    return s_snapshot.online && s_snapshot.parsed_valid;
}
