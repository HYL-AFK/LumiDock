$ErrorActionPreference = 'Stop'

# 蓝牙广播名和 SoftAP 名必须是同一个字符串，都是 "ESP32S3_" 加 STA MAC 的后六位。
#
# 这条约束很容易被无意破坏：两边分属 ble_provision.c 和 wifi_manager.c，各改各的
# 不会报错，只有把手机同时打开扫一遍才看得出来。所以在这里钉死三件事——同一个前缀、
# 同一个 MAC 来源、不再有第二处生成名字的地方。

$root = "$PSScriptRoot\..\main"
# 前缀宏在 .h 里，格式化在 .c 里，两个文件合起来才是完整的约定。
$identity = (Get-Content -Raw -LiteralPath "$root\COM\device_identity.h") + "`n" +
    (Get-Content -Raw -LiteralPath "$root\COM\device_identity.c")
$ble = Get-Content -Raw -LiteralPath "$root\MID\network\ble_provision.c"
$wifi = Get-Content -Raw -LiteralPath "$root\Dri\wifi_manager.c"
$cfg = Get-Content -Raw -LiteralPath "$root\COM\app_config.c"

# 1. 名字只能由 device_identity 生成，且格式固定为 前缀_六位十六进制。
if ($identity -notmatch '"%s_%02X%02X%02X"') {
    throw 'device_identity.c must format the name as PREFIX_XXXXXX (underscore, six hex digits).'
}
if ($identity -notmatch 'ESP32S3') {
    throw 'device_identity.c must keep the ESP32S3 prefix that the product uses.'
}

# 2. MAC 必须取 STA。取 BT 或 AP 会让后缀和另一侧差两位——这正是这次要消灭的混乱。
#    只匹配实际调用，不匹配注释——这些注释正是为了讲清为什么不用另外两个 MAC。
if ($identity -notmatch 'esp_read_mac\(mac, ESP_MAC_WIFI_STA\)') {
    throw 'device_identity.c must derive the suffix from ESP_MAC_WIFI_STA.'
}
if ($identity -match 'esp_read_mac\(mac, ESP_MAC_BT\)|esp_read_mac\(mac, ESP_MAC_WIFI_SOFTAP\)') {
    throw 'device_identity.c must not mix in the Bluetooth or SoftAP MAC.'
}

# 3. 蓝牙侧只能用共享名字，不能自己再拼一个。
if ($ble -match '#define BLE_FIXED_ADV_NAME|snprintf\(s_adv_name') {
    throw 'ble_provision.c still builds its own advertising name.'
}
if ($ble -notmatch 'strlcpy\(s_adv_name, device_identity_name\(\)') {
    throw 'ble_provision.c must take the advertising name from device_identity_name().'
}

# 4. 热点侧同理，并且不能再受配置或 BluFi 下发影响。
if ($wifi -notmatch 'strlcpy\(s_ap_name, device_identity_name\(\)') {
    throw 'wifi_manager.c must fill s_ap_name from device_identity_name().'
}
if ($wifi -match 'strlcpy\(s_ap_name, cfg->ap_name') {
    throw 'wifi_manager.c must not take the AP name from app_config any more.'
}
if ($wifi -match 'void wifi_manager_set_ap_name') {
    throw 'wifi_manager_set_ap_name must stay removed; a no-op setter would look like it works.'
}
if ($ble -match 'wifi_manager_set_ap_name\(|s_cfg\.ap_name, param->softap_ssid') {
    throw 'ble_provision.c must ignore the BluFi SoftAP SSID field.'
}

# 5. 路由器后台里显示的 DHCP 主机名同样要来自这个字符串。
#    不设的时候它是 sdkconfig 里那个固定的 "espressif"，一排设备在后台里全同名——
#    这正是领导提这件事的原因，比其他两处更不能再退回去。
if ($wifi -notmatch 'const char \*hostname = device_identity_name\(\)') {
    throw 'wifi_manager.c must take the DHCP hostname from device_identity_name().'
}
if ($wifi -notmatch 'esp_netif_set_hostname\(s_sta_netif, hostname\)') {
    throw 'wifi_manager.c must apply the DHCP hostname to the STA netif before DHCP starts.'
}

# 6. 老名字不能再被当成默认值写进配置，否则升级过的机器会带着旧名回来。
#    同样只看赋值语句，注释里提到旧名字是在解释为什么废弃它。
if ($cfg -match 'strlcpy\(cfg->ble_name|strlcpy\(cfg->ap_name') {
    throw 'app_config.c still seeds an obsolete device name.'
}

Write-Output 'Device identity naming checks passed.'
