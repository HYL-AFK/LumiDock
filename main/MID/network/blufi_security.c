#include "blufi_security.h"

#include <stdlib.h>
#include <string.h>

#include "app_log.h"
#include "esp_blufi_api.h"
#include "esp_crc.h"
#include "esp_random.h"
#include "mbedtls/aes.h"
#include "mbedtls/dhm.h"
#include "mbedtls/md5.h"

static const char *TAG = "blufi_sec";

#define SEC_TYPE_DH_PARAM_LEN   0x00
#define SEC_TYPE_DH_PARAM_DATA  0x01
#define SEC_TYPE_DH_P           0x02
#define SEC_TYPE_DH_G           0x03
#define SEC_TYPE_DH_PUBLIC      0x04

struct blufi_security_ctx {
#define DH_SELF_PUB_KEY_LEN 128
    uint8_t self_public_key[DH_SELF_PUB_KEY_LEN];
#define SHARE_KEY_LEN 128
    uint8_t share_key[SHARE_KEY_LEN];
    size_t share_len;
#define PSK_LEN 16
    uint8_t psk[PSK_LEN];
    uint8_t *dh_param;
    int dh_param_len;
    uint8_t iv[16];
    mbedtls_dhm_context dhm;
    mbedtls_aes_context aes;
};

static struct blufi_security_ctx *s_blufi_sec;

extern void btc_blufi_report_error(esp_blufi_error_state_t state);

static int blufi_random(void *rng_state, unsigned char *output, size_t len)
{
    (void)rng_state;
    esp_fill_random(output, len);
    return 0;
}

void blufi_dh_negotiate_data_handler(uint8_t *data,
                                     int len,
                                     uint8_t **output_data,
                                     int *output_len,
                                     bool *need_free)
{
    if (data == NULL || len < 3 || output_data == NULL || output_len == NULL || need_free == NULL) {
        btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
        return;
    }

    if (s_blufi_sec == NULL) {
        APP_LOGE(TAG, "security not initialized");
        btc_blufi_report_error(ESP_BLUFI_INIT_SECURITY_ERROR);
        return;
    }

    int ret;
    uint8_t type = data[0];

    switch (type) {
    case SEC_TYPE_DH_PARAM_LEN:
        s_blufi_sec->dh_param_len = ((data[1] << 8) | data[2]);
        free(s_blufi_sec->dh_param);
        s_blufi_sec->dh_param = NULL;
        s_blufi_sec->dh_param = malloc((size_t)s_blufi_sec->dh_param_len);
        if (s_blufi_sec->dh_param == NULL) {
            s_blufi_sec->dh_param_len = 0;
            btc_blufi_report_error(ESP_BLUFI_DH_MALLOC_ERROR);
            APP_LOGE(TAG, "malloc dh_param failed");
        }
        break;

    case SEC_TYPE_DH_PARAM_DATA: {
        if (s_blufi_sec->dh_param == NULL || len < (s_blufi_sec->dh_param_len + 1)) {
            btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
            APP_LOGE(TAG, "invalid dh param");
            return;
        }

        memcpy(s_blufi_sec->dh_param, &data[1], (size_t)s_blufi_sec->dh_param_len);
        uint8_t *param = s_blufi_sec->dh_param;
        ret = mbedtls_dhm_read_params(&s_blufi_sec->dhm,
                                      &param,
                                      &param[s_blufi_sec->dh_param_len]);
        if (ret != 0) {
            APP_LOGE(TAG, "read params failed: %d", ret);
            btc_blufi_report_error(ESP_BLUFI_READ_PARAM_ERROR);
            return;
        }

        free(s_blufi_sec->dh_param);
        s_blufi_sec->dh_param = NULL;

        const int dhm_len = mbedtls_dhm_get_len(&s_blufi_sec->dhm);
        if (dhm_len > DH_SELF_PUB_KEY_LEN) {
            APP_LOGE(TAG, "dhm len unsupported: %d", dhm_len);
            btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
            return;
        }

        ret = mbedtls_dhm_make_public(&s_blufi_sec->dhm,
                                      dhm_len,
                                      s_blufi_sec->self_public_key,
                                      DH_SELF_PUB_KEY_LEN,
                                      blufi_random,
                                      NULL);
        if (ret != 0) {
            APP_LOGE(TAG, "make public failed: %d", ret);
            btc_blufi_report_error(ESP_BLUFI_MAKE_PUBLIC_ERROR);
            return;
        }

        ret = mbedtls_dhm_calc_secret(&s_blufi_sec->dhm,
                                      s_blufi_sec->share_key,
                                      SHARE_KEY_LEN,
                                      &s_blufi_sec->share_len,
                                      blufi_random,
                                      NULL);
        if (ret != 0) {
            APP_LOGE(TAG, "calc secret failed: %d", ret);
            btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
            return;
        }

        ret = mbedtls_md5(s_blufi_sec->share_key, s_blufi_sec->share_len, s_blufi_sec->psk);
        if (ret != 0) {
            APP_LOGE(TAG, "md5 failed: %d", ret);
            btc_blufi_report_error(ESP_BLUFI_CALC_MD5_ERROR);
            return;
        }

        mbedtls_aes_setkey_enc(&s_blufi_sec->aes, s_blufi_sec->psk, PSK_LEN * 8);
        *output_data = s_blufi_sec->self_public_key;
        *output_len = dhm_len;
        *need_free = false;
        break;
    }

    case SEC_TYPE_DH_P:
    case SEC_TYPE_DH_G:
    case SEC_TYPE_DH_PUBLIC:
    default:
        break;
    }
}

