#include "bms_protocol.h"

#include <stdbool.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "bms_protocol";

enum {
    BMS_BODY_OFF_ID = 0,
    BMS_BODY_OFF_SOFT_ID = BMS_BODY_OFF_ID + BMS_ID_TEXT_LEN,
    BMS_BODY_OFF_PACK_MV = BMS_BODY_OFF_SOFT_ID + BMS_SOFT_ID_TEXT_LEN,
    BMS_BODY_OFF_CURRENT_MA = BMS_BODY_OFF_PACK_MV + 4,
    BMS_BODY_OFF_CELL_MV = BMS_BODY_OFF_CURRENT_MA + 4,
};

static uint16_t read_be16(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8) | data[1];
}

static int16_t read_be16_signed(const uint8_t *data)
{
    return (int16_t)read_be16(data);
}

static int32_t read_be32_signed(const uint8_t *data)
{
    uint32_t value = ((uint32_t)data[0] << 24) |
                     ((uint32_t)data[1] << 16) |
                     ((uint32_t)data[2] << 8) |
                     data[3];
    return (int32_t)value;
}

static void copy_ascii_field(char *dst, size_t dst_size, const uint8_t *src, size_t src_len)
{
    size_t copy_len = src_len < dst_size - 1 ? src_len : dst_size - 1;

    for (size_t i = 0; i < copy_len; i++) {
        uint8_t ch = src[i];
        dst[i] = (ch >= 0x20 && ch <= 0x7e) ? (char)ch : '.';
    }
    dst[copy_len] = '\0';
}

static bms_material_t material_from_soft_id(const uint8_t *soft_id)
{
    if ((soft_id[0] == '0' && soft_id[1] == '1') || soft_id[0] == 0x01) {
        return BMS_MATERIAL_TERNARY_14S;
    }
    if ((soft_id[0] == '0' && soft_id[1] == '2') || soft_id[0] == 0x02) {
        return BMS_MATERIAL_LFP_15S;
    }
    return BMS_MATERIAL_UNKNOWN;
}

/* 电池编码末两位为材质代码和串数，例如 6E=三元材料-14串。 */
static bms_material_t material_from_asset_number(const char *asset_number)
{
    /* 资产编码示例：EH1241028KNL6E00000037，6E 位于下标 12~13。 */
    if (asset_number == NULL || strlen(asset_number) < 14) {
        return BMS_MATERIAL_UNKNOWN;
    }

    if (asset_number[12] == '6' && asset_number[13] == 'E') {
        return BMS_MATERIAL_TERNARY_14S;
    }
    if (asset_number[12] == '3' && asset_number[13] == 'F') {
        return BMS_MATERIAL_LFP_15S;
    }
    return BMS_MATERIAL_UNKNOWN;
}

