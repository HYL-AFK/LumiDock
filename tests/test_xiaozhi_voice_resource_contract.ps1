$ErrorActionPreference = 'Stop'

$sdkconfig = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\sdkconfig"
$defaults = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\sdkconfig.defaults"
$partitions = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\partitions.csv"
$manifest = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\idf_component.yml"
$cmake = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\CMakeLists.txt"
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"
$ui = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"
$manager = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_manager.c"

@(
    @{ Content = $sdkconfig; Pattern = 'CONFIG_SPIRAM=y'; Message = 'Voice runtime needs PSRAM enabled in the active sdkconfig.' },
    @{ Content = $sdkconfig; Pattern = 'CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240=y'; Message = 'Voice runtime must use the confirmed 240 MHz ESP32-S3 clock.' },
    @{ Content = $sdkconfig; Pattern = 'CONFIG_MODEL_IN_FLASH=y'; Message = 'WakeNet models must reside in Flash.' },
    @{ Content = $sdkconfig; Pattern = 'CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y'; Message = 'The supplied "ni hao xiao zhi" WakeNet model must be enabled.' },
    @{ Content = $sdkconfig; Pattern = '# CONFIG_SR_WN_WN9_XIAOAITONGXUE is not set'; Message = 'The incompatible "xiao ai tong xue" WakeNet model must be disabled.' },
    @{ Content = $sdkconfig; Pattern = 'CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y'; Message = 'Wi-Fi and LwIP dynamic allocations must prefer PSRAM to preserve internal AFE memory.' },
    @{ Content = $sdkconfig; Pattern = 'CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y'; Message = 'NimBLE dynamic allocations must use PSRAM to preserve contiguous internal AFE memory.' },
    @{ Content = $defaults; Pattern = 'CONFIG_SPIRAM=y'; Message = 'PSRAM must remain enabled after a clean configure.' },
    @{ Content = $defaults; Pattern = 'CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y'; Message = 'Wi-Fi/LwIP PSRAM preference must remain enabled after a clean configure.' },
    @{ Content = $defaults; Pattern = 'CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y'; Message = 'NimBLE PSRAM allocation must remain enabled after a clean configure.' },
    @{ Content = $defaults; Pattern = 'CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y'; Message = '240 MHz must remain selected after a clean configure.' },
    @{ Content = $partitions; Pattern = '(?m)^factory,\s+app,\s+factory,\s+0x10000,\s+0x770000,'; Message = 'The 8 MB application partition must reserve Flash for the voice model.' },
    @{ Content = $partitions; Pattern = '(?m)^model,\s+data,\s+spiffs,\s*,\s*0x80000,'; Message = 'A valid 512 KiB Flash model partition is required for WakeNet.' },
    @{ Content = $manifest; Pattern = 'espressif/esp-sr'; Message = 'ESP-SR must be declared for WakeNet and VAD.' },
    @{ Content = $cmake; Pattern = 'MID/voice/xiaozhi_voice_service.c'; Message = 'The XiaoZhi WSS service must be compiled.' },
    @{ Content = $cmake; Pattern = 'MID/voice/xiaozhi_audio_pipeline.c'; Message = 'The XiaoZhi audio pipeline must be compiled.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

$beginStart = $voice.IndexOf('static esp_err_t start_wss(bool foreground_session)')
$beginEnd = $voice.IndexOf('static void prewarm_wss(void)', $beginStart)
$initStart = $voice.IndexOf('esp_err_t xiaozhi_voice_service_init(void)')
$initEnd = $voice.IndexOf('void xiaozhi_voice_service_network_lost', $initStart)
$beginBlock = $voice.Substring($beginStart, $beginEnd - $beginStart)
$initBlock = $voice.Substring($initStart, $initEnd - $initStart)
$managerStart = $manager.IndexOf('esp_err_t app_manager_start(void)')
$audioStartupIndex = $manager.IndexOf('xTaskCreatePinnedToCore(audio_startup_task', $managerStart)
$wifiInitIndex = $manager.IndexOf('wifi_service_init()')
$clientInit = $beginBlock.IndexOf('s_client = esp_websocket_client_init(&config)')
$eventRegister = $beginBlock.IndexOf('esp_websocket_register_events(s_client')
if ($clientInit -lt 0 -or $eventRegister -lt 0 -or $eventRegister -lt $clientInit -or
    $initBlock -match 'esp_websocket_register_events') {
    throw 'WSS events must be registered only after a client handle has been created.'
}

$eventDeclaration = $voice.IndexOf('static void websocket_event(void *handler_args')
if ($eventDeclaration -lt 0 -or $eventDeclaration -gt $beginStart) {
    throw 'The WSS event callback must be declared before begin_channel uses it.'
}

if ($managerStart -lt 0 -or $audioStartupIndex -lt 0 -or $wifiInitIndex -lt 0 -or
    $audioStartupIndex -ge $wifiInitIndex) {
    throw 'The internal audio-startup stack must be reserved before network allocations fragment RAM.'
}
$audioTaskStart = $manager.IndexOf('static void audio_startup_task(void *arg)')
$audioTaskEnd = $manager.IndexOf('esp_err_t app_manager_start(void)', $audioTaskStart)
if ($audioTaskStart -lt 0 -or $audioTaskEnd -lt 0) {
    throw 'The pre-reserved audio startup task must exist.'
}
$audioTask = $manager.Substring($audioTaskStart, $audioTaskEnd - $audioTaskStart)
if ($audioTask -notmatch 'snapshot\.voice_activation\.activated' -or
    $audioTask -notmatch '!snapshot\.voice_activation\.restart_required') {
    throw 'The pre-reserved task must wait for confirmed XiaoZhi activation before starting audio services.'
}

if ($sdkconfig -match '(?m)^CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y\r?$' -or
    $sdkconfig -match '(?m)^CONFIG_ESP32S3_DEFAULT_CPU_FREQ_160=y\r?$') {
    throw 'The active sdkconfig must not select both 160 MHz and 240 MHz CPU modes.'
}

@(
    @{ Content = $pipeline; Pattern = 'config->afe_ringbuf_size = XIAOZHI_AFE_RINGBUF_FRAMES;'; Message = 'AFE must use the confirmed bounded PSRAM ring-buffer configuration.' },
    @{ Content = $pipeline; Pattern = 'afe_config_init\("M", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF\)'; Message = 'The lightweight AFE must use the single-microphone speech-recognition input format.' },
    @{ Content = $pipeline; Pattern = 'config->aec_init = false;'; Message = 'AEC must remain disabled on the 2 MiB PSRAM target.' },
    @{ Content = $pipeline; Pattern = 'config->ns_init = false;'; Message = 'Noise suppression must remain disabled on the 2 MiB PSRAM target.' },
    @{ Content = $pipeline; Pattern = 's_pipeline\.afe->feed\(s_pipeline\.afe_data, microphone\);'; Message = 'The AFE feed must provide only microphone PCM after AEC reference removal.' },
    @{ Content = $pipeline; Pattern = 'REFERENCE_QUEUE_BYTES'; Message = 'The AEC playback-reference queue must be removed.'; Negate = $true },
    @{ Content = $pipeline; Pattern = 'esp_opus_enc_get_frame_size\(s_pipeline\.encoder, &pcm_bytes, &opus_bytes\)'; Message = 'A direct Opus encoder handle must use the matching direct Opus frame-size API.' },
    @{ Content = $pipeline; Pattern = 'esp_audio_enc_get_frame_size\('; Message = 'The generic encoder API must not receive a direct Opus encoder handle.'; Negate = $true },
    @{ Content = $pipeline; Pattern = 'config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;'; Message = 'AFE buffers must preferentially use PSRAM.' },
    @{ Content = $pipeline; Pattern = 'audio_feed_task,\s+"voice_feed",\s+6144,\s+NULL,\s+5,\s+(?:NULL|&s_pipeline\.feed_task),\s+0,\s+MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT'; Message = 'I2S/AFE feed must use a PSRAM stack on core 0 at realtime priority.' },
    @{ Content = $pipeline; Pattern = 'audio_detect_task,\s+"voice_detect",\s+6144,\s+NULL,\s+5,\s+(?:NULL|&s_pipeline\.detect_task),\s+1,\s+MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT'; Message = 'WakeNet/VAD fetch must use a PSRAM stack on core 1.' },
    @{ Content = $pipeline; Pattern = 'voice_codec_task,\s+"voice_codec",\s+VOICE_CODEC_STACK_BYTES,\s+NULL,\s+4,\s+&s_pipeline\.codec_task,\s+0,\s+MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT'; Message = 'Opus encoding and decoding must use one PSRAM codec stack on core 0.' },
    @{ Content = $ui; Pattern = 'xTaskCreatePinnedToCore\(ui_task, "ui_service", 8192, NULL, 2, &s_task, 1\)'; Message = 'UI task must stay on core 1 below voice processing priority.' },
    @{ Content = $voice; Pattern = 'voice_session_task,\s+"voice_session",\s+6144,\s+NULL,\s+3,\s+&s_session_task,\s+0,\s+MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT'; Message = 'The XiaoZhi session task must use a PSRAM stack.' },
    @{ Content = $manager; Pattern = 'xTaskCreatePinnedToCore\(audio_startup_task, "audio_startup", 4096, NULL, 2,\s+NULL, 0\)'; Message = 'The audio startup task must retain its confirmed internal stack while ESP-SR loads Flash models.' },
    @{ Content = $manager; Pattern = 'xTaskCreatePinnedToCoreWithCaps\(snapshot_bridge_task, "snapshot_bridge", 3072, NULL, 2, NULL, 1,\s+MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT\)'; Message = 'Service polling must stay on core 1 below voice processing priority.' }
) | ForEach-Object {
    $matched = $_.Content -match $_.Pattern
    if (($_.Negate -and $matched) -or (-not $_.Negate -and -not $matched)) { throw $_.Message }
}

Write-Output 'XiaoZhi voice resource contract passed.'

