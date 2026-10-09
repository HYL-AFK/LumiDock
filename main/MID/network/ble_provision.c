#include "ble_provision.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_log.h"
#include "blufi_security.h"
#include "esp_blufi.h"
#include "esp_blufi_api.h"
#include "esp_bt.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"
#include "runtime_event_log.h"
#include "startup_gate.h"
#include "wifi_manager.h"

static const char *TAG = "ble_provision";

void ble_store_config_init(void);

#define BLE_FIXED_ADV_NAME "ESPARK"
#define BLUFI_SCAN_LIST_MAX_AP 20
#define BLE_PROVISION_WINDOW_MS (5 * 60 * 1000)
#define BLE_BOOT_WINDOW_MS (60 * 1000)
/* The window-expiry stop calls NimBLE (ble_hs_lock) and MUST NOT run on the
 * FreeRTOS software-timer task (Tmr Svc), which both blocks and, on SMP,
 * corrupts the scheduler. The expiry is handed to a dedicated task with its
 * own stack so the (potentially blocking) NimBLE stop runs off the timer task. */
#define BLE_WINDOW_EXPIRE_TASK_STACK 5120U
#define BLE_WINDOW_EXPIRE_TASK_PRIO  (tskIDLE_PRIORITY + 3)

static app_config_t s_cfg;
static bool s_ready;
static bool s_advertising;
static bool s_connected;
static bool s_window_active;
static bool s_window_requested;
static bool s_stack_started;
static uint32_t s_window_ms;
static bool s_deep_sleep_on_expire = true;
static ble_window_kind_t s_window_kind;
static int64_t s_window_deadline_us;
static TimerHandle_t s_window_timer;
static SemaphoreHandle_t s_window_expire_sem;
static TaskHandle_t s_window_expire_task;
static bool s_notify_enabled;
static bool s_sta_connecting;
static bool s_scan_in_progress;
static bool s_wifi_list_requested;
static bool s_pending_sta_bssid_set;
static uint16_t s_conn_handle = 0xFFFF;
static uint8_t s_pending_sta_bssid[6];
static uint8_t s_last_sta_bssid[6];
static bool s_last_sta_bssid_set;
static uint8_t s_last_sta_reason = 0xff;
static int8_t s_last_sta_rssi = -128;
static uint8_t s_own_addr_type;
static char s_adv_name[BLE_NAME_MAX_LEN + 1];
static char s_pending_sta_ssid[WIFI_SSID_MAX_LEN + 1];
static char s_pending_sta_pass[WIFI_PASS_MAX_LEN + 1];

static void blufi_start_advertising(void);
static void blufi_start_wifi_scan(void);
static void blufi_apply_pending_sta_config(void);

/* 这里曾经有一个「窗口到期后把 BLE 栈拆掉、把内部 RAM 还给 WiFi」的实现
 * （esp_blufi_profile_deinit + nimble_port_stop + nimble_port_deinit，并在
 * ble_provision_open_window 里重新 blufi_stack_init）。已经删除，不要加回来。
 *
 * 原因：ESP32-S3 上没有定义 SOC_ESP_NIMBLE_CONTROLLER，所以 esp_nimble_init()
 * 是一次不折不扣的全量 host 初始化——里面 npl_freertos_mempool_init() 会执行
 * os_mempool_init(&ble_freertos_ev_pool, 10, ...)，而 ev 池总共只有 10 个块。
 *
 * 于是只要在 host 任务还活着的时候再跑一次 esp_nimble_init()，就是拿
 * os_mempool_init 去重排一个正在被 os_memblock_get 使用的链表：
 *   os_mempool_init     : mp_num_free = blocks        (os_mempool.c:150)
 *     ← 这里是抢占窗口 →
 *   os_memblock_get     : 读到 mp_num_free > 0，但 SLIST_FIRST(mp) 还是被抽干后的
 *                         NULL → SLIST_NEXT(NULL) → LoadProhibited
 * 实机表现：开机约 1.7 秒必 panic 并无限重启，栈是
 *   ble_hs_event_start_stage2 → ble_hs_start → npl_freertos_callout_init
 *   → npl_freertos_event_init → os_memblock_get (os_mempool.c:359)，EXCVADDR=0。
 *
 * 触发路径就在 ble_provision_init() 内部：它先把窗口定时器建好、之后才调用
 * blufi_stack_init()、最后才置位 s_stack_started；这段窗口里 network_coordinator_task
 * 上的并发调用者（NETWORK_EVENT_OPEN_BLUFI / wifi_status_changed）一进
 * ble_provision_open_window()，就会看到 s_stack_started 仍为假而第二次拉栈。
 *
 * 结论：BLE 栈全进程只允许被 ble_provision_init() 拉起一次；栈不在了就只能重启后
 * 再配网，不能在本进程内重建。 */

