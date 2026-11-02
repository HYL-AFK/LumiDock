#include "ds3231.h"

#include "board_pins.h"

#if !LUMIDOCK_DS3231_ENABLE

/* 模块还没接，只占着引脚。整个驱动退化成 stub，不碰 I2C。
 * 开关定义在 board_pins.h 的 LUMIDOCK_DS3231_ENABLE。 */
esp_err_t ds3231_init(void) { return ESP_ERR_NOT_SUPPORTED; }
bool ds3231_is_ready(void) { return false; }
esp_err_t ds3231_read(ds3231_snapshot_t *out)
{
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t ds3231_write_utc(const struct tm *utc)
{
    (void)utc;
    return ESP_ERR_NOT_SUPPORTED;
}
time_t ds3231_utc_to_unix(const struct tm *utc)
{
    (void)utc;
    return (time_t)-1;
}

#else

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

#define DS3231_REG_TIME 0x00U
#define DS3231_REG_STATUS 0x0FU
#define DS3231_TIME_REG_COUNT 7U

/* 状态寄存器 bit7：振荡器停振标志。电池没电、首次上电、或者芯片被强制
 * 断电重启之后它会被置位，此时寄存器里保留的是停振那一刻的值（出厂态是
 * 2000-01-01 00:00:00），不能当成真实时间。 */
#define DS3231_STATUS_OSF 0x80U

#define DS3231_HOUR_12H_MODE 0x40U
#define DS3231_HOUR_PM 0x20U
#define DS3231_HOUR_12H_MASK 0x1FU
#define DS3231_HOUR_24H_MASK 0x3FU
#define DS3231_MONTH_CENTURY 0x80U
#define DS3231_MONTH_MASK 0x1FU

#define DS3231_TRANSFER_TIMEOUT_MS 100
/* 400 kHz 拿不到应答时的第二档速度。见 ds3231_init() 里的说明。 */
#define DS3231_FALLBACK_CLOCK_HZ 100000U
/* 早于这个年份的读数一律视为无效。芯片出厂值是 2000 年，而设备不可能回到
 * 2020 年之前工作，用它当分界既挡得住出厂值，也容得下手动校准。 */
#define DS3231_YEAR_MIN 2020
#define DS3231_YEAR_MAX 2199

static const char *TAG = "ds3231";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint32_t s_clock_hz;

static uint8_t bcd_to_bin(uint8_t value)
{
    return (uint8_t)(((value >> 4U) * 10U) + (value & 0x0FU));
}

static uint8_t bin_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10U) << 4U) | (value % 10U));
}

/* 小时寄存器有 12/24 两种编码，芯片断电后可能停在任何一种。移植自 WiTime，
 * 那一版已经按这个思路处理过，这里保持一致。 */
static uint8_t decode_hour(uint8_t hour_reg)
{
    if ((hour_reg & DS3231_HOUR_12H_MODE) == 0U) {
        return bcd_to_bin(hour_reg & DS3231_HOUR_24H_MASK);
    }

    uint8_t hour = bcd_to_bin(hour_reg & DS3231_HOUR_12H_MASK);
    if (hour == 12U) hour = 0U;
    if ((hour_reg & DS3231_HOUR_PM) != 0U) hour = (uint8_t)(hour + 12U);
    return hour;
}

time_t ds3231_utc_to_unix(const struct tm *utc)
{
    if (utc == NULL) return (time_t)-1;

    /* days_from_civil（Howard Hinnant 的算法）：把公历年月日折成"1970-01-01
     * 起的天数"，对任意公历日期都成立，不用逐月累加也不用管闰年特例。
     * 之所以不调用现成的转换函数：newlib 没有声明 timegm()，而 mktime() 会按
     * 当前 TZ 解释输入——我们要的恰恰是"不解释时区"。 */
    int year = utc->tm_year + 1900;
    const int month = utc->tm_mon + 1;
    year -= month <= 2 ? 1 : 0;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = (unsigned)(year - era * 400);
    const unsigned day_of_year =
        (unsigned)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + utc->tm_mday - 1);
    const unsigned day_of_era =
        year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    const long long days = (long long)era * 146097LL + (long long)day_of_era - 719468LL;

    return (time_t)(days * 86400LL + (long long)utc->tm_hour * 3600LL +
                    (long long)utc->tm_min * 60LL + (long long)utc->tm_sec);
}

static esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    if (s_dev == NULL || data == NULL || len == 0U) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len,
                                       DS3231_TRANSFER_TIMEOUT_MS);
}

/* 真读一个寄存器，而不是只看地址有没有 ACK。地址能 ACK 但数据读回来是错的，
 * 说明上拉或走线有问题，这种"半通"状态比完全不通更难查，不如在这里就拦住。 */
static bool probe_once(void)
{
    uint8_t seconds = 0U;
    return read_regs(DS3231_REG_TIME, &seconds, 1) == ESP_OK;
}

