#include "xiaozhi_activation_service.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "nvs.h"

#include "device_data.h"
#include "mid_snapshot.h"
#include "http_memory_log.h"
#include "wifi_manager.h"

/* Official xiaozhi.me OTA/registration endpoint.  The device POSTs its
 * application/board descriptors here and receives the websocket URL + token
 * (or an activation code on first binding). */
#define XIAOZHI_OTA_URL "https://api.tenclass.net/xiaozhi/ota/"
#define XIAOZHI_NVS_NAMESPACE "xiaozhi"
#define XIAOZHI_NVS_CLIENT_ID "client_id"
#define XIAOZHI_CLIENT_ID_LEN 37U
#define XIAOZHI_HTTP_BODY_MAX 2048U
#define XIAOZHI_RETRY_MS 5000U

/* Board identity reported to the official service.  LumiDock hardware is an
 * ESP32-S3 + ES8311 board, which is exactly the upstream bread-compact-wifi
 * profile, so the official endpoint always answers with a usable websocket
 * URL/token pair.  The name keeps the upstream "-128x64" variant convention
 * (a distinct name under a shared type).  LumiDock never consumes the
 * "firmware" section of the response, so the advertised type is inert. */
#define XIAOZHI_REFERENCE_BOARD_NAME "bread-compact-wifi-128x64"
#define XIAOZHI_REFERENCE_BOARD_TYPE "bread-compact-wifi"
#define XIAOZHI_REFERENCE_USER_AGENT "bread-compact-wifi-128x64/1.0.1"

static const char *TAG = "xiaozhi_activate";

typedef struct {
    bool initialized;
    volatile bool network_ready;
    bool activated;
    bool request_failed;
    bool activation_code_issued;
    bool restart_required;
    bool body_overflow;
    size_t body_len;
    char client_id[XIAOZHI_CLIENT_ID_LEN];
    char device_id[18];
    char activation_code[32];
    char websocket_url[256];
    char websocket_token[512];
    char body[XIAOZHI_HTTP_BODY_MAX];
} xiaozhi_activation_state_t;

static xiaozhi_activation_state_t s_state;
static TaskHandle_t s_task;

static void activation_task(void *arg);

static void publish_snapshot(void)
{
    const voice_activation_snapshot_t snapshot = {
        .initialized = s_state.initialized,
        .activated = s_state.activated,
        .request_failed = s_state.request_failed,
        .restart_required = s_state.restart_required,
        .activation_code = {0},
    };
    voice_activation_snapshot_t mutable_snapshot = snapshot;
    (void)strlcpy(mutable_snapshot.activation_code, s_state.activation_code,
                  sizeof(mutable_snapshot.activation_code));
    mid_snapshot_set_voice_activation(&mutable_snapshot);
}

static esp_err_t load_or_create_client_id(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(XIAOZHI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    size_t len = sizeof(s_state.client_id);
    err = nvs_get_str(nvs, XIAOZHI_NVS_CLIENT_ID, s_state.client_id, &len);
    if (err == ESP_OK && len == XIAOZHI_CLIENT_ID_LEN) {
        nvs_close(nvs);
        return ESP_OK;
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs);
        return err;
    }

    uint32_t p0 = esp_random();
    uint32_t p1 = (esp_random() & UINT32_C(0xffff0fff)) | UINT32_C(0x00004000);
    uint32_t p2 = (esp_random() & UINT32_C(0x3fffffff)) | UINT32_C(0x80000000);
    uint32_t p3 = esp_random();
    (void)snprintf(s_state.client_id, sizeof(s_state.client_id),
                   "%08" PRIx32 "-%04" PRIx32 "-%04" PRIx32 "-%04" PRIx32 "-%04" PRIx32 "%08" PRIx32,
                   p0, p1 >> 16U, p1 & UINT32_C(0xffff), p2 >> 16U,
                   p2 & UINT32_C(0xffff), p3);

    err = nvs_set_str(nvs, XIAOZHI_NVS_CLIENT_ID, s_state.client_id);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t response_event(esp_http_client_event_t *event)
{
    /* 本回调跑在 PSRAM 栈的激活任务上。记账只动自旋锁和内存、不碰 NVS，
       所以这里不存在"PSRAM 栈写 Flash"那个坑。 */
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        device_data_traffic_add_rx((size_t)event->data_len);
    }
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == NULL) return ESP_OK;
    if (s_state.body_len + event->data_len >= sizeof(s_state.body)) {
        s_state.body_overflow = true;
        return ESP_OK;
    }
    memcpy(s_state.body + s_state.body_len, event->data, event->data_len);
    s_state.body_len += event->data_len;
    s_state.body[s_state.body_len] = '\0';
    return ESP_OK;
}

static bool copy_json_string(cJSON *object, const char *name, char *out, size_t out_size)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(value) || value->valuestring == NULL) return false;
    return strlcpy(out, value->valuestring, out_size) < out_size;
}

static bool parse_activation_response(void)
{
    cJSON *root = cJSON_Parse(s_state.body);
    cJSON *websocket = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, "websocket") : NULL;
    if (!cJSON_IsObject(websocket) ||
        !copy_json_string(websocket, "url", s_state.websocket_url, sizeof(s_state.websocket_url)) ||
        !copy_json_string(websocket, "token", s_state.websocket_token, sizeof(s_state.websocket_token))) {
        cJSON_Delete(root);
        return false;
    }

    cJSON *activation = cJSON_GetObjectItemCaseSensitive(root, "activation");
    s_state.activation_code[0] = '\0';
    s_state.activated = !cJSON_IsObject(activation);
    if (s_state.activated) {
        /* A restart is needed only for the first binding observed in this boot.
         * Credential refreshes after that must not reboot the device again. */
        s_state.restart_required = s_state.activation_code_issued;
        s_state.activation_code_issued = false;
    } else if (!copy_json_string(activation, "code", s_state.activation_code,
                                 sizeof(s_state.activation_code))) {
        cJSON_Delete(root);
        return false;
    } else {
        s_state.activation_code_issued = true;
        s_state.restart_required = false;
    }
    cJSON_Delete(root);
    return true;
}