static void ble_window_expire(void)
{
    if (!s_window_active) return;
    s_window_active = false;
    s_window_requested = false;
    s_window_deadline_us = 0;
    if (s_window_timer != NULL) (void)xTimerStop(s_window_timer, 0);
    if (s_advertising) {
        (void)esp_blufi_adv_stop();
        s_advertising = false;
    }
    APP_LOGW(TAG, "BLE provisioning window expired; deep_sleep_on_expire=%d",
             (int)s_deep_sleep_on_expire);
    (void)runtime_event_log_append(RUNTIME_EVENT_BLE_TIMEOUT, 0, 0, 0);
    if (s_deep_sleep_on_expire) {
        startup_gate_enter_deep_sleep();
    }
}

static void ble_window_expire_task(void *param)
{
    (void)param;
    for (;;) {
        if (xSemaphoreTake(s_window_expire_sem, portMAX_DELAY) == pdTRUE) {
            ble_window_expire();
        }
    }
}

static void ble_window_timer_callback(TimerHandle_t timer)
{
    (void)timer;
    /* FreeRTOS software-timer callbacks run on the low-priority "Tmr Svc" task
       and MUST NOT block. Calling esp_blufi_adv_stop() -> NimBLE ble_hs_lock()
       from there both blocks and, on SMP, corrupts the scheduler (panic).
       Hand the expiry off to the dedicated task instead so the NimBLE stop
       (and any deep sleep) runs off the software-timer task. */
    if (s_window_active && s_window_expire_sem != NULL) {
        (void)xSemaphoreGive(s_window_expire_sem);
    }
}

static const char *blufi_error_to_str(esp_blufi_error_state_t state)
{
    switch (state) {
    case ESP_BLUFI_SEQUENCE_ERROR:
        return "SEQUENCE";
    case ESP_BLUFI_CHECKSUM_ERROR:
        return "CHECKSUM";
    case ESP_BLUFI_DECRYPT_ERROR:
        return "DECRYPT";
    case ESP_BLUFI_ENCRYPT_ERROR:
        return "ENCRYPT";
    case ESP_BLUFI_INIT_SECURITY_ERROR:
        return "INIT_SECURITY";
    case ESP_BLUFI_DH_MALLOC_ERROR:
        return "DH_MALLOC";
    case ESP_BLUFI_DH_PARAM_ERROR:
        return "DH_PARAM";
    case ESP_BLUFI_READ_PARAM_ERROR:
        return "READ_PARAM";
    case ESP_BLUFI_MAKE_PUBLIC_ERROR:
        return "MAKE_PUBLIC";
    case ESP_BLUFI_DATA_FORMAT_ERROR:
        return "DATA_FORMAT";
    case ESP_BLUFI_CALC_MD5_ERROR:
        return "CALC_MD5";
    case ESP_BLUFI_WIFI_SCAN_FAIL:
        return "WIFI_SCAN";
    case ESP_BLUFI_MSG_STATE_ERROR:
        return "MSG_STATE";
    default:
        return "UNKNOWN";
    }
}

static void blufi_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void make_adv_name(void)
{
    uint8_t mac[6];

    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_BT));
    snprintf(s_adv_name, sizeof(s_adv_name), "%s-%02X%02X%02X",
             BLE_FIXED_ADV_NAME, mac[3], mac[4], mac[5]);
}

static int blufi_gap_event(struct ble_gap_event *event, void *arg)
{
    int rc = esp_blufi_handle_gap_events(event, arg);

    if (event->type == BLE_GAP_EVENT_CONNECT && event->connect.status != 0) {
        if (s_window_active && !s_connected) blufi_start_advertising();
    } else if (event->type == BLE_GAP_EVENT_ADV_COMPLETE &&
               s_window_active && !s_connected) {
        ble_window_expire();
    }

    return rc;
}

