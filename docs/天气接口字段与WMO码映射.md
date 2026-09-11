# LumiDock 天气接口字段与 WMO 码映射

本文整理设备当前实际使用的天气数据链路：接口地址、请求与解析的字段、WMO 天气码到各处的映射，以及失败时的降级行为。

对应代码：

| 文件 | 职责 |
| --- | --- |
| `main/APP/app_time.c` | 定位请求、时区应用（天气的上游依赖） |
| `main/APP/app_weather.c` | 天气请求与 JSON 解析 |
| `main/COM/location_snapshot.c` / `.h` | 定位结果快照 |
| `main/COM/weather_snapshot.c` / `.h` | 天气结果快照 |
| `main/MID/network/weather_service.c` | 天气码到中文文案的转换、写入 UI 快照 |
| `main/APP/ui_service.c` | 背景场景与图标选择 |

## 整体链路

```
[定位] ipwho.is (主) / ipapi.co (备)
  └─> latitude, longitude, timezone, UTC 偏移
        │
        ▼
[天气] api.open-meteo.com/v1/forecast
  └─> current.temperature_2m, current.weather_code
        │
        ▼
weather_snapshot_t  (COM/weather_snapshot.h)
        │
        ▼
weather_service_poll()  ── 按 WMO 码查中文文案
        │
        ▼
time_weather_snapshot_t  (MID/mid_snapshot.h)
        │
        ▼
UI：温度文本 / 天气中文 / 6 路背景场景 / 4 路图标
```

## 一、天气接口（Open-Meteo）

### 请求

```
https://api.open-meteo.com/v1/forecast
  ?latitude=%.6f
  &longitude=%.6f
  &current=temperature_2m,weather_code
  &timezone=auto
```

- 经纬度来自定位快照，**不写死**；定位无效时不伪造坐标，直接跳过请求
- HTTP 超时 5000 ms
- 刷新间隔 30 分钟（`WEATHER_REFRESH_INTERVAL_MS`），网络恢复事件也会触发一次

### 解析的字段

| JSON 路径 | C 类型 | 落到 | 必需 |
| --- | --- | --- | --- |
| `current.temperature_2m` | number → `float` | `weather_snapshot_t.temperature_c` | 是 |
| `current.weather_code` | number → `int` | `weather_snapshot_t.weather_code` | 是 |
| `current`（对象本身） | object | 必需字段的容器 | 是 |

**覆盖率说明**：Open-Meteo 的响应实际还包含 `latitude`、`longitude`、`generationtime_ms`、`utc_offset_seconds`、`timezone`、`timezone_abbreviation`、`elevation`、`current.time`、`current.interval`、`current_units` 等字段，**代码一个都不读取**。

**覆盖语义**：两个字段都解析成功才写入快照（`has_temperature && has_weather_code`）——只有完整响应才能覆盖上一份网络天气。

**响应体上限**：2048 字节，超出按失败处理。

## 二、WMO 4677 天气码的四张下游映射表

同一个 `weather_code` 在四个地方走四张不同的表，粒度各不相同。

### 1. 日志名（`app_weather.c: weather_condition_name`）

只进日志，分得最细。

| code | 名称 | | code | 名称 |
| --- | --- | --- | --- | --- |
| 0 | clear | | 71–77 | snow |
| 1–3 | cloudy | | 80–82 | showers |
| 45, 48 | fog | | 85–86 | snow_showers |
| 51–57 | drizzle | | 95 | thunder |
| 61–67 | rain | | ≥96 | hail_thunder |
| 其他 | unknown | | | |

### 2. UI 文本（`weather_service.c: weather_text`）

只有 7 个取值。契约测试 **明确禁止**这张表再细分（不得出现 `code == 61/65/75` 这类分支）。

| code | 文本 |
| --- | --- |
| 0 | 晴 |
| 1–3 | 云 |
| 45, 48 | 雾 |
| 51–67 或 80–82 | 雨 |
| 71–77 或 85–86 | 雪 |
| ≥95 | 雷 |
| 其他 | 未知 |

### 3. 背景场景（`ui_service.c: weather_scene_for_code`）

6 张全屏图，**按数值 code 选择，不按本地化文本**。

| code | 场景资源 |
| --- | --- |
| 45, 48 | `ui_weather_fog_scene` |
| 51–67, 80–82 | `ui_weather_rain_scene` |
| 71–77, 85–86 | `ui_weather_snow_scene` |
| ≥95 | `ui_weather_thunder_scene` |
| 1–3 | `ui_weather_cloudy_scene` |
| 其他（含 0） | `ui_weather_clear_scene` |

