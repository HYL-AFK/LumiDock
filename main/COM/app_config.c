#include "app_config.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "nvs.h"
#include "app_log.h"

static const char *TAG = "app_config";

static esp_err_t erase_optional_key(nvs_handle_t handle, const char *key)
{
    esp_err_t err = nvs_erase_key(handle, key);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

/*
 * NVS 配置存储模块。
 *
 * 命名空间：app_cfg
 *
 * 当前使用的 key：
 * - ssid0/pass0 ... ssid4/pass4：最多 5 组 STA Wi-Fi 配置。
 * - wifi_count：已保存 Wi-Fi 数量。
 * - ble_name：用户显示名，不再直接作为 BLE 广播名。
 * - ap_name：设备 SoftAP 热点名称。
 * - ap_pass：设备 SoftAP 热点密码。
 *
 * 维护原则：
 * - 其他模块不要直接读写这些 key。
 * - 新增持久化字段时，优先在 app_config_t 增加字段，再在 load/save 中集中处理。
 */

#define APP_NVS_NS "app_cfg"

static esp_err_t erase_legacy_location_keys(nvs_handle_t handle, bool *erased)
{
    static const char *keys[] = {"loc_ver", "loc_lat", "loc_lon", "loc_tz"};
    if (erased != NULL) *erased = false;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        esp_err_t err = nvs_erase_key(handle, keys[i]);
        if (err == ESP_OK) {
            if (erased != NULL) *erased = true;
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            return err;
        }
    }
    return ESP_OK;
}

/* 生成一份内存默认配置，不访问 NVS。 */
void app_config_set_default(app_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    strlcpy(cfg->ble_name, "ESP32S3-BLE", sizeof(cfg->ble_name));
    strlcpy(cfg->ap_name, "ESP32S3-SETUP", sizeof(cfg->ap_name));
    strlcpy(cfg->ap_pass, "12345678", sizeof(cfg->ap_pass));
    cfg->has_ap_pass = true;
}

static void load_wifi_profiles(app_config_t *cfg, nvs_handle_t handle)
{
    /*
     * 从 NVS 按序加载多个 Wi-Fi 配置。
     * 保存顺序只代表写入顺序，真正连接时 wifi_manager 会结合扫描结果选择信号最强的可用热点。
     */
    char key[16];
    size_t count = 0;

    for (int i = 0; i < WIFI_PROFILE_MAX_NUM; ++i) {
        snprintf(key, sizeof(key), "ssid%d", i);
        size_t ssid_len = sizeof(cfg->wifi_profiles[i].wifi_ssid);
        if (nvs_get_str(handle, key, cfg->wifi_profiles[i].wifi_ssid, &ssid_len) != ESP_OK) {
            continue;
        }

        snprintf(key, sizeof(key), "pass%d", i);
        size_t pass_len = sizeof(cfg->wifi_profiles[i].wifi_pass);
        if (nvs_get_str(handle, key, cfg->wifi_profiles[i].wifi_pass, &pass_len) != ESP_OK) {
            continue;
        }

        cfg->wifi_profiles[i].valid = true;
        ++count;
    }

    cfg->wifi_count = (int)count;
    cfg->has_wifi = (count > 0);
}

/* 将当前配置完整写入 NVS。 */
esp_err_t app_config_save(const app_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    /*
     * Wi-Fi 列表采用全量覆盖保存。
     * 先清掉旧 key，再写入当前有效列表，避免删除某个 SSID 后 NVS 里残留旧数据。
     */
    for (int i = 0; i < WIFI_PROFILE_MAX_NUM; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "ssid%d", i);
        err = erase_optional_key(handle, key);
        if (err != ESP_OK) goto fail;
        snprintf(key, sizeof(key), "pass%d", i);
        err = erase_optional_key(handle, key);
        if (err != ESP_OK) goto fail;
    }

    for (int i = 0; i < cfg->wifi_count && i < WIFI_PROFILE_MAX_NUM; ++i) {
        if (!cfg->wifi_profiles[i].valid) {
            continue;
        }

        char key[16];
        snprintf(key, sizeof(key), "ssid%d", i);
        err = nvs_set_str(handle, key, cfg->wifi_profiles[i].wifi_ssid);
        if (err != ESP_OK) goto fail;
        snprintf(key, sizeof(key), "pass%d", i);
        err = nvs_set_str(handle, key, cfg->wifi_profiles[i].wifi_pass);
        if (err != ESP_OK) goto fail;
    }

    if (cfg->has_ble_name) {
        err = nvs_set_str(handle, "ble_name", cfg->ble_name);
        if (err != ESP_OK) goto fail;
    }

    if (cfg->has_ap_name) {
        err = nvs_set_str(handle, "ap_name", cfg->ap_name);
        if (err != ESP_OK) goto fail;
    }

    if (cfg->has_ap_pass) {
        err = nvs_set_str(handle, "ap_pass", cfg->ap_pass);
        if (err != ESP_OK) goto fail;
    }

    err = nvs_set_i32(handle, "wifi_count", cfg->wifi_count);
    if (err != ESP_OK) goto fail;
    err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) APP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
    return err;

