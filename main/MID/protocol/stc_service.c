#include "stc_service.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "stc_protocol.h"
#include "watchdog_service.h"

#define UART_BAUD_RATE 9600
#define STC_RX_BUF_SIZE 256
#define STC_FRAME_BUFFER_SIZE 64
#define STC_REQUEST_INTERVAL_MS 1000U
#define STC_TIMEOUT_MS 5000U

static const char *TAG = "stc_service";
static stc_snapshot_t s_snapshot;
static uint8_t s_rx_frame[STC_FRAME_BUFFER_SIZE];
static size_t s_rx_len;
static TaskHandle_t s_task;

static void publish(void) { mid_snapshot_set_stc(&s_snapshot); }

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
            memcpy(s_snapshot.sw_protocol, response.sw_protocol, sizeof(response.sw_protocol));
            s_snapshot.fault_code = response.fault_code;
            s_snapshot.parsed_frames++;
        } else if (err == ESP_ERR_INVALID_CRC) {
            s_snapshot.crc_error_count++;
        } else {
            s_snapshot.parse_error_count++;
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
            uint8_t request[STC_PROTOCOL_FRAME_LEN_REQUEST];
            if (stc_protocol_build_request((uint32_t)(now / 1000), request) == ESP_OK) {
                (void)uart_write_bytes(LUMIDOCK_STC_UART_PORT, request, sizeof(request));
                (void)uart_wait_tx_done(LUMIDOCK_STC_UART_PORT, pdMS_TO_TICKS(100));
            }
            last_request_us = now;
        }
        int length = uart_read_bytes(LUMIDOCK_STC_UART_PORT, chunk, sizeof(chunk), pdMS_TO_TICKS(100));
        if (length > 0) {
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
    if (xTaskCreate(stc_task, "stc_poll", 3072, NULL, 5, &s_task) != pdPASS) return ESP_ERR_NO_MEM;
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
