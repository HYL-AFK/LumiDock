$ErrorActionPreference = 'Stop'
$root = "$PSScriptRoot\.."
$news = Get-Content -Raw -LiteralPath "$root\main\MID\network\news_service.c"
$wifi = Get-Content -Raw -LiteralPath "$root\main\MID\network\wifi_service.c"
$mcp = Get-Content -Raw -LiteralPath "$root\main\MID\system\mcp_service.c"

# 每个分类一个源，用栏目自己的权威划分，不从标题猜关键词——把体育播成经济
# 比"没有分类"更糟。换源前先确认设备侧能直接拿到 XML（36kr 会被人机校验拦掉）。
@(
    @{ Pattern = 'chinanews\.com\.cn/rss/scroll-news\.xml'; Message = 'The headline feed is missing.' },
    @{ Pattern = 'chinanews\.com\.cn/rss/finance\.xml'; Message = 'The economy feed is missing.' },
    @{ Pattern = 'chinanews\.com\.cn/rss/culture\.xml'; Message = 'The arts feed is missing.' },
    @{ Pattern = 'chinanews\.com\.cn/rss/ent\.xml'; Message = 'The entertainment feed is missing.' }
) | ForEach-Object { if ($news -notmatch $_.Pattern) { throw $_.Message } }

# 娱乐源实测会返回结构完整但没有 item 的 channel。那不是错误，更不能把上一次的
# 好数据清空——否则用户会在毫无征兆的时候听到"今天没有新闻"。
if ($news -notmatch '(?s)count == 0U[\s\S]{0,400}?return s_counts\[category\]') {
    throw 'An empty feed must keep the previous cache instead of wiping it.'
}
if ($news -notmatch '(?s)err != ESP_OK \|\| status != 200[\s\S]{0,300}?return s_counts\[category\]') {
    throw 'A failed fetch must keep the previous cache.'
}
# channel 级自己也有一个 <title>，不卡 </item> 边界的话第一条永远是频道名。
if ($news -notmatch 'title > item_end' -or $news -notmatch 'title_end > item_end') {
    throw 'Title extraction must stay inside the item element.'
}
# 中文标题截断必须退到 UTF-8 首字节，否则会切出半个汉字念不出来。
if ($news -notmatch '0xC0U' -or $news -notmatch '0x80U') {
    throw 'Title truncation must back off to a UTF-8 boundary.'
}
@(
    @{ Pattern = 'MALLOC_CAP_SPIRAM'; Message = 'The news buffers must live in PSRAM.' },
    @{ Pattern = 'xTaskCreatePinnedToCoreWithCaps'; Message = 'The news task stack must not come out of internal RAM.' },
    @{ Pattern = 'watchdog_service_set_current_task_blocking\(true\)'; Message = 'The HTTP fetch must mark itself blocking for the watchdog.' }
) | ForEach-Object { if ($news -notmatch $_.Pattern) { throw $_.Message } }

if ($wifi -notmatch 'news_service_notify_network\(\);') {
    throw 'The network-ready handler must wake the news service.'
}

# 工具登记与分发
if ($mcp -notmatch '#define MCP_TOOL_NEWS_FETCH "self\.news\.fetch"') {
    throw 'The news tool name is missing.'
}
if ($mcp -notmatch 'static const char \*handle_news_fetch\(const cJSON \*arguments, long id\)') {
    throw 'The news tool handler is missing.'
}
if ($mcp -notmatch 'strcmp\(tool, MCP_TOOL_NEWS_FETCH\)') {
    throw 'The news tool is not dispatched.'
}
# 还没拉到属于正常等待而不是故障，要给一句能直接播的话，别让云端念报错。
if ($mcp -notmatch '新闻还在加载') {
    throw 'An unready news cache must answer with a speakable sentence, not an error.'
}
# 回包比充电建议长得多，必须走 PSRAM scratch，不能压在会话任务栈上。
if ($mcp -notmatch 'NEWS_PLAIN_BYTES' -or $mcp -notmatch 'ensure_news_scratch') {
    throw 'The news reply must be assembled in PSRAM scratch, not on the session stack.'
}

Write-Output 'News service contract passed.'
