#include "bms_service.h"

#include <inttypes.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "bms_protocol.h"
#include "mid_snapshot.h"
#include "watchdog_service.h"

#define BMS_RX_BUF_SIZE 512
#define BMS_TX_BUF_SIZE 64
#define BMS_RX_CHUNK_SIZE 128
#define BMS_FRAME_BUFFER_SIZE (BMS_PROTOCOL_MAX_FRAME_LEN * 2)
#define BMS_REQUEST_INTERVAL_MS 1000
#define BMS_TIMEOUT_MS 5000

static const char *TAG = "bms_service";
static bms_info_t s_info;
static uint8_t s_frame_buffer[BMS_FRAME_BUFFER_SIZE];
static size_t s_frame_len;
static TaskHandle_t s_task;

static bool bms_values_are_safe(const bms_info_t *info)
{
    if (info == NULL || info->cell_count == 0 || info->cell_count > BMS_MAX_CELL_COUNT ||
        info->material == BMS_MATERIAL_UNKNOWN || info->pack_mv <= 0 ||
        info->rsoc_percent > 100 || info->asoc_percent > 100 || info->soh_percent > 100) {
        return false;
    }

    for (uint8_t index = 0; index < info->cell_count; ++index) {
        if (info->cell_mv[index] < 1500 || info->cell_mv[index] > 5000) return false;
    }
    return true;
}

static void publish_snapshot(void)
{
    bms_snapshot_t snapshot = {
        .online = s_info.online,
        .parsed_valid = s_info.parsed_valid,
        .pack_mv = s_info.pack_mv,
        .current_ma = s_info.current_ma,
        .soc_percent = s_info.rsoc_percent,
        .soh_percent = s_info.soh_percent,
        .charge_mos_state = s_info.charge_mos_state,
        .discharge_mos_state = s_info.discharge_mos_state,
        .protect_1 = s_info.protect_1,
        .protect_2 = s_info.protect_2,
        .protect_3 = s_info.protect_3,
        .timeout_count = s_info.timeout_count,
        .crc_error_count = s_info.crc_error_count,
    };
    mid_snapshot_set_bms(&snapshot);
}

