$ErrorActionPreference = 'Stop'

# 电台调试通道（GATT 0xFFE0/0xFFE1）的约束。
#
# 这条通道让手机蓝牙调试助手不握手、不组帧就能直接发命令切台。它比配网路径低一层
# 门槛，所以更容易被无意改坏：有人为了省事直接在这里拼个 URL 下发给电台，或者把
# 名字从广播包挪回 scan response（那样手机扫描列表里就只看得到一串地址）。这几条
# 都钉住。

$root = "$PSScriptRoot\..\main"
$ble = Get-Content -Raw -LiteralPath "$root\MID\network\ble_provision.c"

# 1. UUID 必须是 0xFFE0/0xFFE1：手机调试助手多把这对当预设条目，换成 128 位的
#    自定义 UUID 就得手工填，等于把"随手能用"这件事弄丢。
if ($ble -notmatch '#define DEBUG_SVC_UUID 0xFFE0') {
    throw 'Debug service must use 0xFFE0 (the HM-10/JDY convention).'
}
if ($ble -notmatch '#define DEBUG_CMD_UUID 0xFFE1') {
    throw 'Debug command characteristic must use 0xFFE1.'
}
if ($ble -notmatch 'ble_gatts_add_svcs\(s_debug_svcs\)') {
    throw 'ble_provision.c must actually register the debug service.'
}

# 2. 命令只能转发到 radio_stream_service 的公开入口。电台频道表写死在
#    radio_stream_service.c 里，外部一律不许点名或下发地址。
if ($ble -notmatch 'radio_stream_service_play_next\(\)') {
    throw 'Debug channel must switch stations through radio_stream_service_play_next().'
}
if ($ble -match 'https?://') {
    throw 'Debug channel must not carry URLs; the station list is compiled in.'
}
if ($ble -match 'esp_http_client|esp_wifi_set_config|nvs_set_') {
    throw 'Debug channel must not reach into network or storage directly.'
}

# 3. 广播名必须进广播包本体。只放 scan response 的话，被动扫描的一方（Windows
#    蓝牙栈、部分手机调试助手）永远拿不到名字，扫描列表里只剩地址。
if ($ble -notmatch 'adv_fields\.name = \(uint8_t \*\)s_adv_name') {
    throw 'The advertising name must be carried in the advertisement packet, not only in the scan response.'
}
if ($ble -notmatch 'adv_fields\.name_is_complete = 1') {
    throw 'The advertisement name must be marked complete so scanners do not expect truncation.'
}

# 4. 调试通道不能让配网跟着起不来：注册失败只能降级，不能 return 掉整个 BLE 初始化。
if ($ble -match 'return ESP_FAIL;\s*\r?\n\s*\}\s*\r?\n\s*\(void\)register_debug_gatt') {
    throw 'A debug-service failure must not abort BLE bring-up; BluFi is required at boot.'
}

Write-Output 'ble radio debug contract OK'
