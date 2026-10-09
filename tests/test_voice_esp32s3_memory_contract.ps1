$ErrorActionPreference = 'Stop'

$defaults = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\sdkconfig.defaults"
$sdkconfig = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\sdkconfig"
$audio = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

foreach ($config in @($defaults, $sdkconfig)) {
    if ($config -notmatch '(?m)^CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y\s*$' -or
        $config -notmatch '(?m)^CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA=y\s*$') {
        throw 'ESP32-S3 voice builds must use the external, dynamically released TLS allocator.'
    }
    if ($config -notmatch '(?m)^CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048\s*$') {
        throw 'ESP32-S3 voice builds must route ordinary allocations to PSRAM above 2 KiB.'
    }
    if ($config -notmatch '(?m)^CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=98304\s*$') {
        throw 'ESP32-S3 voice builds must reserve 96 KiB of internal RAM for DMA, FreeRTOS, WSS, and ESP-SR.'
    }
    if ($config -notmatch '(?m)^CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=3\s*$' -or
        $config -notmatch '(?m)^CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=6\s*$' -or
        $config -notmatch '(?m)^CONFIG_ESP_WIFI_RX_BA_WIN=4\s*$' -or
        $config -notmatch '(?m)^CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=8\s*$' -or
        $config -notmatch '(?m)^CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=16\s*$') {
        throw 'ESP32-S3 voice builds must use the constrained-memory Wi-Fi/LWIP baseline.'
    }
}

if ($voice -notmatch '\.task_stack = 4096' -or
    $voice -notmatch 'XIAOZHI_PIPELINE_INIT_STACK_BYTES 4096U' -or
    $voice -notmatch 'xTaskCreateStaticPinnedToCore\(') {
    throw 'WSS and ESP-SR model initialization must retain their native internal-RAM task stacks.'
}

foreach ($token in @(
    'ESP-SR init failure: model list unavailable',
    'ESP-SR init failure: AFE config unavailable',
    'ESP-SR init failure: WakeNet model unavailable',
    'ESP-SR init failure: AFE interface unavailable',
    'ESP-SR init failure: AFE instance unavailable',
    'afe_config_free\(config\)')) {
    if ($audio -notmatch $token) {
        throw "Audio pipeline must diagnose and clean up the ESP-SR failure boundary: missing $token"
    }
}

Write-Output 'ESP32-S3 voice memory and ESP-SR failure-boundary contract passed.'