static void blufi_start_advertising(void)
{
    if (!s_window_active || s_connected) return;

    struct ble_hs_adv_fields adv_fields;
    struct ble_hs_adv_fields scan_rsp;
    struct ble_gap_adv_params adv_params;
    int rc = ble_hs_util_ensure_addr(0);

    if (rc != 0) {
        APP_LOGE(TAG, "BLE address setup failed: rc=%d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        APP_LOGE(TAG, "BLE address type inference failed: rc=%d", rc);
        return;
    }

    memset(&scan_rsp, 0, sizeof(scan_rsp));
    scan_rsp.name = (uint8_t *)s_adv_name;
    scan_rsp.name_len = strlen(s_adv_name);
    scan_rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&scan_rsp);
    if (rc != 0) {
        APP_LOGE(TAG, "BLE scan response setup failed: rc=%d", rc);
        return;
    }

    memset(&adv_fields, 0, sizeof(adv_fields));
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv_fields.tx_pwr_lvl_is_present = 1;
    adv_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    adv_fields.uuids16 = (ble_uuid16_t[]) { BLE_UUID16_INIT(BLUFI_APP_UUID) };
    adv_fields.num_uuids16 = 1;
    adv_fields.uuids16_is_complete = 1;
    rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        APP_LOGE(TAG, "BLE advertisement setup failed: rc=%d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    /* 清零后的 itvl_min/itvl_max 是 0，NimBLE 会按 conn_mode 填默认档：UND 落在
       ble_gap_adv_dflt_itvls() 的 FAST_INTERVAL1，也就是 30~60 ms 一次，约每 45 ms
       就要占一回射频，而开机换网窗口正好 60 s，恰好盖住了开机后语音会话最集中的
       那一段。蓝牙每广播一次，WiFi 都要在共存调度里排队让路；实测三次“WSS 上行
       写不出去”全部落在这 60 s 内，窗口结束后同样时长、同样码率的语音会话一次
       都没复现过。改成 1000~2000 ms 后射频占用从约 2% 降到 0.07%，手机扫描最多
       多等两秒就能看到设备，代价远低于一次唤醒对话被打断。 */
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(1000);
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(2000);
    int64_t remaining_ms = (s_window_deadline_us - esp_timer_get_time()) / 1000;
    if (remaining_ms <= 0) {
        ble_window_expire();
        return;
    }
    if (remaining_ms > INT32_MAX) remaining_ms = INT32_MAX;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, (int32_t)remaining_ms,
                           &adv_params, blufi_gap_event, NULL);
    if (rc != 0) {
        APP_LOGE(TAG, "BLE advertisement start failed: rc=%d", rc);
        return;
    }

    s_advertising = true;
    APP_LOGI(TAG, "BluFi advertising, name=%s", s_adv_name);
}

static int softap_get_current_connection_number(void)
{
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        return sta_list.num;
    }
    return 0;
}

static void build_blufi_conn_info(esp_blufi_extra_info_t *info)
{
    memset(info, 0, sizeof(*info));

    const char *sta_ssid = wifi_manager_get_sta_ssid();
    if (sta_ssid[0] != '\0') {
        info->sta_ssid = (uint8_t *)sta_ssid;
        info->sta_ssid_len = (int)strlen(sta_ssid);
    } else if (s_pending_sta_ssid[0] != '\0') {
        info->sta_ssid = (uint8_t *)s_pending_sta_ssid;
        info->sta_ssid_len = (int)strlen(s_pending_sta_ssid);
    }

    uint8_t bssid[6];
    if (wifi_manager_get_sta_bssid(bssid)) {
        memcpy(info->sta_bssid, bssid, sizeof(info->sta_bssid));
        info->sta_bssid_set = true;
    } else if (s_last_sta_bssid_set) {
        memcpy(info->sta_bssid, s_last_sta_bssid, sizeof(info->sta_bssid));
        info->sta_bssid_set = true;
    }

    if (wifi_manager_is_sta_connected()) {
        info->sta_conn_rssi_set = true;
        info->sta_conn_rssi = (int8_t)wifi_manager_get_sta_rssi();
    } else if (s_last_sta_rssi != -128) {
        info->sta_conn_rssi_set = true;
        info->sta_conn_rssi = s_last_sta_rssi;
    }

    if (!wifi_manager_is_sta_connected() && !s_sta_connecting && s_last_sta_reason != 0xff) {
        info->sta_conn_end_reason_set = true;
        info->sta_conn_end_reason = s_last_sta_reason;
    }
}

