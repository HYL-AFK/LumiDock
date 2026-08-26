#include "http_memory_log.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "http_memory";

void http_memory_log(const char *phase, const char *owner)
{
    ESP_LOGI(TAG, "%s %s internal=%u largest=%u psram=%u psram_largest=%u",
             phase != NULL ? phase : "request",
             owner != NULL ? owner : "unknown",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}
