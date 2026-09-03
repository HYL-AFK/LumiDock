$ErrorActionPreference = 'Stop'

$driver = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\i2s_audio_driver.c"
$header = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\i2s_audio_driver.h"
$pins = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\board_pins.h"
$manifest = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\idf_component.yml"
$manager = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_manager.c"

@(
    @{ Content = $pins; Pattern = '#define LUMIDOCK_I2S_BCLK_GPIO GPIO_NUM_10'; Message = 'ES8311 SCLK must use GPIO10.' },
    @{ Content = $pins; Pattern = '#define LUMIDOCK_I2S_LRCLK_GPIO GPIO_NUM_9'; Message = 'ES8311 LRCK must use GPIO9.' },
    @{ Content = $pins; Pattern = '#define LUMIDOCK_I2S_AMP_DIN_GPIO GPIO_NUM_7'; Message = 'ES8311 DIN must use GPIO7.' },
    @{ Content = $pins; Pattern = '#define LUMIDOCK_I2S_MIC_DATA_GPIO GPIO_NUM_8'; Message = 'ES8311 DOUT must use GPIO8.' },
    @{ Content = $pins; Pattern = '#define LUMIDOCK_I2S_MCLK_GPIO GPIO_NUM_38'; Message = 'ES8311 MCLK must use GPIO38.' },
    @{ Content = $driver; Pattern = '#include "driver/i2c_master.h"'; Message = 'The ES8311 driver must configure its I2C control bus.' },
    @{ Content = $driver; Pattern = '#include "driver/i2s_std.h"'; Message = 'The ES8311 driver must configure its I2S data bus.' },
    @{ Content = $header; Pattern = '#include <stdbool.h>'; Message = 'The public audio-driver header must define bool consistently for every C translation unit.' },
    @{ Content = $header; Pattern = '#define LUMIDOCK_AUDIO_SAMPLE_RATE_HZ 16000U'; Message = 'The initial voice PCM stream must define a 16 kHz sample rate.' },
    @{ Content = $driver; Pattern = 'I2S_STD_CLK_DEFAULT_CONFIG\(LUMIDOCK_AUDIO_SAMPLE_RATE_HZ\)'; Message = 'The I2S stream must use the defined 16 kHz PCM sample rate.' },
    @{ Content = $header; Pattern = '#define LUMIDOCK_AUDIO_BITS_PER_SAMPLE 16U'; Message = 'The initial voice PCM stream must define 16-bit samples.' },
    @{ Content = $header; Pattern = '#define LUMIDOCK_AUDIO_CHANNELS 1U'; Message = 'The initial voice PCM stream must define one channel.' },
    @{ Content = $driver; Pattern = 'I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG\(\s*LUMIDOCK_AUDIO_BITS_PER_SAMPLE, I2S_SLOT_MODE_MONO\)'; Message = 'The initial voice PCM stream must use 16-bit mono I2S.' },
    @{ Content = $driver; Pattern = '\.mclk = LUMIDOCK_I2S_MCLK_GPIO'; Message = 'The codec driver must output MCLK on GPIO38.' },
    @{ Content = $driver; Pattern = '\.dout = LUMIDOCK_I2S_AMP_DIN_GPIO'; Message = 'ESP32 I2S output must feed ES8311 DIN.' },
    @{ Content = $driver; Pattern = '\.din = LUMIDOCK_I2S_MIC_DATA_GPIO'; Message = 'ESP32 I2S input must read ES8311 DOUT.' },
    @{ Content = $driver; Pattern = '#define ES8311_I2C_ADDRESS_7BIT \(ES8311_CODEC_DEFAULT_ADDR >> 1U\)'; Message = 'The driver must derive the 7-bit probe address from the codec wire address.' },
    @{ Content = $driver; Pattern = 'i2c_master_probe\(s_i2c_bus, ES8311_I2C_ADDRESS_7BIT'; Message = 'The driver must probe the ES8311 at its 7-bit I2C address.' },
    @{ Content = $driver; Pattern = '\.addr = s_codec_address'; Message = 'The codec control interface must use the default or detected I2C address.' },
    @{ Content = $driver; Pattern = '\.pa_pin = GPIO_NUM_NC'; Message = 'This module has no ESP-controlled NS4150B CTRL pin; PA GPIO must stay unused.' },
    @{ Content = $driver; Pattern = '\.use_mclk = true'; Message = 'The ES8311 driver must use the provided MCLK reference.' },
    @{ Content = $driver; Pattern = '\.digital_mic = false'; Message = 'The ZTS6216 board microphone is wired to ES8311 MIC1P and must select the analog input path.' },
    @{ Content = $driver; Pattern = '#define ES8311_MIC_INPUT_GAIN_DB 24\.0f'; Message = 'The ES8311 microphone must use the 24 dB input gain for far-field speech pickup.' },
    @{ Content = $driver; Pattern = '#define ES8311_MIC_GAIN_REG16_VALUE 0x24'; Message = 'The driver must write the open-time default ADC_REG16 pattern 0x24 (0x28 silences the ADC).' },
    @{ Content = $driver; Pattern = 'esp_codec_dev_write_reg\(s_codec, ES8311_ADC_REG16, ES8311_MIC_GAIN_REG16_VALUE\)'; Message = 'The correct mic gain register pattern must be written directly after the codec is open.' },
    @{ Content = $driver; Pattern = 'esp_codec_dev_set_in_gain\(s_codec, ES8311_MIC_INPUT_GAIN_DB\)'; Message = 'The codec must apply the named calibrated microphone gain.' },
    @{ Content = $driver; Pattern = 'i2c_master_probe\(s_i2c_bus, ES8311_I2C_ADDRESS_7BIT'; Message = 'The driver must probe ES8311 before attempting codec register initialization.' },
    @{ Content = $driver; Pattern = 'scan_i2c_addresses'; Message = 'When ES8311 does not acknowledge at its default address, the driver must scan the I2C bus for diagnostic evidence.' },
    @{ Content = $driver; Pattern = 'I2C device found: 0x%02x'; Message = 'The I2C address scan must log every responding 7-bit address.' },
    @{ Content = $driver; Pattern = 's_codec_address = \(uint8_t\)\(candidate_address << 1U\)'; Message = 'The 7-bit scan result must be converted to the codec controller wire address.' },
    @{ Content = $driver; Pattern = 'ES8311 candidate detected at 7-bit I2C 0x%02x'; Message = 'A responding codec candidate at a non-default address must be selected for initialization.' },
    @{ Content = $driver; Pattern = '\.addr = s_codec_address'; Message = 'The codec control interface must use the detected I2C address.' },
    @{ Content = $driver; Pattern = 'No I2C devices responded on GPIO17/18'; Message = 'The diagnostic must clearly identify an empty I2C bus.' },
    @{ Content = $driver; Pattern = 'I2C timeout on GPIO17/18; address scan skipped'; Message = 'An electrically stalled I2C bus must not trigger a noisy full address scan.' },
    @{ Content = $driver; Pattern = 'esp_codec_dev_open'; Message = 'The codec must be opened for PCM capture and playback.' },
    @{ Content = $driver; Pattern = '(?m)^\s*audio_codec_i2s_cfg_t i2s_cfg'; Message = 'The codec I2S configuration must be writable for esp_codec_dev.' },
    @{ Content = $driver; Pattern = '(?m)^\s*audio_codec_i2c_cfg_t i2c_cfg'; Message = 'The codec I2C configuration must be writable for esp_codec_dev.' },
    @{ Content = $driver; Pattern = '(?m)^\s*esp_codec_dev_cfg_t device_cfg'; Message = 'The codec device configuration must be writable for esp_codec_dev.' },
    @{ Content = $driver; Pattern = '(?m)^\s*esp_codec_dev_sample_info_t pcm_format'; Message = 'The PCM format configuration must be writable for esp_codec_dev.' },
    @{ Content = $header; Pattern = 'i2s_audio_driver_record'; Message = 'The driver must expose a PCM record API.' },
    @{ Content = $header; Pattern = 'i2s_audio_driver_play'; Message = 'The driver must expose a PCM playback API.' },
    @{ Content = $manifest; Pattern = 'espressif/esp_codec_dev'; Message = 'The project must declare the reference codec dependency.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

if ($driver -match '\.pa_pin = LUMIDOCK_I2S_AMP_DIN_GPIO|\.pa_pin = GPIO_NUM_7') {
    throw 'GPIO7 is ES8311 DIN and must never be configured as a PA enable output.'
}

if ($manager -notmatch 'audio_startup_task' -or
    $manager -notmatch 'snapshot\.voice_activation\.activated' -or
    $manager -notmatch '!snapshot\.voice_activation\.restart_required' -or
    $manager -notmatch 'activation confirmed.*starting deferred audio services' -or
    $manager -notmatch 'ES8311 audio init: %s; retry in %u ms') {
    throw 'The pre-reserved optional ES8311 startup must wait for confirmed XiaoZhi activation and remain non-blocking.'
}
if ($manager -match 'voice_activation_gate_task') {
    throw 'A late activation-gate task must not allocate audio startup after network/UI initialization.'
}

if ($driver -notmatch 'if \(s_i2c_bus != NULL\) return ESP_OK;' -or
    $driver -notmatch 'i2s_del_channel' -or
    $driver -notmatch 'esp_codec_dev_delete') {
    throw 'Audio initialization retries must reuse the I2C bus and release failed I2S/codec state.'
}
if ($manager -notmatch 'AUDIO_RETRY_DELAY_MS 3000U' -or
    $manager -notmatch 'vTaskDelay\(pdMS_TO_TICKS\(AUDIO_RETRY_DELAY_MS\)\)') {
    throw 'ES8311 startup failures must retry in the background every 3 seconds.'
}
if ($manager -notmatch 'run_microphone_self_test' -or
    $manager.IndexOf('run_microphone_self_test') -gt $manager.IndexOf('xiaozhi_voice_service_init')) {
    throw 'A microphone self-test must run after codec setup and before the voice pipeline starts.'
}
if ($driver -notmatch 'set_codec_output_mute_with_retry\(true\)' -or
    $driver -notmatch 'i2s_audio_driver_start_output\s*\(' -or
    $driver -notmatch 'i2s_audio_driver_stop_output\s*\(') {
    throw 'The ES8311 output must remain muted while idle and expose explicit PCM playback transitions.'
}
if ($driver -notmatch 'i2s_audio_driver_start_output[\s\S]{0,1400}set_codec_output_mute_with_retry\(false\)') {
    throw 'The explicit playback-start API must be the only path that unmutes the DAC.'
}

Write-Output 'ES8311 audio-driver contract passed.'