const char *bms_material_name(bms_material_t material)
{
    switch (material) {
    case BMS_MATERIAL_TERNARY_14S:
        return "TERNARY_14S";
    case BMS_MATERIAL_LFP_15S:
        return "LFP_15S";
    case BMS_MATERIAL_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

uint16_t bms_protocol_crc16_modbus(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xffff;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if ((crc & 0x0001) != 0) {
                crc = (crc >> 1) ^ 0xa001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

size_t bms_protocol_expected_len_from_header(const uint8_t *frame, size_t len)
{
    if (frame == NULL || len < BMS_PROTOCOL_HEADER_LEN + BMS_BODY_OFF_SOFT_ID + BMS_SOFT_ID_TEXT_LEN) {
        return 0;
    }
    if (memcmp(frame, BMS_PROTOCOL_HEADER_TEXT, BMS_PROTOCOL_HEADER_LEN) != 0) {
        return 0;
    }

    /* soft_id 不是电池型号，不能用它决定 14S/15S 帧长度。 */
    return 0;
}

bool bms_protocol_crc_is_valid(const uint8_t *frame, size_t len, bms_crc_result_t *out_result)
{
    if (frame == NULL || len < BMS_PROTOCOL_HEADER_LEN + 2) {
        return false;
    }

    uint16_t calc = bms_protocol_crc16_modbus(frame, len - 2);
    uint16_t calc_body = bms_protocol_crc16_modbus(frame + BMS_PROTOCOL_HEADER_LEN,
                                                   len - BMS_PROTOCOL_HEADER_LEN - 2);
    uint16_t rx_le = (uint16_t)frame[len - 2] | ((uint16_t)frame[len - 1] << 8);
    uint16_t rx_be = ((uint16_t)frame[len - 2] << 8) | frame[len - 1];

    if (out_result != NULL) {
        out_result->calc_full = calc;
        out_result->calc_body = calc_body;
        out_result->rx_le = rx_le;
        out_result->rx_be = rx_be;
    }

    return calc == rx_le || calc == rx_be || calc_body == rx_le || calc_body == rx_be;
}

static bool ascii_field_is_valid(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (data[i] < 0x20 || data[i] > 0x7e) {
            return false;
        }
    }
    return true;
}

static esp_err_t parse_body_layout(const uint8_t *body, size_t body_len, uint8_t cell_count,
                                   const bms_info_t *base, bms_info_t *out_info)
{
    size_t cells_bytes = (size_t)cell_count * 2;
    /*
     * VoltPilot's field order contains six status bytes before the two ASCII
     * identifiers.  Some firmware revisions append up to four reserved
     * bytes before the CRC, so the body length is not identical to the
     * decoded field length.
     */
    size_t decoded_body_bytes = BMS_BODY_OFF_CELL_MV + cells_bytes +
                                (BMS_TEMP_COUNT * 2) + 1 + 1 + 2 + 2 + 2 + 2 +
                                6 + BMS_BATTERY_CODE_TEXT_LEN +
                                BMS_GLOBAL_ASSET_TEXT_LEN + 1 + 1;
    if (body_len < decoded_body_bytes || body_len - decoded_body_bytes > 4) {
        return ESP_ERR_INVALID_SIZE;
    }

    bms_info_t parsed = *base;
    memset(parsed.cell_mv, 0, sizeof(parsed.cell_mv));
    memset(parsed.temp_c, 0, sizeof(parsed.temp_c));
    parsed.material = cell_count == 15 ? BMS_MATERIAL_LFP_15S : BMS_MATERIAL_TERNARY_14S;
    parsed.cell_count = cell_count;
    copy_ascii_field(parsed.bms_id, sizeof(parsed.bms_id), body + BMS_BODY_OFF_ID, BMS_ID_TEXT_LEN);
    copy_ascii_field(parsed.soft_id, sizeof(parsed.soft_id), body + BMS_BODY_OFF_SOFT_ID,
                     BMS_SOFT_ID_TEXT_LEN);
    parsed.pack_mv = read_be32_signed(body + BMS_BODY_OFF_PACK_MV);
    parsed.current_ma = read_be32_signed(body + BMS_BODY_OFF_CURRENT_MA);

    size_t offset = BMS_BODY_OFF_CELL_MV;
    for (uint8_t i = 0; i < cell_count; i++) {
        parsed.cell_mv[i] = read_be16(body + offset);
        if (parsed.cell_mv[i] < 1500 || parsed.cell_mv[i] > 5000) {
            ESP_LOGI(TAG, "layout=%uS reject cell[%u]=%umV", cell_count, i, parsed.cell_mv[i]);
            return ESP_ERR_INVALID_RESPONSE;
        }
        offset += 2;
    }
    for (uint8_t i = 0; i < BMS_TEMP_COUNT; i++) {
        parsed.temp_c[i] = read_be16_signed(body + offset);
        if (parsed.temp_c[i] < -40 || parsed.temp_c[i] > 125) {
            ESP_LOGI(TAG, "layout=%uS reject temp[%u]=%dC", cell_count, i, parsed.temp_c[i]);
            return ESP_ERR_INVALID_RESPONSE;
        }
        offset += 2;
    }

    parsed.rsoc_percent = body[offset++];
    parsed.asoc_percent = body[offset++];
    parsed.soc_permille = (int32_t)parsed.rsoc_percent * 10;
    parsed.remaining_capacity_mah = read_be16(body + offset);
    offset += 2;
    parsed.full_charge_capacity_mah = read_be16(body + offset);
    offset += 2;
    parsed.cycle_count = read_be16(body + offset);
    offset += 2;
    parsed.soh_percent = read_be16(body + offset);
    offset += 2;
    if (parsed.pack_mv <= 0 || parsed.rsoc_percent > 100 || parsed.asoc_percent > 100 ||
        parsed.soh_percent > 100) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    parsed.battery_status = body[offset++];
    parsed.charge_mos_state = body[offset++];
    parsed.discharge_mos_state = body[offset++];
    parsed.protect_1 = body[offset++];
    parsed.protect_2 = body[offset++];
    parsed.protect_3 = body[offset++];

    if (!ascii_field_is_valid(body + offset, BMS_BATTERY_CODE_TEXT_LEN) ||
        !ascii_field_is_valid(body + offset + BMS_BATTERY_CODE_TEXT_LEN,
                              BMS_GLOBAL_ASSET_TEXT_LEN)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    copy_ascii_field(parsed.battery_code, sizeof(parsed.battery_code), body + offset,
                     BMS_BATTERY_CODE_TEXT_LEN);
    offset += BMS_BATTERY_CODE_TEXT_LEN;
    copy_ascii_field(parsed.global_asset_number, sizeof(parsed.global_asset_number), body + offset,
                     BMS_GLOBAL_ASSET_TEXT_LEN);
    offset += BMS_GLOBAL_ASSET_TEXT_LEN;
    parsed.work_mode = body[offset++];
    parsed.predischarge_mos_state = body[offset++];

    /* Ignore revision-specific reserved bytes between the payload and CRC. */
    if (offset > body_len || body_len - offset > 4) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_info = parsed;
    ESP_LOGI(TAG, "layout=%uS valid reserved_bytes=%u asset=%s", cell_count,
             (unsigned)(body_len - offset), parsed.global_asset_number);
    return ESP_OK;
}

static int64_t pack_voltage_error(const bms_info_t *info)
{
    int64_t cell_sum = 0;
    for (uint8_t i = 0; i < info->cell_count; i++) {
        cell_sum += info->cell_mv[i];
    }
    int64_t error = cell_sum - info->pack_mv;
    return error < 0 ? -error : error;
}

esp_err_t bms_protocol_parse(const uint8_t *frame, size_t len, bms_info_t *out_info)
{
    if (frame == NULL || out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len != BMS_PROTOCOL_FRAME_LEN_128 && len != BMS_PROTOCOL_FRAME_LEN_14S &&
        len != BMS_PROTOCOL_FRAME_LEN_132) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(frame, BMS_PROTOCOL_HEADER_TEXT, BMS_PROTOCOL_HEADER_LEN) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    bms_crc_result_t crc;
    if (!bms_protocol_crc_is_valid(frame, len, &crc)) {
        out_info->last_crc_calc = crc.calc_body;
        out_info->last_crc_rx_le = crc.rx_le;
        out_info->last_crc_rx_be = crc.rx_be;
        return ESP_ERR_INVALID_CRC;
    }
    out_info->last_crc_calc = crc.calc_full == crc.rx_le || crc.calc_full == crc.rx_be
                                  ? crc.calc_full
                                  : crc.calc_body;
    out_info->last_crc_rx_le = crc.rx_le;
    out_info->last_crc_rx_be = crc.rx_be;

    const uint8_t *body = frame + BMS_PROTOCOL_HEADER_LEN;
    size_t body_len = len - BMS_PROTOCOL_HEADER_LEN - 2;
    bms_info_t layout14 = *out_info;
    bms_info_t layout15 = *out_info;
    esp_err_t err14 = parse_body_layout(body, body_len, 14, out_info, &layout14);
    esp_err_t err15 = parse_body_layout(body, body_len, 15, out_info, &layout15);

    if (err14 != ESP_OK && err15 != ESP_OK) {
        return err14 == ESP_ERR_INVALID_RESPONSE || err15 == ESP_ERR_INVALID_RESPONSE
                   ? ESP_ERR_INVALID_RESPONSE
                   : ESP_ERR_INVALID_SIZE;
    }

    bms_info_t *selected = NULL;
    if (err14 == ESP_OK && err15 != ESP_OK) {
        selected = &layout14;
    } else if (err15 == ESP_OK && err14 != ESP_OK) {
        selected = &layout15;
    } else {
        /* The published frame sizes identify the normal chemistry layouts. */
        if (len == BMS_PROTOCOL_FRAME_LEN_14S) {
            selected = &layout14;
        } else if (len == BMS_PROTOCOL_FRAME_LEN_132) {
            selected = &layout15;
        }
    }

    if (selected == NULL) {
        bms_material_t asset14 = material_from_asset_number(layout14.global_asset_number);
        bms_material_t asset15 = material_from_asset_number(layout15.global_asset_number);
        if (asset14 == BMS_MATERIAL_TERNARY_14S) {
            selected = &layout14;
        } else if (asset15 == BMS_MATERIAL_LFP_15S) {
            selected = &layout15;
        } else {
            bms_material_t soft_material = material_from_soft_id(body + BMS_BODY_OFF_SOFT_ID);
            if (soft_material == BMS_MATERIAL_TERNARY_14S) {
                selected = &layout14;
            } else if (soft_material == BMS_MATERIAL_LFP_15S) {
                selected = &layout15;
            } else {
                selected = pack_voltage_error(&layout14) <= pack_voltage_error(&layout15)
                               ? &layout14
                               : &layout15;
            }
        }
    }

    bms_material_t soft_material = material_from_soft_id(body + BMS_BODY_OFF_SOFT_ID);
    bms_material_t asset_material = material_from_asset_number(selected->global_asset_number);
    bms_material_t layout_material = selected->cell_count == 15 ? BMS_MATERIAL_LFP_15S
                                                                 : BMS_MATERIAL_TERNARY_14S;
    selected->material = layout_material;
    if ((soft_material != BMS_MATERIAL_UNKNOWN && soft_material != layout_material) ||
        (asset_material != BMS_MATERIAL_UNKNOWN && asset_material != layout_material)) {
        ESP_LOGW(TAG,
                 "BMS 型号字段不一致 selected=%uS softid=%s asset=%s，保留有效字段布局",
                 selected->cell_count, selected->soft_id, selected->global_asset_number);
    }

    *out_info = *selected;
    ESP_LOGI(TAG, "selected=%uS asset=%s softid=%s", out_info->cell_count,
             out_info->global_asset_number, out_info->soft_id);
    return ESP_OK;
}