static void blufi_send_wifi_report(void)
{
    if (!s_connected) {
        return;
    }

    wifi_mode_t mode = WIFI_MODE_APSTA;
    esp_wifi_get_mode(&mode);

    esp_blufi_extra_info_t info;
    build_blufi_conn_info(&info);

    esp_blufi_sta_conn_state_t state;
    if (wifi_manager_is_sta_connected()) {
        state = ESP_BLUFI_STA_CONN_SUCCESS;
        s_sta_connecting = false;
    } else if (s_sta_connecting) {
        state = ESP_BLUFI_STA_CONNECTING;
    } else {
        state = ESP_BLUFI_STA_CONN_FAIL;
    }

    esp_err_t err = esp_blufi_send_wifi_conn_report(mode,
                                                    state,
                                                    (uint8_t)softap_get_current_connection_number(),
                                                    &info);
    if (err != ESP_OK) {
        APP_LOGW(TAG, "send wifi report failed: %s", esp_err_to_name(err));
    }
}

static void blufi_send_wifi_list(void)
{
    if (!s_connected) {
        APP_LOGI(TAG, "skip wifi list send, BluFi not connected");
        return;
    }

    uint16_t ap_count = 0;
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK || ap_count == 0) {
        APP_LOGW(TAG, "no AP found for BluFi scan");
        return;
    }

    if (ap_count > BLUFI_SCAN_LIST_MAX_AP) {
        ap_count = BLUFI_SCAN_LIST_MAX_AP;
    }

    wifi_ap_record_t *ap_list = calloc(ap_count, sizeof(*ap_list));
    esp_blufi_ap_record_t *blufi_list = calloc(ap_count, sizeof(*blufi_list));
    if (ap_list == NULL || blufi_list == NULL) {
        free(ap_list);
        free(blufi_list);
        APP_LOGE(TAG, "alloc wifi list failed");
        return;
    }

    if (esp_wifi_scan_get_ap_records(&ap_count, ap_list) != ESP_OK) {
        free(ap_list);
        free(blufi_list);
        APP_LOGW(TAG, "get AP records failed");
        return;
    }

    for (uint16_t i = 0; i < ap_count; ++i) {
        blufi_list[i].rssi = ap_list[i].rssi;
        memcpy(blufi_list[i].ssid, ap_list[i].ssid, sizeof(ap_list[i].ssid));
        blufi_list[i].ssid[sizeof(blufi_list[i].ssid) - 1] = '\0';
    }

    esp_err_t err = esp_blufi_send_wifi_list(ap_count, blufi_list);
    if (err != ESP_OK) {
        APP_LOGW(TAG, "send wifi list failed: %s", esp_err_to_name(err));
    }

    free(ap_list);
    free(blufi_list);
}

void ble_provision_handle_network_event(network_event_type_t event)
{
    if (event == NETWORK_EVENT_STA_CONNECTED) {
        APP_LOGI(TAG, "BluFi STA connected");
    } else if (event == NETWORK_EVENT_STA_DISCONNECTED) {
        blufi_send_wifi_report();
    } else if (event == NETWORK_EVENT_SCAN_DONE) {
        s_scan_in_progress = false;
        if (s_wifi_list_requested) {
            s_wifi_list_requested = false;
            blufi_send_wifi_list();
        }
    } else if (event == NETWORK_EVENT_AP_START || event == NETWORK_EVENT_AP_STOP ||
               event == NETWORK_EVENT_STA_GOT_IP) {
        blufi_send_wifi_report();
    } else if (event == NETWORK_EVENT_OPEN_BLUFI) {
        (void)ble_provision_open_window();
    } else if (event == NETWORK_EVENT_CLOSE_BLUFI) {
        ble_provision_close_window();
    } else if (event == NETWORK_EVENT_BLUFI_DISCONNECT) {
        wifi_manager_disconnect_sta();
        s_sta_connecting = false;
        blufi_send_wifi_report();
    } else if (event == NETWORK_EVENT_BLUFI_STATUS) {
        blufi_send_wifi_report();
    } else if (event == NETWORK_EVENT_BLUFI_SCAN) {
        blufi_start_wifi_scan();
    } else if (event == NETWORK_EVENT_BLUFI_CONNECT) {
        blufi_apply_pending_sta_config();
    }
}

static void blufi_start_wifi_scan(void)
{
    if (s_scan_in_progress) {
        APP_LOGI(TAG, "BluFi Wi-Fi scan already in progress");
        return;
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        APP_LOGW(TAG, "BluFi scan start failed: %s", esp_err_to_name(err));
        esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        return;
    }

    s_scan_in_progress = true;
    s_wifi_list_requested = true;
}

