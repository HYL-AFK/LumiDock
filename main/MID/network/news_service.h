#ifndef NEWS_SERVICE_H
#define NEWS_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 分类直接对应一个 RSS 源，不做关键词猜测：源自己的栏目划分比从标题里猜准。
 * 娱乐源实测会偶发返回空 channel，所以取不到时要有兜底，不能给用户报错。 */
typedef enum {
    NEWS_CATEGORY_NEWS = 0,      /* 综合/时政 */
    NEWS_CATEGORY_ECONOMY,       /* 财经 */
    NEWS_CATEGORY_ARTS,          /* 文化/艺术 */
    NEWS_CATEGORY_ENTERTAINMENT, /* 娱乐 */
    NEWS_CATEGORY_COUNT
} news_category_t;

#define NEWS_MAX_ITEMS 10U
#define NEWS_TITLE_MAX 128U

typedef struct {
    uint8_t count;
    char titles[NEWS_MAX_ITEMS][NEWS_TITLE_MAX];
} news_digest_t;

esp_err_t news_service_init(void);
/* 网络可用后调一次；断网重连会再次调用，服务自己判断要不要重新拉。 */
void news_service_notify_network(void);
/* 数据还没拉到就返回 false，此时别硬播报。 */
bool news_service_get(news_category_t category, news_digest_t *out);
/* 该分类当前缓存了几条，用来判断要不要给"暂时没有"的提示。 */
uint8_t news_service_count(news_category_t category);
const char *news_service_category_name(news_category_t category);

#endif
