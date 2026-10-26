#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * 设备数据区。
 *
 * 目的：把"这台设备是谁、跑了多久、用了多少流量"收拢到一处可持久化的记录里，
 * 而不是让每个模块各算各的。
 *
 * 概念上分三类，但只有两类在这里落盘：
 *
 *   生产数据 dev_factory —— 首次开机采集一次，之后只读。恢复出厂设置也不清除：
 *                           清掉它设备就丢了自己的身份（序列号、MAC、出厂时刻）。
 *   系统数据 dev_system  —— 开机次数、运行时长、激活时刻、流量累计，自动累积。
 *   用户数据             —— 音量、Wi-Fi 凭据、闹钟、时区等，沿用 app_cfg /
 *                           device_cfg / lumidock_timer / location 这些既有命名空间。
 *                           这里不重建：迁移等于读旧→写新→删旧，中途掉电就会丢配网
 *                           凭据和闹钟，代价远大于"命名空间不统一"这点别扭。
 *
 * 两份记录都带 magic/version/crc32，字段将来要改时先抬 version，读取端据此决定
 * 是迁移还是丢弃重建。
 *
 * 读写约定：
 *   - device_data_init() 必须在 NVS 初始化之后、其余服务之前调用；它会把开机记录
 *     立即落盘一次。
 *   - 两个 getter 返回内存镜像的只读指针，不要改，也不要跨线程长期持有。
 *   - 流量是热路径数据，读实时值请走 device_data_traffic_get()。
 */

/* 生产数据：首次开机写一次，此后只读。 */
#define DEVICE_DATA_CHIP_MODEL_MAX 16
#define DEVICE_DATA_FW_VERSION_MAX 32
#define DEVICE_DATA_BUILD_STAMP_MAX 16
#define DEVICE_DATA_IDF_VERSION_MAX 32
#define DEVICE_DATA_SERIAL_MAX 24

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;

    /* 硬件 */
    char chip_model[DEVICE_DATA_CHIP_MODEL_MAX]; /* "ESP32-S3" */
    uint8_t chip_revision;
    uint8_t core_count;
    uint8_t mac[6];
    uint32_t flash_size_bytes;
    uint32_t psram_size_bytes;
    uint8_t board_revision; /* 目前只有一块板，换板时改这里 */
    uint8_t reserved2[3];

    /* 软件 */
    char fw_version[DEVICE_DATA_FW_VERSION_MAX];
    char fw_build_date[DEVICE_DATA_BUILD_STAMP_MAX];
    char fw_build_time[DEVICE_DATA_BUILD_STAMP_MAX];
    char idf_version[DEVICE_DATA_IDF_VERSION_MAX];

    /* 生产 */
    char serial[DEVICE_DATA_SERIAL_MAX]; /* 由 MAC 派生，全球唯一且可追溯 */
    uint64_t manufacture_ts;             /* 出厂/首次开机时刻，时间同步后补齐 */

    uint32_t crc32;
} device_data_factory_t;

/* 系统数据：自动累积，周期落盘。 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;

    uint32_t boot_count;         /* 累计开机次数 */
    uint32_t abnormal_resets;    /* 看门狗/panic/欠压 等异常复位次数 */
    uint32_t last_reset_reason;  /* esp_reset_reason() 的原始值 */
    uint32_t reserved2;

    uint64_t first_boot_ts;      /* 首次开机时刻（= 出厂时刻的另一种记法） */
    uint64_t last_boot_ts;       /* 最近一次开机时刻 */
    uint64_t activated_ts;       /* 激活成功时刻 */
    uint64_t total_uptime_s;     /* 累计运行时长（秒） */

    uint64_t traffic_total_rx;   /* 累计接收字节 */
    uint64_t traffic_total_tx;   /* 累计发送字节 */
    uint64_t traffic_boot_rx;    /* 本次开机接收字节 */
    uint64_t traffic_boot_tx;    /* 本次开机发送字节 */

    uint8_t activated;
    uint8_t reserved3[7];

    uint32_t crc32;
} device_data_system_t;

/* 流量的实时快照。结构体里的 traffic_* 字段是"上次落盘时"的值，
 * 要当前值就用下面的 getter。 */
typedef struct {
    uint64_t total_rx;
    uint64_t total_tx;
    uint64_t boot_rx;
    uint64_t boot_tx;
} device_data_traffic_t;

esp_err_t device_data_init(void);

const device_data_factory_t *device_data_factory(void);
const device_data_system_t *device_data_system(void);
void device_data_traffic_get(device_data_traffic_t *out);

/* 激活成功时调用一次，记下激活时刻（幂等，重复调用不会覆盖首次时间）。
 *
 * 只改内存、不落盘：调用方（激活任务）的栈在 PSRAM 上，而 NVS 写入会禁用 SPI
 * Cache，在 PSRAM 栈上写 NVS 会直接断言崩溃。时刻由周期 flush 带出去，落盘前
 * 掉电丢的只是这个时间戳，激活状态本身由 xiaozhi 命名空间的 client_id 决定。 */
void device_data_mark_activated(void);

/* 流量累加：热路径，只碰内存和自旋锁，不写 NVS，任何栈上都能调。 */
void device_data_traffic_add_rx(size_t bytes);
void device_data_traffic_add_tx(size_t bytes);

/* 强制落盘。关机、进入配网这类"接下来可能没人再调 flush"的节点上调用。
 * 注意：它会写 NVS，调用者所在任务的栈必须落在内部 RAM；PSRAM 栈上调用会崩
 * （原因同上）。 */
esp_err_t device_data_flush(void);