static void blufi_apply_pending_sta_config(void)
{
    if (s_pending_sta_ssid[0] == '\0') {
        APP_LOGW(TAG, "BluFi connect request without SSID");
        esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
        return;
    }

    if (!app_config_add_or_update_wifi(&s_cfg, s_pending_sta_ssid, s_pending_sta_pass)) {
        APP_LOGW(TAG, "save STA profile failed");
        esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
        return;
    }

    esp_err_t save_err = app_config_save(&s_cfg);
    if (save_err != ESP_OK) {
        APP_LOG_ERR(TAG, save_err, "save STA profile to NVS failed");
        esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
        return;
    }
    s_sta_connecting = true;
    s_last_sta_reason = 0xff;
    s_last_sta_rssi = -128;

    wifi_manager_enable_sta_reconnect(true);
    wifi_manager_connect_specific_sta(&s_cfg,
                                      s_pending_sta_ssid,
                                      s_pending_sta_pass,
                                      s_pending_sta_bssid,
                                      s_pending_sta_bssid_set);
    (void)runtime_event_log_append(RUNTIME_EVENT_BLE_PROVISION_ACCEPTED, 0, 0, 0);
}

static void blufi_event_callback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param)
{
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        APP_LOGI(TAG, "BluFi init finish");
        s_ready = true;
        if (s_window_requested) (void)ble_provision_open_window();
        break;

    case ESP_BLUFI_EVENT_DEINIT_FINISH:
        APP_LOGI(TAG, "BluFi deinit finish");
        s_ready = false;
        s_advertising = false;
        break;

    case ESP_BLUFI_EVENT_BLE_CONNECT:
        s_connected = true;
        s_notify_enabled = true;
        s_advertising = false;
        s_conn_handle = param->connect.conn_id;
        APP_LOGI(TAG, "BluFi BLE connected, conn=%u", s_conn_handle);
        if (blufi_security_init() != ESP_OK) {
            APP_LOGE(TAG, "BluFi security init failed");
        }
        esp_blufi_adv_stop();
        break;

    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        APP_LOGI(TAG, "BluFi BLE disconnected");
        s_connected = false;
        s_notify_enabled = false;
        s_conn_handle = 0xFFFF;
        s_wifi_list_requested = false;
        blufi_security_deinit();
        if (s_window_active) blufi_start_advertising();
        break;

    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        APP_LOGI(TAG, "BluFi requested wifi mode=%d, keep APSTA", param->wifi_mode.op_mode);
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        break;

    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
        APP_LOGI(TAG, "BluFi request connect to AP");
        (void)wifi_manager_post_network_event(&(network_coordinator_event_t){.type = NETWORK_EVENT_BLUFI_CONNECT});
        break;

    case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
        APP_LOGI(TAG, "BluFi request disconnect from AP");
        (void)wifi_manager_post_network_event(&(network_coordinator_event_t){.type = NETWORK_EVENT_BLUFI_DISCONNECT});
        break;

    case ESP_BLUFI_EVENT_GET_WIFI_STATUS:
        APP_LOGI(TAG, "BluFi get wifi status");
        (void)wifi_manager_post_network_event(&(network_coordinator_event_t){.type = NETWORK_EVENT_BLUFI_STATUS});
        break;

    case ESP_BLUFI_EVENT_RECV_STA_BSSID:
        memcpy(s_pending_sta_bssid, param->sta_bssid.bssid, sizeof(s_pending_sta_bssid));
        s_pending_sta_bssid_set = true;
        APP_LOGI(TAG, "BluFi recv STA BSSID");
        break;

    case ESP_BLUFI_EVENT_RECV_STA_SSID:
        if (param->sta_ssid.ssid_len <= 0 || param->sta_ssid.ssid_len > WIFI_SSID_MAX_LEN) {
            APP_LOGW(TAG, "invalid BluFi STA SSID len=%d", param->sta_ssid.ssid_len);
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memcpy(s_pending_sta_ssid, param->sta_ssid.ssid, (size_t)param->sta_ssid.ssid_len);
        s_pending_sta_ssid[param->sta_ssid.ssid_len] = '\0';
        APP_LOGI(TAG, "BluFi recv STA SSID=%s", s_pending_sta_ssid);
        break;

    case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        if (param->sta_passwd.passwd_len < 0 || param->sta_passwd.passwd_len > WIFI_PASS_MAX_LEN) {
            APP_LOGW(TAG, "invalid BluFi STA password len=%d", param->sta_passwd.passwd_len);
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memcpy(s_pending_sta_pass, param->sta_passwd.passwd, (size_t)param->sta_passwd.passwd_len);
        s_pending_sta_pass[param->sta_passwd.passwd_len] = '\0';
        APP_LOGI(TAG, "BluFi recv STA password len=%d", param->sta_passwd.passwd_len);
        break;

    case ESP_BLUFI_EVENT_RECV_SOFTAP_SSID: {
        if (param->softap_ssid.ssid_len <= 0 || param->softap_ssid.ssid_len > AP_NAME_MAX_LEN) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memcpy(s_cfg.ap_name, param->softap_ssid.ssid, (size_t)param->softap_ssid.ssid_len);
        s_cfg.ap_name[param->softap_ssid.ssid_len] = '\0';
        s_cfg.has_ap_name = true;
        esp_err_t save_err = app_config_save(&s_cfg);
        if (save_err != ESP_OK) {
            APP_LOG_ERR(TAG, save_err, "save SoftAP SSID to NVS failed");
            break;
        }
        wifi_manager_set_ap_name(s_cfg.ap_name);
        APP_LOGI(TAG, "BluFi recv SoftAP SSID=%s", s_cfg.ap_name);
        break;
    }

    case ESP_BLUFI_EVENT_RECV_SOFTAP_PASSWD: {
        if (param->softap_passwd.passwd_len < 0 || param->softap_passwd.passwd_len > AP_PASS_MAX_LEN) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memcpy(s_cfg.ap_pass, param->softap_passwd.passwd, (size_t)param->softap_passwd.passwd_len);
        s_cfg.ap_pass[param->softap_passwd.passwd_len] = '\0';
        s_cfg.has_ap_pass = true;
        esp_err_t save_err = app_config_save(&s_cfg);
        if (save_err != ESP_OK) {
            APP_LOG_ERR(TAG, save_err, "save SoftAP password to NVS failed");
            break;
        }
        if (strlen(s_cfg.ap_pass) >= 8) {
            wifi_manager_set_ap_password(s_cfg.ap_pass);
        }
        APP_LOGI(TAG, "BluFi recv SoftAP password len=%d", param->softap_passwd.passwd_len);
        break;
    }

    case ESP_BLUFI_EVENT_GET_WIFI_LIST:
        APP_LOGI(TAG, "BluFi request wifi list");
        (void)wifi_manager_post_network_event(&(network_coordinator_event_t){.type = NETWORK_EVENT_BLUFI_SCAN});
        break;

    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        APP_LOGI(TAG, "BluFi request disconnect BLE");
        esp_blufi_disconnect();
        break;

    case ESP_BLUFI_EVENT_REPORT_ERROR:
        APP_LOGW(TAG,
                 "BluFi report error=%d(%s)",
                 param->report_error.state,
                 blufi_error_to_str(param->report_error.state));
        esp_blufi_send_error_info(param->report_error.state);
        break;

    case ESP_BLUFI_EVENT_RECV_SOFTAP_MAX_CONN_NUM:
    case ESP_BLUFI_EVENT_RECV_SOFTAP_AUTH_MODE:
    case ESP_BLUFI_EVENT_RECV_SOFTAP_CHANNEL:
    case ESP_BLUFI_EVENT_RECV_USERNAME:
    case ESP_BLUFI_EVENT_RECV_CA_CERT:
    case ESP_BLUFI_EVENT_RECV_CLIENT_CERT:
    case ESP_BLUFI_EVENT_RECV_SERVER_CERT:
    case ESP_BLUFI_EVENT_RECV_CLIENT_PRIV_KEY:
    case ESP_BLUFI_EVENT_RECV_SERVER_PRIV_KEY:
        APP_LOGI(TAG, "BluFi event %d ignored in current firmware", event);
        break;

    case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:
        APP_LOGI(TAG, "BluFi custom data len=%" PRIu32, param->custom_data.data_len);
        break;

    default:
        break;
    }
}

static esp_blufi_callbacks_t s_blufi_callbacks = {
    .event_cb = blufi_event_callback,
    .negotiate_data_handler = blufi_dh_negotiate_data_handler,
    .encrypt_func = blufi_aes_encrypt,
    .decrypt_func = blufi_aes_decrypt,
    .checksum_func = blufi_crc_checksum,
};

static void blufi_on_reset(int reason)
{
    APP_LOGW(TAG, "NimBLE reset, reason=%d", reason);
}

static void blufi_on_sync(void)
{
    make_adv_name();
    ble_svc_gap_device_name_set(s_adv_name);
    esp_blufi_profile_init();
}

static esp_err_t blufi_controller_init(void)
{
    esp_bt_controller_status_t status = esp_bt_controller_get_status();
    if (status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        return ESP_OK;
    }

    if (status == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            APP_LOGW(TAG, "release classic bt memory failed: %s", esp_err_to_name(err));
        }

        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        err = esp_bt_controller_init(&bt_cfg);
        if (err != ESP_OK) {
            APP_LOGE(TAG, "bt controller init failed: %s", esp_err_to_name(err));
            return err;
        }
        status = esp_bt_controller_get_status();
    }

    if (status == ESP_BT_CONTROLLER_STATUS_INITED) {
        esp_err_t err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (err != ESP_OK) {
            APP_LOGE(TAG, "bt controller enable failed: %s", esp_err_to_name(err));
            return err;
        }
        return ESP_OK;
    }

    APP_LOGE(TAG, "unexpected bt controller status=%d", status);
    return ESP_FAIL;
}

