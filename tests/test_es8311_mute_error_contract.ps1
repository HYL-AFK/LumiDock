$ErrorActionPreference = 'Stop'

$codecDev = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\managed_components\espressif__esp_codec_dev\esp_codec_dev.c') -Raw
$es8311 = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\managed_components\espressif__esp_codec_dev\device\es8311\es8311.c') -Raw
$driver = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\main\Dri\i2s_audio_driver.c') -Raw

$muteBody = [regex]::Match($es8311, 'static int es8311_set_mute\(.*?\n}\n', 'Singleline').Value
if ($muteBody -notmatch 'if \(ret != ESP_CODEC_DEV_OK\) \{\s*return ret;\s*\}') {
    throw 'ES8311 mute must stop when the DAC mute-register read fails.'
}
if ($muteBody -notmatch 'ret = es8311_write_reg\(codec, ES8311_DAC_REG31, (?s:.*?)\);(?s:.*?)return ret;') {
    throw 'ES8311 mute must return the actual DAC mute-register write result.'
}

$deviceMuteBody = [regex]::Match($codecDev, 'int esp_codec_dev_set_out_mute\(.*?\n}\n', 'Singleline').Value
if ($deviceMuteBody -notmatch 'ret = codec->mute\(codec, mute\);\s*if \(ret != ESP_CODEC_DEV_OK\) \{\s*return ret;\s*\}\s*dev->muted = mute;') {
    throw 'Codec-device mute state must change only after the codec mute call succeeds.'
}
if ($deviceMuteBody -match 'dev->muted = mute;\s*if \(codec && codec->mute\)') {
    throw 'Codec-device mute state must not be set before the hardware result is known.'
}

$driverMuteBody = [regex]::Match($driver, 'esp_err_t i2s_audio_driver_set_output_muted\(.*?\n}\n', 'Singleline').Value
$retryBody = [regex]::Match($driver, 'static esp_err_t set_codec_output_mute_with_retry\(.*?\n}\n', 'Singleline').Value
if ($retryBody -notmatch 'ES8311_MUTE_RETRY_COUNT' -or
    $retryBody -notmatch 'for \(uint32_t attempt = 1U; attempt <= ES8311_MUTE_RETRY_COUNT; \+\+attempt\)') {
    throw 'ES8311 output mute must retry transient I2C failures a bounded number of times.'
}
if ($driverMuteBody -notmatch 'set_codec_output_mute_with_retry\(muted\)') {
    throw 'Normal playback mute changes must use the same bounded retry helper.'
}
if ($driverMuteBody -notmatch 's_output_muted = muted;\s*ESP_LOGI\(TAG, "ES8311 output %s"') {
    throw 'Driver mute state must only update after a successful hardware mute operation.'
}
if ($driver -notmatch 'static esp_err_t set_codec_output_mute_with_retry\(bool muted\)') {
    throw 'ES8311 startup mute must share the bounded retry helper.'
}
$initBody = [regex]::Match($driver, 'esp_err_t i2s_audio_driver_init\(void\).*?\n}\n\nbool i2s_audio_driver_is_ready', 'Singleline').Value
if ($initBody -notmatch 'set_codec_output_mute_with_retry\(true\)') {
    throw 'ES8311 initial DAC mute must retry transient I2C failures before startup fails.'
}

Write-Output 'ES8311 mute error propagation contract passed.'
