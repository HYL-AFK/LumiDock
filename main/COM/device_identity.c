#include "device_identity.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_mac.h"
#include "esp_system.h"

#include "app_log.h"

#define TAG "device_identity"

/*
 * 名字只在第一次问的时候算一遍。
 *
 * 蓝牙在 NimBLE sync 里问一次，WiFi 在 manager init 里问一次，两边可能分属不同
 * 任务；算出来的结果对同一颗芯片是恒定的，所以先算完再置 ready 就够了，这里没有
 * 加锁的价值。真出现两个任务同时首次调用，最坏结果也只是各自把同一个字符串写进
 * 同一个缓冲，内容逐字节相同。
 */
static char s_name[DEVICE_IDENTITY_NAME_MAX + 1];
static bool s_ready;

const char *device_identity_name(void)
{
    if (s_ready) {
        return s_name;
    }

    /* 取 STA 的 MAC（也就是 base MAC），不是蓝牙那个。
     *
     * ESP32-S3 一次分配三个连号地址：STA = base、AP = base + 1、BT = base + 2。
     * 以前蓝牙广播名读的是 ESP_MAC_BT，热点名干脆不读 MAC，两边各说各的。改成
     * 双双读 STA 之后，热点名后缀和蓝牙名后缀逐字符相同，扫一眼就知道是一台机器。
     *
     * 不选 AP 的 MAC 是因为 base 才是这台设备的"主"地址——device_data.c 上报
     * 设备 MAC 用的就是它，日后要拿名字后缀去对任何一处登记信息都对得上。 */
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        /* eFuse 读不出来属于硬件级异常，但这时候更需要一个能看见的名字：退回纯
           前缀，至少手机列表里还能认出是哪一代产品，而不是一串空白。 */
        APP_LOG_ERR(TAG, err, "read STA MAC failed, falling back to the bare prefix");
        strlcpy(s_name, DEVICE_IDENTITY_PREFIX, sizeof(s_name));
        s_ready = true;
        return s_name;
    }

    /* 后三个字节即人们常说的"MAC 后六位"（A1B2C3 这种六位十六进制）。
     *
     * 大写是手机扫描列表里最常见的写法，也是这台机器日志里 MAC 的一贯写法，
     * 混着小写会让人以为是两个不同的串。 */
    snprintf(s_name, sizeof(s_name), "%s_%02X%02X%02X",
             DEVICE_IDENTITY_PREFIX, mac[3], mac[4], mac[5]);
    s_ready = true;
    APP_LOGI(TAG, "device name: %s", s_name);
    return s_name;
}
