#ifndef BMS_SERVICE_H
#define BMS_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define BMS_ID_TEXT_LEN 16
#define BMS_SOFT_ID_TEXT_LEN 5
#define BMS_BATTERY_CODE_TEXT_LEN 14
#define BMS_GLOBAL_ASSET_TEXT_LEN 22
#define BMS_MAX_CELL_COUNT 15
#define BMS_TEMP_COUNT 5

typedef enum {
    BMS_MATERIAL_UNKNOWN = 0,
    BMS_MATERIAL_TERNARY_14S,
    BMS_MATERIAL_LFP_15S,
} bms_material_t;

typedef struct {
    bool online;
    bool parsed_valid;
    bms_material_t material;
    uint8_t cell_count;
    char bms_id[BMS_ID_TEXT_LEN + 1];
    char soft_id[BMS_SOFT_ID_TEXT_LEN + 1];
    char battery_code[BMS_BATTERY_CODE_TEXT_LEN + 1];
    char global_asset_number[BMS_GLOBAL_ASSET_TEXT_LEN + 1];
    int32_t pack_mv;
    int32_t current_ma;
    int32_t soc_permille;
    uint16_t cell_mv[BMS_MAX_CELL_COUNT];
    int16_t temp_c[BMS_TEMP_COUNT];
    uint8_t rsoc_percent;
    uint8_t asoc_percent;
    uint16_t remaining_capacity_mah;
    uint16_t full_charge_capacity_mah;
    uint16_t cycle_count;
    uint16_t soh_percent;
    uint8_t battery_status;
    uint8_t charge_mos_state;
    uint8_t discharge_mos_state;
    uint8_t protect_1;
    uint8_t protect_2;
    uint8_t protect_3;
    uint8_t work_mode;
    uint8_t predischarge_mos_state;
    uint32_t rx_frames;
    uint32_t parsed_frames;
    uint32_t crc_error_count;
    uint32_t parse_error_count;
    uint32_t timeout_count;
    uint32_t last_rx_ms;
    uint32_t last_parse_ms;
    uint16_t last_crc_calc;
    uint16_t last_crc_rx_le;
    uint16_t last_crc_rx_be;
} bms_info_t;

esp_err_t bms_service_init(void);
void bms_service_poll(void);
esp_err_t bms_protocol_parse(const uint8_t *frame, size_t len, bms_info_t *out_info);
bool bms_service_get_info(bms_info_t *out_info);

#endif
