#include "device_settings_service.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "i2s_audio_driver.h"

static const char *const TAG = "device_settings";

/* 独立命名空间：app_cfg 归 Wi-Fi/BLE 配置所有，这里只存设备偏好。 */
#define DEVICE_SETTINGS_NVS_NAMESPACE "device_cfg"
#define DEVICE_SETTINGS_KEY_VOLUME "volume"

#define DEVICE_SETTINGS_VOLUME_MIN 0
#define DEVICE_SETTINGS_VOLUME_MAX 100
/* 一串连续调整（本地快捷路径 + 云端回声）必须合并成一次 Flash 写入。 */
#define DEVICE_SETTINGS_VOLUME_DEBOUNCE_MS 1200U
#define DEVICE_SETTINGS_PERSIST_TASK_STACK 2560U

static TaskHandle_t s_persist_task;
static SemaphoreHandle_t s_lock;
/* 待写入的最新值；worker 醒来后只取一次，天然完成去抖合并。 */
static int s_pending_volume;
static bool s_pending_valid;
/* 已落盘的值，用来跳过重复写入。 */
static int s_persisted_volume;
static bool s_has_persisted_volume;
/* 最近一次生效的音量（含仍在去抖窗口内、还没落盘的那个）。
 * 音频重新初始化发生在去抖窗口内时，必须回填它而不是回填 s_persisted_volume，
 * 否则刚调好的音量会被旧值覆盖。 */
static int s_target_volume;
static bool s_target_valid;

static int clamp_percent(int percent)
{
    if (percent < DEVICE_SETTINGS_VOLUME_MIN) return DEVICE_SETTINGS_VOLUME_MIN;
    if (percent > DEVICE_SETTINGS_VOLUME_MAX) return DEVICE_SETTINGS_VOLUME_MAX;
    return percent;
}

static esp_err_t read_stored_volume(int *percent_out)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(DEVICE_SETTINGS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    int32_t stored = 0;
    err = nvs_get_i32(handle, DEVICE_SETTINGS_KEY_VOLUME, &stored);
    nvs_close(handle);
    if (err != ESP_OK) return err;
    *percent_out = clamp_percent((int)stored);
    return ESP_OK;
}

static esp_err_t write_volume_to_nvs(int percent)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(DEVICE_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_i32(handle, DEVICE_SETTINGS_KEY_VOLUME, percent);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

/* NVS 写入会关闭 SPI Cache，PSRAM 栈此时不可访问，所以真正的落盘必须发生
 * 在这个内部 RAM 栈的任务上。 */
static void persist_worker_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        uint32_t bits = 0U;
        if (xTaskNotifyWait(0U, ULONG_MAX, &bits, portMAX_DELAY) != pdTRUE) continue;
        /* 吸收抖动窗口内的后续请求，只保留最后一个值。 */
        while (xTaskNotifyWait(0U, ULONG_MAX, &bits,
                               pdMS_TO_TICKS(DEVICE_SETTINGS_VOLUME_DEBOUNCE_MS)) == pdTRUE)
        {
        }
        int percent = 0;
        bool valid = false;
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE)
        {
            percent = s_pending_volume;
            valid = s_pending_valid;
            s_pending_valid = false;
            xSemaphoreGive(s_lock);
        }
        if (!valid) continue;
        if (s_has_persisted_volume && percent == s_persisted_volume) continue;
        const esp_err_t err = write_volume_to_nvs(percent);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "speaker volume persist failed: %s", esp_err_to_name(err));
            continue;
        }
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE)
        {
            s_persisted_volume = percent;
            s_has_persisted_volume = true;
            xSemaphoreGive(s_lock);
        }
        ESP_LOGI(TAG, "speaker volume stored: %d%%", percent);
    }
}

/* 任务可能在开机时因内部 RAM 紧张而创建失败。每次请求落盘前重试一次，让音量
 * 持久化能自行恢复，而不是整个开机周期都静默失效。调用方不得持有 s_lock：
 * 本函数内部会取锁以避开并发重建。 */