static esp_err_t add_device(uint32_t clock_hz)
{
    const i2c_device_config_t device_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LUMIDOCK_DS3231_I2C_ADDRESS,
        .scl_speed_hz = clock_hz,
    };
    const esp_err_t err = i2c_master_bus_add_device(s_bus, &device_cfg, &s_dev);
    if (err == ESP_OK) {
        s_clock_hz = clock_hz;
    } else {
        s_dev = NULL;
    }
    return err;
}

static void drop_device(void)
{
    if (s_dev == NULL) return;
    (void)i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    s_clock_hz = 0U;
}

static esp_err_t ensure_bus(void)
{
    if (s_bus != NULL) return ESP_OK;

    const i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = LUMIDOCK_DS3231_I2C_PORT,
        .scl_io_num = LUMIDOCK_DS3231_I2C_SCL_GPIO,
        .sda_io_num = LUMIDOCK_DS3231_I2C_SDA_GPIO,
        .glitch_ignore_cnt = 7,
        /* 内部上拉只有 45 kΩ 量级，400 kHz 下带不动。DS3231 模块板一般自带
         * 4.7 kΩ，这里打开只是兜底，别指望它顶事。 */
        .flags.enable_internal_pullup = true,
    };
    const esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        s_bus = NULL;
        ESP_LOGW(TAG, "I2C bus init failed on SDA=%d SCL=%d: %s",
                 (int)LUMIDOCK_DS3231_I2C_SDA_GPIO, (int)LUMIDOCK_DS3231_I2C_SCL_GPIO,
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t ds3231_init(void)
{
    /* 每次都真读一次再决定。芯片可能被拔掉、电池可能刚换过，不能因为上一次
     * 成功过就一直当它在——这里多花一次 I2C 事务，换的是后面每次读失败时
     * 上层不用自己判断要不要重建。 */
    if (s_dev != NULL && probe_once()) return ESP_OK;
    drop_device();

    esp_err_t err = ensure_bus();
    if (err != ESP_OK) return err;

    /* 器件在不在，先用最轻的 i2c_master_probe 判断。它只发一个地址字节看
     * ACK，不建 device 句柄也不分配队列，栈开销比 add_device 小得多——
     * 没接芯片时这条路径每 5 秒就要走一次，而且调用方 snapshot_bridge
     * 只有 3072 字节栈，在这里省下的每一字节都是实打实的余量。 */
    err = i2c_master_probe(s_bus, LUMIDOCK_DS3231_I2C_ADDRESS,
                           DS3231_TRANSFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DS3231 not found: addr=0x%02x sda=%d scl=%d",
                 (unsigned)LUMIDOCK_DS3231_I2C_ADDRESS,
                 (int)LUMIDOCK_DS3231_I2C_SDA_GPIO,
                 (int)LUMIDOCK_DS3231_I2C_SCL_GPIO);
        return ESP_ERR_NOT_FOUND;
    }

    err = add_device(LUMIDOCK_DS3231_I2C_CLOCK_HZ);
    if (err != ESP_OK) return err;
    if (probe_once()) {
        ESP_LOGI(TAG, "DS3231 ready: addr=0x%02x sda=%d scl=%d %u Hz",
                 (unsigned)LUMIDOCK_DS3231_I2C_ADDRESS,
                 (int)LUMIDOCK_DS3231_I2C_SDA_GPIO,
                 (int)LUMIDOCK_DS3231_I2C_SCL_GPIO, (unsigned)s_clock_hz);
        return ESP_OK;
    }

    /* 400 kHz 下数据读不对。最常见的原因不是没接芯片，而是模块上没焊外部
     * 上拉，只靠 ESP32 内部那 45 kΩ 撑不住 400 kHz 的上升沿。降到 100 kHz
     * 再试一次：这个速度下内部上拉勉强够用，不至于因为一个电阻就把整个时钟
     * 功能判死。 */
    drop_device();
    err = add_device(DS3231_FALLBACK_CLOCK_HZ);
    if (err != ESP_OK) return err;
    if (probe_once()) {
        ESP_LOGW(TAG, "DS3231 only answered at %u Hz; check the module pull-ups",
                 (unsigned)DS3231_FALLBACK_CLOCK_HZ);
        return ESP_OK;
    }
    drop_device();
    ESP_LOGW(TAG, "DS3231 answered but returned bad data: addr=0x%02x sda=%d scl=%d",
             (unsigned)LUMIDOCK_DS3231_I2C_ADDRESS,
             (int)LUMIDOCK_DS3231_I2C_SDA_GPIO,
             (int)LUMIDOCK_DS3231_I2C_SCL_GPIO);
    return ESP_ERR_NOT_FOUND;
}

bool ds3231_is_ready(void)
{
    return s_dev != NULL;
}