static esp_err_t bms_uart_init(void)
{
    const uart_config_t config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(LUMIDOCK_BMS_UART_PORT,
                                        BMS_RX_BUF_SIZE,
                                        BMS_TX_BUF_SIZE,
                                        0,
                                        NULL,
                                        0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = uart_param_config(LUMIDOCK_BMS_UART_PORT, &config);
    if (err != ESP_OK) return err;
    return uart_set_pin(LUMIDOCK_BMS_UART_PORT,
                        LUMIDOCK_BMS_TX_GPIO,
                        LUMIDOCK_BMS_RX_GPIO,
                        UART_PIN_NO_CHANGE,
                        UART_PIN_NO_CHANGE);
}

static void consume_bytes(size_t count)
{
    if (count >= s_frame_len) {
        s_frame_len = 0;
        return;
    }
    memmove(s_frame_buffer, s_frame_buffer + count, s_frame_len - count);
    s_frame_len -= count;
}

static int find_header(void)
{
    for (size_t i = 0; i + BMS_PROTOCOL_HEADER_LEN <= s_frame_len; ++i) {
        if (memcmp(&s_frame_buffer[i], BMS_PROTOCOL_HEADER_TEXT, BMS_PROTOCOL_HEADER_LEN) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static size_t header_prefix_keep_len(void)
{
    size_t max_keep = s_frame_len < BMS_PROTOCOL_HEADER_LEN - 1U
                          ? s_frame_len
                          : BMS_PROTOCOL_HEADER_LEN - 1U;
    for (size_t keep = max_keep; keep > 0; --keep) {
        if (memcmp(s_frame_buffer + s_frame_len - keep, BMS_PROTOCOL_HEADER_TEXT, keep) == 0) {
            return keep;
        }
    }
    return 0;
}

static void append_rx_bytes(const uint8_t *data, size_t length)
{
    if (length > sizeof(s_frame_buffer)) {
        data += length - sizeof(s_frame_buffer);
        length = sizeof(s_frame_buffer);
        s_frame_len = 0;
    }
    if (s_frame_len + length > sizeof(s_frame_buffer)) {
        const size_t drop = s_frame_len + length - sizeof(s_frame_buffer);
        ESP_LOGW(TAG, "BMS RX buffer overflow; discard=%u", (unsigned)drop);
        consume_bytes(drop);
    }
    memcpy(s_frame_buffer + s_frame_len, data, length);
    s_frame_len += length;
}

static esp_err_t parse_frame_candidate(size_t frame_len, bms_info_t *parsed)
{
    *parsed = s_info;
    const esp_err_t err = bms_protocol_parse(s_frame_buffer, frame_len, parsed);
    if (err != ESP_OK) return err;
    if (!bms_values_are_safe(parsed)) return ESP_ERR_INVALID_RESPONSE;

    const uint32_t now_ms = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
    parsed->online = true;
    parsed->parsed_valid = true;
    parsed->rx_frames = s_info.rx_frames + 1U;
    parsed->parsed_frames = s_info.parsed_frames + 1U;
    parsed->timeout_count = s_info.timeout_count;
    parsed->crc_error_count = s_info.crc_error_count;
    parsed->parse_error_count = s_info.parse_error_count;
    parsed->last_rx_ms = now_ms;
    parsed->last_parse_ms = now_ms;
    return ESP_OK;
}

static void log_parse_error(esp_err_t err, const bms_info_t *candidate, size_t frame_len)
{
    if (err == ESP_ERR_INVALID_CRC) {
        s_info.crc_error_count++;
        ESP_LOGW(TAG, "BMS CRC fail len=%u calc=0x%04X rx_le=0x%04X rx_be=0x%04X count=%" PRIu32,
                 (unsigned)frame_len, candidate->last_crc_calc, candidate->last_crc_rx_le,
                 candidate->last_crc_rx_be, s_info.crc_error_count);
    } else {
        s_info.parse_error_count++;
        ESP_LOGW(TAG, "BMS parse fail len=%u err=%s count=%" PRIu32,
                 (unsigned)frame_len, esp_err_to_name(err), s_info.parse_error_count);
    }
}

static void accept_parsed_frame(const bms_info_t *parsed, size_t frame_len)
{
    s_info = *parsed;
    ESP_LOGI(TAG, "BMS online pack=%ldmV current=%ldmA soc=%u%%",
             (long)s_info.pack_mv, (long)s_info.current_ma, s_info.rsoc_percent);
    consume_bytes(frame_len);
    publish_snapshot();
}

static bool try_crc_locked_candidate(size_t frame_len)
{
    bms_crc_result_t crc;
    if (!bms_protocol_crc_is_valid(s_frame_buffer, frame_len, &crc)) return false;

    bms_info_t parsed;
    const esp_err_t err = parse_frame_candidate(frame_len, &parsed);
    if (err == ESP_OK) {
        accept_parsed_frame(&parsed, frame_len);
    } else {
        /* CRC 已确认帧边界，字段失败时消费整帧，不能继续拼接后续响应。 */
        log_parse_error(err, &parsed, frame_len);
        consume_bytes(frame_len);
        publish_snapshot();
    }
    return true;
}

static void process_frames(void)
{
    for (;;) {
        int header = find_header();
        if (header < 0) {
            const size_t keep = header_prefix_keep_len();
            if (keep > 0) {
                memmove(s_frame_buffer, s_frame_buffer + s_frame_len - keep, keep);
            }
            s_frame_len = keep;
            return;
        }
        if (header > 0) consume_bytes((size_t)header);

        if (s_frame_len < BMS_PROTOCOL_FRAME_LEN_128) return;
        if (try_crc_locked_candidate(BMS_PROTOCOL_FRAME_LEN_128)) continue;

        /* 128 bytes may be only the first UART segment of a 130-byte response. */
        if (s_frame_len < BMS_PROTOCOL_FRAME_LEN_14S) return;
        if (try_crc_locked_candidate(BMS_PROTOCOL_FRAME_LEN_14S)) continue;

        if (s_frame_len < BMS_PROTOCOL_FRAME_LEN_132) return;
        if (try_crc_locked_candidate(BMS_PROTOCOL_FRAME_LEN_132)) continue;

        bms_info_t failed = s_info;
        bms_crc_result_t crc;
        (void)bms_protocol_crc_is_valid(s_frame_buffer, BMS_PROTOCOL_FRAME_LEN_132, &crc);
        failed.last_crc_calc = crc.calc_body;
        failed.last_crc_rx_le = crc.rx_le;
        failed.last_crc_rx_be = crc.rx_be;
        log_parse_error(ESP_ERR_INVALID_CRC, &failed, BMS_PROTOCOL_FRAME_LEN_132);
        consume_bytes(1);
        publish_snapshot();
    }
}

static void discard_stale_response_before_request(void)
{
    if (s_frame_len > 0) {
        ESP_LOGW(TAG, "BMS previous response remainder len=%u; discarding before request",
                 (unsigned)s_frame_len);
        s_frame_len = 0;
    }
}

static void bms_send_request(void)
{
    const char request[] = BMS_PROTOCOL_REQUEST_TEXT;
    const int written = uart_write_bytes(LUMIDOCK_BMS_UART_PORT, request, sizeof(request) - 1U);
    (void)uart_wait_tx_done(LUMIDOCK_BMS_UART_PORT, pdMS_TO_TICKS(100));
    if (written != (int)(sizeof(request) - 1U)) {
        ESP_LOGW(TAG, "BMS request write failed written=%d", written);
    }
}

static void bms_task(void *arg)
{
    (void)arg;
    (void)watchdog_service_subscribe_current_task();
    uint8_t chunk[BMS_RX_CHUNK_SIZE];
    TickType_t last_request = 0;
    TickType_t last_rx = xTaskGetTickCount();

    for (;;) {
        watchdog_service_feed();
        TickType_t now = xTaskGetTickCount();
        if (last_request == 0 ||
            pdTICKS_TO_MS(now - last_request) >= BMS_REQUEST_INTERVAL_MS) {
            discard_stale_response_before_request();
            bms_send_request();
            last_request = now;
        }

        int length = uart_read_bytes(LUMIDOCK_BMS_UART_PORT,
                                     chunk,
                                     sizeof(chunk),
                                     pdMS_TO_TICKS(100));
        if (length > 0) {
            last_rx = xTaskGetTickCount();
            s_info.online = true;
            s_info.last_rx_ms = (uint32_t)pdTICKS_TO_MS(last_rx);
            append_rx_bytes(chunk, (size_t)length);
            process_frames();
        }

        if (pdTICKS_TO_MS(xTaskGetTickCount() - last_rx) >= BMS_TIMEOUT_MS) {
            s_info.online = false;
            s_info.parsed_valid = false;
            s_info.timeout_count++;
            s_frame_len = 0;
            last_rx = xTaskGetTickCount();
            ESP_LOGW(TAG, "BMS receive timeout count=%" PRIu32, s_info.timeout_count);
            publish_snapshot();
        }
    }
}

esp_err_t bms_service_init(void)
{
    memset(&s_info, 0, sizeof(s_info));
    s_frame_len = 0;
    esp_err_t err = bms_uart_init();
    if (err != ESP_OK) return err;
    if (xTaskCreate(bms_task, "bms_poll", 4096, NULL, 5, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "BMS UART2 ready TX=%d RX=%d, DE/RE unused",
             LUMIDOCK_BMS_TX_GPIO, LUMIDOCK_BMS_RX_GPIO);
    return ESP_OK;
}

bool bms_service_get_info(bms_info_t *out_info)
{
    if (out_info == NULL) return false;
    *out_info = s_info;
    return s_info.online && s_info.parsed_valid;
}

void bms_service_poll(void)
{
    /* BMS communication is owned by the background task. */
}
