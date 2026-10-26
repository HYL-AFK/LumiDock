# LumiDock

LumiDock 是一台桌面智能终端固件，跑在 ESP32-S3（N8R2：8 MB Flash + 2 MB PSRAM）上，
把语音助手、网络电台、天气时间与充电管理收进一块 320×170 的小屏里。

## 目录结构

`main/` 按四层组织、路径扁平。`tests/test_architecture_layout.ps1` 与
`tests/test_layer_file_placement.ps1` 会检查它们没有倒退到旧布局。

| 层 | 职责 |
|---|---|
| `main/APP` | 应用编排与界面：启动协调、页面服务、字体与图像资产 |
| `main/COM` | 只放跨层共享的公共物：`app_config`、`runtime_event_log` 与三份快照（location / time / weather） |
| `main/Dri` | 硬件驱动与底层策略：ST7789、I2S、WS2812、按键、Wi-Fi 管理、公网健康判定 |
| `main/MID` | 业务中间层，按领域分四个子目录 |

`main/MID` 的划分由 `tests/test_mid_directory_layout.ps1` 固化：

- `network/` — Wi-Fi 与蓝牙配网、SNTP 时间、天气、新闻、电价
- `protocol/` — BMS 电池、STC8H 顶板链路
- `system/` — MCP 工具、电台、音频播放、定时器、充电策略、设置持久化、状态灯、看门狗
- `voice/` — 小智语音链路：激活、音频管线、会话

业务服务一律沉到 `MID`，`COM` 只留公共快照与配置。历史上的 `COM/WiTime` 目录已废弃，
契约测试会拒绝它重新出现。

## 主要功能

- **语音助手**：小智云端链路。单条 WSS 会话 + 唤醒词（WakeNet）+ VAD，Opus 编解码，
  云端 TTS 播放。开机问候语由云端 LLM 现场生成，固件不再拼接本地音频片段。
- **网络电台**：央广四台 HLS（AAC/TS）直播，支持「打开电台」「换一个台」。
- **充电策略**：结合电价时段、明天气温与降水、当前 SOC 给出充电建议，经 MCP 交给云端播报。
- **新闻播报**：四个分类源，刷新标题缓存后供云端朗读。
- **天气与时间**：SNTP 授时 + 时区换算，Open-Meteo 当前天气与两日预报。
- **定时器**：闹钟、倒计时、正计时，配本地提示音。
- **配网**：BluFi。首次无凭据走 `BLE_WINDOW_PROVISION`（超时进深睡）；已配网设备开机时
  走 `BLE_WINDOW_BOOT`（短窗口，超时恢复联网）。
- **电源与协议**：SW3518S 快充状态（I2C 只读）、VoltPilot BMS（UART2）、STC8H 顶板（UART0）。
- **界面**：LVGL 9，开机动画、主页面、充电页、待机表情、天气场景。

## 硬件

引脚集中在 `main/Dri/board_pins.h`，改动前请与实物核对。

| 功能 | 引脚 |
|---|---|
| LCD（SPI2） | SCLK=1, MOSI=2, RST=11, DC=4, CS=5, BL=6 |
| 音频（I2S） | BCLK=10, LRCLK=9, MIC=8, AMP=7, MCLK=38 |
| ES8311（I2C，400 kHz） | SDA=17, SCL=18 |
| 按键 | 12 |
| WS2812 | 灯带=13/14（24 颗），板载=48 |
| BMS（UART2） | TX=39, RX=40 |
| STC8H（UART0） | TX=43, RX=44 |

## 构建

需要 ESP-IDF v5.3.x：

```text
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Flash 为 8 MB 单 app 分区、无 OTA，见表 `partitions.csv`。配置以 `sdkconfig.defaults`
为准；`sdkconfig` 是生成物，不入库。

## 运行时约定

改动相关代码时请一并遵守，`tests/*.ps1` 会逐条断言：

- **单条 WSS 会话**：同一时刻只允许一条小智链路，收场与重连由 `xiaozhi_voice_service`
  统一编排。
- **任务栈的内存归属**：会写 Flash/NVS 的任务必须用内部 RAM（PSRAM 上的栈不能写 Flash），
  其余才允许放 PSRAM。见各服务里的 `xTaskCreatePinnedToCoreWithCaps`。
- **音频设备单一持有者**：Opus 编解码与 I2S 通道由音频管线统一持有，不重复创建。
- **电台与语音共存**：电台播放期间唤醒语音会先让出扬声器，收场时再按收场性质决定
  归还电台还是停掉它。

验证请在本地运行 `tests/` 下的 PowerShell 契约测试。

## 相关文档

- `docs/天气接口字段与WMO码映射.md`
- `docs/xiaozhi-voice-test-handoff-2026-08-12.md`
