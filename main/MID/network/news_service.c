#include "news_service.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "http_memory_log.h"
#include "watchdog_service.h"

static const char *const TAG = "news";

/* 每个分类一个源。用中新网的栏目划分，不从标题里猜关键词——猜错的代价是
 * 把体育新闻播成经济新闻，而栏目本身就有权威划分。
 * 注意：36kr 的 feed 实测会被火山引擎的安全检测拦掉（返回人机校验页），
 * 换源时记得先确认设备侧能不能直接拿到 XML。 */
static const char *const k_feed_urls[NEWS_CATEGORY_COUNT] = {
    "https://www.chinanews.com.cn/rss/scroll-news.xml",
    "https://www.chinanews.com.cn/rss/finance.xml",
    "https://www.chinanews.com.cn/rss/culture.xml",
    "https://www.chinanews.com.cn/rss/ent.xml",
};

/* 实测单份 feed 15~17 KB，24 KB 留足余量。整块放 PSRAM。 */
#define NEWS_BODY_BYTES (24U * 1024U)
/* 即时新闻几分钟就一条，但拉一次要连四个源，太勤只是白耗电。 */
#define NEWS_REFRESH_INTERVAL_MS (30U * 60U * 1000U)
#define NEWS_HTTP_TIMEOUT_MS 10000

#define NEWS_ITEM_OPEN "<item>"
#define NEWS_ITEM_CLOSE "</item>"
#define NEWS_TITLE_OPEN "<title>"
#define NEWS_TITLE_CLOSE "</title>"
#define NEWS_CDATA_OPEN "<![CDATA["
#define NEWS_CDATA_CLOSE "]]>"

/* 标题缓存：4 个分类 × 10 条 × 128 字节 = 5120 字节，放 PSRAM。 */
#define NEWS_TITLE_BYTES (NEWS_CATEGORY_COUNT * NEWS_MAX_ITEMS * NEWS_TITLE_MAX)

static char *s_titles;
static uint8_t s_counts[NEWS_CATEGORY_COUNT];
static char *s_body;
static size_t s_body_len;
static SemaphoreHandle_t s_lock;  /* 保护 s_titles / s_counts */
static SemaphoreHandle_t s_wake;  /* 网络就绪通知 */
static TaskHandle_t s_task;

static bool is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* 把一段标题原文收拾成能直接播的字符串：脱 CDATA、去首尾空白、按 UTF-8 边界截断。 */
static bool copy_clean_title(const char *src, size_t len, char *out)
{
    if (len >= sizeof(NEWS_CDATA_OPEN) - 1U &&
        strncmp(src, NEWS_CDATA_OPEN, sizeof(NEWS_CDATA_OPEN) - 1U) == 0)
    {
        src += sizeof(NEWS_CDATA_OPEN) - 1U;
        len -= sizeof(NEWS_CDATA_OPEN) - 1U;
        if (len >= sizeof(NEWS_CDATA_CLOSE) - 1U &&
            strncmp(src + len - (sizeof(NEWS_CDATA_CLOSE) - 1U), NEWS_CDATA_CLOSE,
                    sizeof(NEWS_CDATA_CLOSE) - 1U) == 0)
        {
            len -= sizeof(NEWS_CDATA_CLOSE) - 1U;
        }
    }
    while (len > 0U && is_blank(src[0]))
    {
        ++src;
        --len;
    }
    while (len > 0U && is_blank(src[len - 1U]))
    {
        --len;
    }
    if (len == 0U)
    {
        return false;
    }
    if (len >= NEWS_TITLE_MAX)
    {
        len = NEWS_TITLE_MAX - 1U;
        /* 别把汉字劈成两半：UTF-8 续字节是 10xxxxxx，一路退到首字节为止。
           半个汉字会让云端 TTS 念出错音，也会让 JSON 里多出无效序列。 */
        while (len > 0U && ((unsigned char)src[len] & 0xC0U) == 0x80U)
        {
            --len;
        }
    }
    memcpy(out, src, len);
    out[len] = '\0';
    return true;
}

/* 从 feed 里抠标题。只认 <item> 里面的 <title>：channel 自己也有一个 <title>，
   不卡边界的话第一条永远是"中新网即时新闻"。 */
static uint8_t parse_items(const char *xml, char titles[NEWS_MAX_ITEMS][NEWS_TITLE_MAX])
{
    uint8_t count = 0U;
    const char *cursor = xml;
    while (count < NEWS_MAX_ITEMS)
    {
        const char *item = strstr(cursor, NEWS_ITEM_OPEN);
        if (item == NULL)
        {
            break;
        }
        const char *item_end = strstr(item, NEWS_ITEM_CLOSE);
        if (item_end == NULL)
        {
            break;
        }
        cursor = item_end + (sizeof(NEWS_ITEM_CLOSE) - 1U);
        const char *title = strstr(item, NEWS_TITLE_OPEN);
        if (title == NULL || title > item_end)
        {
            continue;
        }
        title += sizeof(NEWS_TITLE_OPEN) - 1U;
        const char *title_end = strstr(title, NEWS_TITLE_CLOSE);
        if (title_end == NULL || title_end > item_end)
        {
            continue;
        }
        if (copy_clean_title(title, (size_t)(title_end - title), titles[count]))
        {
            ++count;
        }
    }
    return count;
}

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data == NULL || evt->data_len <= 0)
    {
        return ESP_OK;
    }
    if (s_body == NULL)
    {
        return ESP_OK;
    }
    const size_t room = (NEWS_BODY_BYTES - 1U) - s_body_len;
    if (room == 0U)
    {
        return ESP_OK; /* 满了就丢，保住缓冲不越界 */
    }
    size_t take = (size_t)evt->data_len;
    if (take > room)
    {
        take = room;
    }
    memcpy(s_body + s_body_len, evt->data, take);
    s_body_len += take;
    s_body[s_body_len] = '\0';
    return ESP_OK;
}

