#include "http_memory_log.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "multi_heap.h"

static const char *TAG = "http_memory";

/* 内部 RAM 的真实健康度，光看 free 会被骗。
 *
 * 实测电台全速播放时 internal=2471 largest=832——空闲还有 2.4 KB，但最大连续
 * 块只有 832。WSS 的 websocket task 要 4096 字节连续内部 RAM
 * （managed_components/espressif__esp_websocket_client 里是普通的
 * xTaskCreatePinnedToCore，栈必然落在内部），所以这种时刻建连一定失败，
 * 用户的语音唤醒直接没反应。free 高而 largest 低，说明是碎片，不是真的没内存。
 *
 * blocks 两项是判断碎片的直接证据：allocated_blocks 是当前被切出来的块数，
 * 它随"反复分配/释放同样大小的东西"单调增长——WSS 每断一次重连一次，rx/tx
 * 各 1024 字节走一遍，probe 每 30 秒再走一遍。min 是从开机以来的历史最低水位，
 * 用来区分"一直很紧"和"某个时刻掉下去过"。 */
void http_memory_log(const char *phase, const char *owner)
{
    multi_heap_info_t internal_info = {0};
    multi_heap_info_t psram_info = {0};
    heap_caps_get_info(&internal_info, MALLOC_CAP_INTERNAL);
    heap_caps_get_info(&psram_info, MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG,
             "%s %s internal=%u largest=%u blocks=%u/%u min=%u | psram=%u psram_largest=%u",
             phase != NULL ? phase : "request",
             owner != NULL ? owner : "unknown",
             (unsigned)internal_info.total_free_bytes,
             (unsigned)internal_info.largest_free_block,
             (unsigned)internal_info.allocated_blocks,
             (unsigned)internal_info.total_blocks,
             (unsigned)internal_info.minimum_free_bytes,
             (unsigned)psram_info.total_free_bytes,
             (unsigned)psram_info.largest_free_block);
}