static esp_err_t blufi_host_init(void)
{
    esp_err_t err = blufi_controller_init();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_nimble_init();
    if (err != ESP_OK) {
        APP_LOGE(TAG, "esp_nimble_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = blufi_on_reset;
    ble_hs_cfg.sync_cb = blufi_on_sync;
    ble_hs_cfg.gatts_register_cb = esp_blufi_gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = 4;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;

    int rc = esp_blufi_gatt_svr_init();
    if (rc != 0) {
        APP_LOGE(TAG, "esp_blufi_gatt_svr_init failed: %d", rc);
        return ESP_FAIL;
    }

    ble_store_config_init();
    esp_blufi_btc_init();
    return esp_nimble_enable(blufi_host_task);
}

static esp_err_t blufi_stack_init(void)
{
    esp_err_t err = esp_blufi_register_callbacks(&s_blufi_callbacks);
    if (err != ESP_OK) {
        return err;
    }

    return blufi_host_init();
}

void ble_provision_init(const app_config_t *cfg, bool open_window, ble_window_kind_t kind)
{
    if (cfg != NULL) {
        s_cfg = *cfg;
    } else {
        app_config_set_default(&s_cfg);
    }
    s_window_requested = open_window;
    s_window_kind = kind;
    switch (kind) {
    case BLE_WINDOW_BOOT:
        s_window_ms = BLE_BOOT_WINDOW_MS;
        s_deep_sleep_on_expire = false;
        break;
    default:
        s_window_ms = BLE_PROVISION_WINDOW_MS;
        s_deep_sleep_on_expire = true;
        break;
    }
    /* The one-shot timer period must always match the current mode. A timer
     * created for a short BOOT window (e.g. 60s) in a prior init would otherwise
     * keep its stale period and expire a first-time PROVISION window after
     * seconds instead of minutes. Re-create it whenever the period differs. */
    if (s_window_timer != NULL &&
        (uint32_t)xTimerGetPeriod(s_window_timer) != (uint32_t)pdMS_TO_TICKS(s_window_ms)) {
        (void)xTimerDelete(s_window_timer, 0);
        s_window_timer = NULL;
    }
    if (s_window_timer == NULL) {
        s_window_timer = xTimerCreate("ble_window", pdMS_TO_TICKS(s_window_ms),
                                      pdFALSE, NULL, ble_window_timer_callback);
        if (s_window_timer == NULL) {
            APP_LOGE(TAG, "BLE provisioning timer creation failed");
            return;
        }
    }
    if (s_window_expire_sem == NULL) {
        s_window_expire_sem = xSemaphoreCreateBinary();
        if (s_window_expire_sem == NULL) {
            APP_LOGE(TAG, "BLE window expire semaphore creation failed");
        }
    }
    if (s_window_expire_task == NULL && s_window_expire_sem != NULL) {
        /* This task is permanently resident (waits forever on the expiry
         * semaphore) and runs the NimBLE adv_stop / deep-sleep path at window
         * expiry. Allocate its stack from PSRAM so it never eats into the small
         * internal-RAM contiguous block that the WSS protocol task (needs 4096
         * bytes of contiguous internal RAM) depends on. */
        BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(ble_window_expire_task,
                                                        "ble_win_expire",
                                                        BLE_WINDOW_EXPIRE_TASK_STACK,
                                                        NULL,
                                                        BLE_WINDOW_EXPIRE_TASK_PRIO,
                                                        &s_window_expire_task,
                                                        tskNO_AFFINITY,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ok != pdPASS) {
            s_window_expire_task = NULL;
            APP_LOGE(TAG, "BLE window expire task creation failed");
        }
    }

    make_adv_name();
    s_conn_handle = 0xFFFF;
    s_last_sta_reason = 0xff;
    s_last_sta_rssi = -128;

    /* Wi-Fi manager owns the single system-event registration and forwards
     * copied events to the network coordinator. BluFi stays callback-free on
     * the sys_evt task to avoid duplicate queueing and Flash access there. */

    if (s_stack_started) return;
    esp_err_t err = blufi_stack_init();
    if (err != ESP_OK) {
        APP_LOGE(TAG, "BluFi init failed: %s", esp_err_to_name(err));
    } else {
        s_stack_started = true;
        APP_LOGI(TAG, "BluFi init started, adv_name=%s", s_adv_name);
    }
}

esp_err_t ble_provision_open_window(void)
{
    if (s_window_timer == NULL) return ESP_ERR_INVALID_STATE;
    s_window_requested = true;
    s_window_active = true;
    s_window_deadline_us = esp_timer_get_time() +
                           (int64_t)s_window_ms * 1000;
    (void)xTimerStop(s_window_timer, 0);
    if (xTimerStart(s_window_timer, 0) != pdPASS) return ESP_FAIL;
    (void)runtime_event_log_append(RUNTIME_EVENT_BLE_WINDOW_OPEN, 0, 0, 0);
    /* 本函数只负责窗口本身（定时器 + 广播），绝不允许拉起 BLE 栈——栈的启动唯一
       归属 ble_provision_init()，且全进程只能做一次。

       开机时本函数会被并发调用（network_coordinator_task 上的
       NETWORK_EVENT_OPEN_BLUFI / wifi_status_changed，与主任务上的
       ble_provision_init 并行），而那时 ble_provision_init 已经把窗口定时器建好、
       却还没跑到 blufi_stack_init()。若在这里补一次拉栈，就是第二次
       esp_nimble_init()：它会把 10 个块的 ble_freertos_ev_pool 就地重排，
       正在跑 nimble_port_run() 的 host 任务立刻 os_mempool.c:359 空指针 panic
       （开机 1.7 秒无限重启，详见上面 blufi_stack_release 处的说明）。

       这里直接返回是安全的：窗口状态和定时器上面已经设好，
       blufi_event_callback(ESP_BLUFI_EVENT_INIT_FINISH) 看到 s_window_requested
       会带着同一个窗口再进来一次，那时 s_ready 为真，自然走到下面的开广播。 */
    if (s_ready && !s_connected) {
        if (s_advertising) (void)esp_blufi_adv_stop();
        s_advertising = false;
        blufi_start_advertising();
    }
    return ESP_OK;
}

void ble_provision_close_window(void)
{
    /* A boot re-provision window must stay open for its full 60s duration so the
     * user can switch Wi-Fi even after the saved network happens to connect.
     * Only the first-time provisioning window closes immediately on a good link. */
    if (s_window_kind == BLE_WINDOW_BOOT) {
        return;
    }
    s_window_active = false;
    s_window_requested = false;
    s_window_deadline_us = 0;
    if (s_window_timer != NULL) (void)xTimerStop(s_window_timer, 0);
    if (s_advertising) {
        (void)esp_blufi_adv_stop();
        s_advertising = false;
    }
}

bool ble_provision_is_window_active(void)
{
    return s_window_active;
}

bool ble_provision_is_ready(void)
{
    return s_ready;
}

bool ble_provision_is_advertising(void)
{
    return s_advertising && !s_connected;
}

bool ble_provision_is_connected(void)
{
    return s_connected;
}

bool ble_provision_is_notify_enabled(void)
{
    return s_notify_enabled;
}

uint16_t ble_provision_get_conn_handle(void)
{
    return s_conn_handle;
}

const char *ble_provision_get_adv_name(void)
{
    return s_adv_name;
}