fail:
    APP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
    nvs_close(handle);
    return err;
}

esp_err_t app_config_save_last_wifi_ssid(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > WIFI_SSID_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, "last_ssid", ssid);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        APP_LOGE(TAG, "save last Wi-Fi SSID failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* 从 NVS 加载配置；如果读取失败则保留默认值。 */
void app_config_load(app_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    app_config_set_default(cfg);

    nvs_handle_t handle;
    if (nvs_open(APP_NVS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }

    int32_t count = 0;
    if (nvs_get_i32(handle, "wifi_count", &count) == ESP_OK && count > 0) {
        cfg->wifi_count = (count > WIFI_PROFILE_MAX_NUM) ? WIFI_PROFILE_MAX_NUM : (int)count;
        load_wifi_profiles(cfg, handle);
    }

    size_t last_ssid_len = sizeof(cfg->last_wifi_ssid);
    if (nvs_get_str(handle, "last_ssid", cfg->last_wifi_ssid,
                    &last_ssid_len) == ESP_OK &&
        cfg->last_wifi_ssid[0] != '\0') {
        cfg->has_last_wifi_ssid = true;
    }

    size_t len = sizeof(cfg->ble_name);
    if (nvs_get_str(handle, "ble_name", cfg->ble_name, &len) == ESP_OK) {
        cfg->has_ble_name = true;
    }

    len = sizeof(cfg->ap_name);
    if (nvs_get_str(handle, "ap_name", cfg->ap_name, &len) == ESP_OK) {
        cfg->has_ap_name = true;
    }

    len = sizeof(cfg->ap_pass);
    if (nvs_get_str(handle, "ap_pass", cfg->ap_pass, &len) == ESP_OK) {
        cfg->has_ap_pass = true;
    }

    bool erased_legacy_location = false;
    esp_err_t cleanup_err = erase_legacy_location_keys(handle, &erased_legacy_location);
    if (cleanup_err == ESP_OK && erased_legacy_location) {
        cleanup_err = nvs_commit(handle);
        if (cleanup_err == ESP_OK) APP_LOGI(TAG, "legacy NVS location cache removed");
    }
    if (cleanup_err != ESP_OK) {
        APP_LOGE(TAG, "remove legacy NVS location cache failed: %s",
                 esp_err_to_name(cleanup_err));
    }
    nvs_close(handle);
}

/* 新增或更新一条 Wi-Fi 配置，最多保存 5 组。 */
bool app_config_add_or_update_wifi(app_config_t *cfg, const char *ssid, const char *pass)
{
    if (cfg == NULL || ssid == NULL || pass == NULL || ssid[0] == '\0') {
        return false;
    }

    /* 先查重：同名 SSID 已存在时只更新密码，不新增条目。 */
    for (int i = 0; i < cfg->wifi_count; ++i) {
        if (strcmp(cfg->wifi_profiles[i].wifi_ssid, ssid) == 0) {
            strlcpy(cfg->wifi_profiles[i].wifi_pass, pass, sizeof(cfg->wifi_profiles[i].wifi_pass));
            cfg->wifi_profiles[i].valid = true;
            cfg->has_wifi = true;
            return true;
        }
    }

    /* 列表未满时才允许追加新配置。 */
    if (cfg->wifi_count >= WIFI_PROFILE_MAX_NUM) {
        return false;
    }

    strlcpy(cfg->wifi_profiles[cfg->wifi_count].wifi_ssid, ssid, sizeof(cfg->wifi_profiles[cfg->wifi_count].wifi_ssid));
    strlcpy(cfg->wifi_profiles[cfg->wifi_count].wifi_pass, pass, sizeof(cfg->wifi_profiles[cfg->wifi_count].wifi_pass));
    cfg->wifi_profiles[cfg->wifi_count].valid = true;
    cfg->wifi_count++;
    cfg->has_wifi = true;
    return true;
}

/* 清空整个应用配置命名空间。 */
esp_err_t app_config_erase_all(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) APP_LOGE(TAG, "erase all NVS config failed: %s", esp_err_to_name(err));
    return err;
}

/* 仅清空 STA Wi-Fi 相关 key。 */
esp_err_t app_config_erase_wifi(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    for (int i = 0; i < WIFI_PROFILE_MAX_NUM; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "ssid%d", i);
        err = erase_optional_key(handle, key);
        if (err != ESP_OK) goto erase_wifi_fail;
        snprintf(key, sizeof(key), "pass%d", i);
        err = erase_optional_key(handle, key);
        if (err != ESP_OK) goto erase_wifi_fail;
    }

    err = erase_optional_key(handle, "wifi_count");
    if (err == ESP_OK) err = erase_optional_key(handle, "last_ssid");
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) APP_LOGE(TAG, "erase Wi-Fi NVS config failed: %s", esp_err_to_name(err));
    return err;

erase_wifi_fail:
    APP_LOGE(TAG, "erase Wi-Fi NVS key failed: %s", esp_err_to_name(err));
    nvs_close(handle);
    return err;
}
