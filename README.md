# LumiDock ESP32-S3

## 1.0 中文版本备注

本版本为 LumiDock ESP32-S3 固件 1.0，重点增强 XiaoZhi 语音链路的终止稳定性：

- VAD 结束后，150ms 仅用于尾音捕获，不会中断已经开始的 `bin-main` WebSocket 写入。
- 主语音写入完成后，终止路径按顺序发送最多 2 帧尾音和 `listen/stop`，避免 TX 任务抢先关闭当前会话。
- 增加主写入等待耗时、写入结果、连接状态、主动问候首个可解码 PCM、短包和 Opus 解码失败计数日志。
- 保留单 WSS/session、`mode:"manual"`、主动问候直接回复窗口及播放后静音门控。

本版本已完成静态语音契约测试；构建和烧录请在本地 ESP-IDF 环境中执行，并通过连续主动问候及追问采集实机日志。

当前工程是 LumiDock 的第一版 ESP-IDF 软件骨架，按四层组织：

```text
main/APP   启动编排、页面服务
main/COM   Wi-Fi、蓝牙配网接口、时间天气、BMS、SW3518S 业务通信
main/MID   统一快照和事件优先级
main/Dri   ST7789、I2C、UART、雷达、WS2812、按键、MOS 硬件驱动
```

## 已实现

- ST7789 开机页和主页面：电池电量优先显示，其次是时间、时区、天气、实时温度、Wi-Fi、BT、充电协议、BMS 在线状态；电量和天气使用填充位图式图形，不使用线框图标。
- SW3518S I2C 只读轮询：7 位地址 `0x3C`，SDA=`GPIO15`，SCL=`GPIO16`，每秒读取协议和电压/电流状态。
- VoltPilot BMS 协议适配：`ESCOREDATA` 请求、`AAA0AAA` 帧头、Modbus CRC16、128/130/132 字节帧兼容解析；UART2 TX=`GPIO37`、RX=`GPIO38`，无 DE/RE。解析器按 14S/15S 字段顺序读取 6 个状态字节，并兼容不同固件追加的保留字节。
- WiTime 方案的 APSTA、标准 BluFi、SNTP/时区、定位和 Open-Meteo 天气任务已复制到 `main/COM/WiTime` 并由 COM 适配层调用。
- 雷达按当前引脚图使用 `GPIO44` 有效高输出；UART 距离帧暂不占用 WS2812 数据脚。
- MOS 上电默认为关闭；单线 UART、按键行为、MOS 实际负载策略均保留接口。

## 构建

在已安装 ESP-IDF 的终端中执行：

```text
idf.py set-target esp32s3
idf.py build
```

本工程按 ESP32-S3 N8R2 配置，固件镜像和分区表均使用 8MB Flash。烧录前请确认 `main/Dri/board_pins.h` 与实物焊盘一致。

## 事件优先级

```text
硬件保护/故障 > 系统控制 > 本地按键 > 雷达 > 充电/BMS/外设通信 > 云端 > ASR > UI刷新
```

## 暂缓项

单线 UART 协议、MOS1/MOS2 负载、按键短按/长按/双击行为、MQTT/HTTP 云端选型、ASR 云端/离线方案，以及电池详细页面字段等待最终协议确认。