esp_err_t ds3231_read(ds3231_snapshot_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (s_dev == NULL) return ESP_ERR_INVALID_STATE;

    uint8_t regs[DS3231_TIME_REG_COUNT] = {0};
    esp_err_t err = read_regs(DS3231_REG_TIME, regs, sizeof(regs));
    if (err != ESP_OK) return err;
    err = read_regs(DS3231_REG_STATUS, &out->status, 1);
    if (err != ESP_OK) return err;

    out->utc.tm_sec = (int)bcd_to_bin(regs[0] & 0x7FU);
    out->utc.tm_min = (int)bcd_to_bin(regs[1] & 0x7FU);
    out->utc.tm_hour = (int)decode_hour(regs[2]);
    /* day 寄存器是 1~7，tm_wday 是 0~6。这个字段不参与换算（时间戳只看年月日
     * 时分秒），留着是为了日志里能对上人手填的星期。 */
    out->utc.tm_wday = (int)bcd_to_bin(regs[3] & 0x07U) - 1;
    out->utc.tm_mday = (int)bcd_to_bin(regs[4] & 0x3FU);
    out->utc.tm_mon = (int)bcd_to_bin(regs[5] & DS3231_MONTH_MASK) - 1;
    out->utc.tm_year = (int)bcd_to_bin(regs[6]) + 100;
    if ((regs[5] & DS3231_MONTH_CENTURY) != 0) out->utc.tm_year += 100;
    out->utc.tm_isdst = 0;

    const int year = out->utc.tm_year + 1900;
    const bool oscillator_ran = (out->status & DS3231_STATUS_OSF) == 0U;
    out->time_valid = oscillator_ran && year >= DS3231_YEAR_MIN && year <= DS3231_YEAR_MAX;
    return ESP_OK;
}

esp_err_t ds3231_write_utc(const struct tm *utc)
{
    if (utc == NULL) return ESP_ERR_INVALID_ARG;
    if (s_dev == NULL) return ESP_ERR_INVALID_STATE;

    const int year = utc->tm_year + 1900;
    if (year < 2000 || year > DS3231_YEAR_MAX) return ESP_ERR_INVALID_ARG;
    if (utc->tm_mon < 0 || utc->tm_mon > 11) return ESP_ERR_INVALID_ARG;
    if (utc->tm_mday < 1 || utc->tm_mday > 31) return ESP_ERR_INVALID_ARG;
    if (utc->tm_hour < 0 || utc->tm_hour > 23) return ESP_ERR_INVALID_ARG;
    if (utc->tm_min < 0 || utc->tm_min > 59) return ESP_ERR_INVALID_ARG;
    if (utc->tm_sec < 0 || utc->tm_sec > 59) return ESP_ERR_INVALID_ARG;

    uint8_t month_reg = bin_to_bcd((uint8_t)(utc->tm_mon + 1));
    uint8_t year_reg = 0U;
    if (year >= 2100) {
        month_reg |= DS3231_MONTH_CENTURY;
        year_reg = bin_to_bcd((uint8_t)(year - 2100));
    } else {
        year_reg = bin_to_bcd((uint8_t)(year - 2000));
    }

    /* day 寄存器只接受 1~7。调用方给来的 tm_wday 可能是 -1（gmtime_r 之外的
     * 来源），钳到 0 再 +1，别往芯片里写 0 这种非法值。 */
    const int wday = (utc->tm_wday >= 0 && utc->tm_wday <= 6) ? utc->tm_wday : 0;
    const uint8_t payload[8] = {
        DS3231_REG_TIME,
        bin_to_bcd((uint8_t)utc->tm_sec),
        bin_to_bcd((uint8_t)utc->tm_min),
        bin_to_bcd((uint8_t)utc->tm_hour),
        bin_to_bcd((uint8_t)(wday + 1)),
        bin_to_bcd((uint8_t)utc->tm_mday),
        month_reg,
        year_reg,
    };
    esp_err_t err = i2c_master_transmit(s_dev, payload, sizeof(payload),
                                        DS3231_TRANSFER_TIMEOUT_MS);
    if (err != ESP_OK) return err;

    /* 写进去的时间一定比"停振后残留的出厂值"可信，OSF 可以清了。读-改-写：
     * 直接写 0 会把闹钟标志一起抹掉。清不掉也不算失败，下次上电重判即可。 */
    uint8_t status = 0U;
    if (read_regs(DS3231_REG_STATUS, &status, 1) == ESP_OK &&
        (status & DS3231_STATUS_OSF) != 0U) {
        const uint8_t clear_osf[2] = {
            DS3231_REG_STATUS,
            (uint8_t)(status & (uint8_t)~DS3231_STATUS_OSF),
        };
        (void)i2c_master_transmit(s_dev, clear_osf, sizeof(clear_osf),
                                  DS3231_TRANSFER_TIMEOUT_MS);
    }
    return ESP_OK;
}

#endif /* LUMIDOCK_DS3231_ENABLE */