static bool ensure_persist_worker(void)
{
    if (s_persist_task != NULL) return true;
    if (s_lock == NULL) return false;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return false;
    bool ready = (s_persist_task != NULL);
    if (!ready)
    {
        if (xTaskCreatePinnedToCoreWithCaps(persist_worker_task, "settings_persist",
                                            DEVICE_SETTINGS_PERSIST_TASK_STACK, NULL, 2,
                                            &s_persist_task, 1,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS)
        {
            s_persist_task = NULL;
        }
        else
        {
            ready = true;
            ESP_LOGI(TAG, "settings worker ready on an internal RAM stack");
        }
    }
    xSemaphoreGive(s_lock);
    return ready;
}

void device_settings_service_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL)
    {
        ESP_LOGE(TAG, "settings lock unavailable; speaker volume stays in memory only");
        return;
    }
    /* 读 NVS 同样会关闭 SPI Cache，必须在这个内部 RAM 栈的调用任务上完成。 */
    int stored = 0;
    if (read_stored_volume(&stored) == ESP_OK)
    {
        s_persisted_volume = stored;
        s_has_persisted_volume = true;
        s_target_volume = stored;
        s_target_valid = true;
        ESP_LOGI(TAG, "speaker volume restored: %d%%", stored);
    }
    else
    {
        ESP_LOGI(TAG, "no stored speaker volume; the codec keeps its power-on default");
    }
    if (!ensure_persist_worker())
    {
        ESP_LOGE(TAG, "settings worker create failed; speaker volume stays in memory only"
                      " until a later save request retries");
    }
}

/* 上次退出时是静音（0%）的话，这次开机给这个音量而不是 0。
 *
 * 静音是个“当场按下”的动作，跨一次断电不该继续生效：开机后一点声音都没有，
 * 用户只会以为设备坏了（或者忘了自己静音过），而不是想起“哦我静音了”。给一个
 * 能听见但不刺耳的下限，他听到第一声就知道设备活着；真想再静音，按一下就行。 */
#define DEVICE_SETTINGS_BOOT_VOLUME_FLOOR 0

bool device_settings_service_apply_saved_volume(void)
{
    if (!s_target_valid) return false;
    int volume =
        s_target_volume <= 0 ? DEVICE_SETTINGS_BOOT_VOLUME_FLOOR : s_target_volume;
    /* TEMP DEBUG：公共场合验证，开机静音。恢复时删掉下面这一行。 */
    volume = 0;
    if (i2s_audio_driver_set_output_volume((float)volume) != ESP_OK)
    {
        ESP_LOGW(TAG, "applying the stored speaker volume failed");
        return false;
    }
    /* 同步内存里的目标值：硬件已经是 volume 了，如果还留着 0，后面的状态上报和
       保存会把两者说成不一样的东西。 */
    if (volume != s_target_volume)
    {
        ESP_LOGI(TAG, "stored speaker volume %d%% raised to the boot floor %d%%",
                 s_target_volume, volume);
    }
    s_target_volume = volume;
    ESP_LOGI(TAG, "stored speaker volume applied: %d%%", s_target_volume);
    return true;
}

void device_settings_service_request_volume_save(int percent)
{
    if (s_lock == NULL) return;
    /* 早期版本在这里直接 return，音量会安静地只留在内存里，重启后看起来像是
     * “根本没保存”。没有内部 RAM 跑 worker 时必须留下痕迹。 */
    if (!ensure_persist_worker())
    {
        ESP_LOGW(TAG, "speaker volume %d%% cannot be persisted: no internal RAM for the"
                      " settings worker", clamp_percent(percent));
        return;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;
    s_pending_volume = clamp_percent(percent);
    s_pending_valid = true;
    s_target_volume = s_pending_volume;
    s_target_valid = true;
    xSemaphoreGive(s_lock);
    (void)xTaskNotify(s_persist_task, 1UL, eSetBits);
}
