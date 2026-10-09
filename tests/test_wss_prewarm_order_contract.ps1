$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

$initStart = $voice.IndexOf('esp_err_t xiaozhi_voice_service_init(void)')
if ($initStart -lt 0) { throw 'Voice service initialization entry point not found.' }
$init = $voice.Substring($initStart)
$prewarm = $init.IndexOf('prewarm_wss();')
$afe = $init.IndexOf('init_audio_pipeline_on_internal_stack()')

if ($prewarm -lt 0 -or $afe -lt 0 -or $prewarm -ge $afe) {
    throw 'WSS prewarm must run before AFE/WakeNet allocation reserves the final internal 4 KiB task block.'
}

# 锁的是"预热的 client 在初始化失败路径上也必须被 stop+destroy，不能泄漏"，
# 不是 `{` 和大括号与语句的相对排版。原断言要求三者全在同一行，源码改成换行
# 写法后它就一直红着——红线一旦长期存在，真正的回归就再也看不见了。
if ($voice -notmatch '(?s)if \(s_client != NULL\)\s*\{\s*if \(s_client_started\)\s*\(void\)esp_websocket_client_stop\(s_client\);\s*\(void\)esp_websocket_client_destroy\(s_client\);') {
    throw 'Initialization failure cleanup must destroy an early-prewarmed WSS client.'
}

Write-Output 'WSS prewarm order contract passed.'