int blufi_aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len)
{
    if (s_blufi_sec == NULL || crypt_data == NULL || crypt_len < 0) {
        return -1;
    }

    size_t iv_offset = 0;
    uint8_t iv0[16];
    memcpy(iv0, s_blufi_sec->iv, sizeof(iv0));
    iv0[0] = iv8;

    int ret = mbedtls_aes_crypt_cfb128(&s_blufi_sec->aes,
                                       MBEDTLS_AES_ENCRYPT,
                                       (size_t)crypt_len,
                                       &iv_offset,
                                       iv0,
                                       crypt_data,
                                       crypt_data);
    return ret == 0 ? crypt_len : -1;
}

int blufi_aes_decrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len)
{
    if (s_blufi_sec == NULL || crypt_data == NULL || crypt_len < 0) {
        return -1;
    }

    size_t iv_offset = 0;
    uint8_t iv0[16];
    memcpy(iv0, s_blufi_sec->iv, sizeof(iv0));
    iv0[0] = iv8;

    int ret = mbedtls_aes_crypt_cfb128(&s_blufi_sec->aes,
                                       MBEDTLS_AES_DECRYPT,
                                       (size_t)crypt_len,
                                       &iv_offset,
                                       iv0,
                                       crypt_data,
                                       crypt_data);
    return ret == 0 ? crypt_len : -1;
}

uint16_t blufi_crc_checksum(uint8_t iv8, uint8_t *data, int len)
{
    (void)iv8;
    return esp_crc16_be(0, data, (size_t)len);
}

esp_err_t blufi_security_init(void)
{
    if (s_blufi_sec != NULL) {
        return ESP_OK;
    }

    s_blufi_sec = calloc(1, sizeof(*s_blufi_sec));
    if (s_blufi_sec == NULL) {
        return ESP_ERR_NO_MEM;
    }

    mbedtls_dhm_init(&s_blufi_sec->dhm);
    mbedtls_aes_init(&s_blufi_sec->aes);
    memset(s_blufi_sec->iv, 0, sizeof(s_blufi_sec->iv));
    return ESP_OK;
}

void blufi_security_deinit(void)
{
    if (s_blufi_sec == NULL) {
        return;
    }

    free(s_blufi_sec->dh_param);
    s_blufi_sec->dh_param = NULL;
    mbedtls_dhm_free(&s_blufi_sec->dhm);
    mbedtls_aes_free(&s_blufi_sec->aes);
    memset(s_blufi_sec, 0, sizeof(*s_blufi_sec));
    free(s_blufi_sec);
    s_blufi_sec = NULL;
}