/* 返回该分类当前的条数。失败时保留旧数据——拉不到新的比播报一半强。 */
static uint8_t fetch_feed(uint32_t category)
{
    if (s_body == NULL)
    {
        return 0U;
    }
    const char *const url = k_feed_urls[category];
    s_body_len = 0U;
    s_body[0] = '\0';

    const esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = NEWS_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_http_event,
        .user_agent = "LumiDock/1.0",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        ESP_LOGW(TAG, "%s HTTP client 创建失败", news_service_category_name((news_category_t)category));
        return s_counts[category];
    }

    http_memory_log("before", "news");
    watchdog_service_set_current_task_blocking(true);
    const esp_err_t err = esp_http_client_perform(client);
    watchdog_service_set_current_task_blocking(false);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    http_memory_log("after", "news");

    if (err != ESP_OK || status != 200)
    {
        ESP_LOGW(TAG, "%s 拉取失败 err=%s status=%d，沿用上次缓存",
                 news_service_category_name((news_category_t)category), esp_err_to_name(err), status);
        return s_counts[category];
    }

    char titles[NEWS_MAX_ITEMS][NEWS_TITLE_MAX];
    const uint8_t count = parse_items(s_body, titles);
    if (count == 0U)
    {
        /* 娱乐源实测会返回结构完整但没有 item 的 channel，这不是错误，
           更不该把上一次的好数据清掉。 */
        ESP_LOGW(TAG, "%s 本次没有条目（%u 字节），沿用上次缓存",
                 news_service_category_name((news_category_t)category), (unsigned)s_body_len);
        return s_counts[category];
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_titles + (size_t)category * NEWS_MAX_ITEMS * NEWS_TITLE_MAX, titles, sizeof(titles));
    s_counts[category] = count;
    xSemaphoreGive(s_lock);
    return count;
}

static void refresh_all(void)
{
    for (uint32_t i = 0U; i < NEWS_CATEGORY_COUNT; ++i)
    {
        const uint8_t count = fetch_feed(i);
        ESP_LOGI(TAG, "%s：%u 条", news_service_category_name((news_category_t)i), (unsigned)count);
    }
}

static void news_task(void *arg)
{
    (void)arg;
    /* 开机时网络还没好，先等通知，别白白空转。 */
    while (xSemaphoreTake(s_wake, portMAX_DELAY) != pdTRUE)
    {
    }
    for (;;)
    {
        refresh_all();
        /* 等下一轮。中途网络重连会给一次信号，那就立刻重拉。 */
        (void)xSemaphoreTake(s_wake, pdMS_TO_TICKS(NEWS_REFRESH_INTERVAL_MS));
    }
}

const char *news_service_category_name(news_category_t category)
{
    switch (category)
    {
    case NEWS_CATEGORY_NEWS:
        return "news";
    case NEWS_CATEGORY_ECONOMY:
        return "economy";
    case NEWS_CATEGORY_ARTS:
        return "arts";
    case NEWS_CATEGORY_ENTERTAINMENT:
        return "entertainment";
    default:
        return "unknown";
    }
}

esp_err_t news_service_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
    if (s_lock == NULL || s_wake == NULL)
    {
        ESP_LOGE(TAG, "新闻服务锁创建失败");
        return ESP_ERR_NO_MEM;
    }

    s_titles = heap_caps_malloc(NEWS_TITLE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_body = heap_caps_malloc(NEWS_BODY_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_titles == NULL || s_body == NULL)
    {
        ESP_LOGE(TAG, "新闻缓冲分配失败：titles=%p body=%p", (void *)s_titles, (void *)s_body);
        return ESP_ERR_NO_MEM;
    }
    memset(s_titles, 0, NEWS_TITLE_BYTES);

    /* 栈也放 PSRAM：解析时要在栈上摆一份 10×128 的标题区，内部 RAM 挤不下。 */
    if (xTaskCreatePinnedToCoreWithCaps(news_task, "news_task", 8192, NULL, 3, &s_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    {
        ESP_LOGE(TAG, "新闻任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "新闻服务就绪：%u 个源，标题缓存 %u 字节、正文缓冲 %u 字节（均在 PSRAM）",
             (unsigned)NEWS_CATEGORY_COUNT, (unsigned)NEWS_TITLE_BYTES, (unsigned)NEWS_BODY_BYTES);
    return ESP_OK;
}

void news_service_notify_network(void)
{
    if (s_wake != NULL)
    {
        xSemaphoreGive(s_wake);
    }
}

bool news_service_get(news_category_t category, news_digest_t *out)
{
    if (out == NULL || category >= NEWS_CATEGORY_COUNT || s_lock == NULL || s_titles == NULL)
    {
        return false;
    }
    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->count = s_counts[category];
    if (out->count > 0U)
    {
        memcpy(out->titles, s_titles + (size_t)category * NEWS_MAX_ITEMS * NEWS_TITLE_MAX,
               sizeof(out->titles));
    }
    xSemaphoreGive(s_lock);
    return out->count > 0U;
}

uint8_t news_service_count(news_category_t category)
{
    if (category >= NEWS_CATEGORY_COUNT || s_lock == NULL)
    {
        return 0U;
    }
    return s_counts[category];
}