static bool request_activation(void)
{
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) return false;
    (void)snprintf(s_state.device_id, sizeof(s_state.device_id), "%02x:%02x:%02x:%02x:%02x:%02x",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return false;
    cJSON *application = cJSON_AddObjectToObject(root, "application");
    cJSON *board = cJSON_AddObjectToObject(root, "board");
    if (root == NULL || application == NULL || board == NULL) {
        cJSON_Delete(root);
        return false;
    }
    cJSON_AddStringToObject(application, "version", "1.0.0");
    cJSON_AddStringToObject(application, "elf_sha256", esp_app_get_elf_sha256_str());
    cJSON_AddStringToObject(board, "name", XIAOZHI_REFERENCE_BOARD_NAME);
    cJSON_AddStringToObject(board, "type", XIAOZHI_REFERENCE_BOARD_TYPE);
    cJSON_AddStringToObject(board, "ssid", wifi_manager_get_sta_ssid());
    cJSON_AddNumberToObject(board, "rssi", wifi_manager_get_sta_rssi());
    cJSON_AddStringToObject(board, "mac", s_state.device_id);
    cJSON_AddStringToObject(board, "ip", wifi_manager_get_sta_ip_str());
    char *request_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (request_body == NULL) return false;

    s_state.body_len = 0;
    s_state.body_overflow = false;
    s_state.body[0] = '\0';
    const esp_http_client_config_t config = {
        .url = XIAOZHI_OTA_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = response_event,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        cJSON_free(request_body);
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", XIAOZHI_REFERENCE_USER_AGENT);
    esp_http_client_set_header(client, "Device-Id", s_state.device_id);
    esp_http_client_set_header(client, "Client-Id", s_state.client_id);
    esp_http_client_set_post_field(client, request_body, strlen(request_body));
    http_memory_log("before", "xiaozhi_ota");
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    cJSON_free(request_body);
    http_memory_log("after", "xiaozhi_ota");
    if (err != ESP_OK || status != 200 || s_state.body_overflow) {
        ESP_LOGW(TAG, "OTA registration request failed: err=%s status=%d", esp_err_to_name(err), status);
        return false;
    }
    return parse_activation_response();
}

esp_err_t xiaozhi_activation_service_init(void)
{
    if (s_state.initialized) return ESP_OK;
    memset(&s_state, 0, sizeof(s_state));
    ESP_LOGI(TAG, "backend: official xiaozhi.me, OTA endpoint %s", XIAOZHI_OTA_URL);
    const esp_err_t err = load_or_create_client_id();
    if (err != ESP_OK) return err;
    s_state.initialized = true;
    publish_snapshot();
    /* 激活任务只发 HTTPS 注册请求，并调用只读的 esp_wifi_get_mac，从不写 NVS，
     * 所以任务栈可以放在 PSRAM，把内部 RAM 留给 ESP-SR。 */
    return xTaskCreatePinnedToCoreWithCaps(activation_task, "xiaozhi_activation", 6144,
                                           NULL, 2, &s_task, 0,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

void xiaozhi_activation_service_notify_network(void)
{
    if (!s_state.initialized) return;
    s_state.network_ready = true;
    if (s_task != NULL) xTaskNotifyGive(s_task);
}

void xiaozhi_activation_service_network_lost(void)
{
    if (!s_state.initialized) return;
    s_state.network_ready = false;
    s_state.activated = false;
    s_state.websocket_url[0] = '\0';
    s_state.websocket_token[0] = '\0';
    publish_snapshot();
    if (s_task != NULL) xTaskNotifyGive(s_task);
}

void xiaozhi_activation_service_refresh_credentials(void)
{
    if (!s_state.initialized) return;
    s_state.activated = false;
    s_state.websocket_url[0] = '\0';
    s_state.websocket_token[0] = '\0';
    publish_snapshot();
    if (s_task != NULL) xTaskNotifyGive(s_task);
}

static void activation_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_state.network_ready) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (!s_state.activated) {
            s_state.request_failed = !request_activation();
            if (s_state.request_failed) {
                ESP_LOGW(TAG, "OTA registration retry pending");
            } else if (s_state.activated) {
                ESP_LOGI(TAG, "device activation confirmed");
                device_data_mark_activated();
            } else {
                ESP_LOGI(TAG, "device activation code received");
            }
            publish_snapshot();
        }
        ulTaskNotifyTake(pdTRUE, s_state.activated ? portMAX_DELAY : pdMS_TO_TICKS(XIAOZHI_RETRY_MS));
    }
}

bool xiaozhi_activation_service_is_activated(void)
{
    return s_state.initialized && s_state.activated;
}

const char *xiaozhi_activation_service_get_websocket_url(void)
{
    return s_state.websocket_url;
}

const char *xiaozhi_activation_service_get_websocket_token(void)
{
    return s_state.websocket_token;
}

const char *xiaozhi_activation_service_get_client_id(void)
{
    return s_state.client_id;
}

const char *xiaozhi_activation_service_get_device_id(void)
{
    return s_state.device_id;
}