### 4. 天气图标（`ui_service.c: weather_icon_for_code`）

4 张。

| code | 图标资源 |
| --- | --- |
| 71–77, 85–86 | `ui_weather_icon_snow` |
| 51–67, 80–82, ≥95 | `ui_weather_icon_rain` |
| 1–3, 45, 48 | `ui_weather_icon_cloudy` |
| 其他 | `ui_weather_icon_sunny` |

### ⚠️ 已知不对称：雾天的图标

`45` / `48`（雾）在**场景表里单独对应"雾"**，但在**图标表里被归到"云"**。因此雾天会出现：

- 文字 = 雾
- 背景 = 雾
- 图标 = 云

原因：`ui_assets.c` 的嵌入清单里天气图标只有 sunny / cloudy / rain / snow 四个，**没有雾图标**，属于没有素材可用的妥协，不是逻辑错误。

## 三、上游依赖：定位接口

天气请求的经纬度完全依赖定位结果。

### 请求

| 顺序 | 地址 | 说明 |
| --- | --- | --- |
| 主 | `https://ipwho.is/` | 优先使用 |
| 备 | `https://ipapi.co/json/` | 主接口失败时切换 |

请求头携带 `User-Agent: ESPARK/1.0`，超时 5000 ms。

### 解析的字段

| JSON 路径 | 必需 | 说明 |
| --- | --- | --- |
| `latitude` | 是 | 校验范围 `[-90, 90]` |
| `longitude` | 是 | 校验范围 `[-180, 180]` |
| `timezone` | 是 | 字符串，或对象形式下的 `.id` |
| `success` | 否 | 显式为 `false` 时整体判定失败 |
| UTC 偏移 | 否 | 用于设置 `TZ`；取不到时保持当前时区并告警 |

定位失败时**不伪造坐标**，天气请求被跳过，保留上一次有效天气数据。

## 四、快照出口

### 内部快照 `weather_snapshot_t`

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `temperature_c` | `float` | 温度（摄氏度） |
| `weather_code` | `int` | WMO 天气码 |
| `valid` | `bool` | 天气数据是否有效 |
| `temperature_valid` | `bool` | 温度是否有效 |
| `temperature_source` | enum | `WEATHER_TEMP_SOURCE_NONE` / `WEATHER_TEMP_SOURCE_NETWORK` |
| `updated_at` | `time_t` | 写入时刻 |

### 对外快照 `time_weather_snapshot_t`

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `valid` | `bool` | **属于时钟/日期部分，天气不参与判定** |
| `local_time` | `struct tm` | 本地时间 |
| `timezone` | `char[32]` | 时区名 |
| `weather_text` | `char[24]` | 由天气码查表得到的中文文案 |
| `weather_code` | `int` | 透传的 WMO 码，供 UI 选场景与图标 |
| `temperature_c` | `float` | 温度 |

> `time_weather.valid` 只反映时钟是否可用。天气可能早于或晚于 SNTP 到达，不能让一个尚未同步的时钟因此看起来有效。

## 五、失败判定与降级

天气请求的每一类失败都会写入 `runtime_event_log`：

| 情况 | `runtime_event_log` 记录 |
| --- | --- |
| HTTP 层失败 | `RUNTIME_EVENT_WEATHER_FAILURE`, 状态码, 错误码, 0 |
| 响应体超过 2048 字节 | 同上，第三参数为 1 |
| HTTP 状态码非 200 | 同上，第三参数为 2 |
| 有 `current` 但必需字段不齐 | 同上，第三参数为 3 |
| 没有 `current` 对象 | 同上，第三参数为 4 |
| 定位无效 | 直接跳过请求，**不发起天气请求** |

成功时记录 `RUNTIME_EVENT_WEATHER_SUCCESS`，携带天气码与温度（放大 10 倍的整数）。

## 六、修改时的注意事项

1. **改 UI 文本表要同步改契约测试**：`tests/test_weather_condition_coverage.ps1` 明确禁止 `weather_text()` 出现细分降水分支。
2. **场景必须按数值 code 选**，不能用 `weather_text` 做字符串匹配——同一份契约测试里有对应断言。
3. **新增背景场景需要三处同步**：`ui_assets.c` 的资源表、`CMakeLists.txt` 的 `EMBED_FILES`、以及 `ui_service.c` 的选择逻辑。
4. **图标缺素材时会被归并到已有图标**（雾 → 云就是这种情况），补素材时记得同步这张表。
