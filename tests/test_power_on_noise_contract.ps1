$ErrorActionPreference = 'Stop'

# 上电噪声与广播可发现性的约束。
#
# 功放 NS4150B 的使能脚没引到板上，它从通电那一刻就开着；而它前面挂的 GPIO7 在上电
# 后一直是高阻。输入端悬空是模拟电路最典型的噪声源，听感就是开机那一两秒的“滋啦”。
# 下面两条把“尽早给功放一个确定电平”和“别把时钟脚驱动开满”钉住，最后一条守住手机
# 扫描能不能看到设备。
#
# 这三处共同点：删掉或改坏都能编译通过、功能看着也正常，只有耳朵能发现问题。

$root = "$PSScriptRoot\..\main"
$mainC = Get-Content -Raw -LiteralPath "$root\main.c"
$driver = Get-Content -Raw -LiteralPath "$root\Dri\i2s_audio_driver.c"
$ble = Get-Content -Raw -LiteralPath "$root\MID\network\ble_provision.c"

# 1. app_main 的第一件事必须是把功放输入摁到确定电平。从 ROM 到 app_main 之间约有
#    1.1 s，软件进不去；能做的只有让“第二早”的那一刻就结束悬空。
if ($mainC -notmatch 'quiet_amp_input_until_i2s_takes_over\(\);') {
    throw 'app_main must drive the amplifier input to a defined level before audio init.'
}
if ($mainC -notmatch 'gpio_set_level\(LUMIDOCK_I2S_AMP_DIN_GPIO,\s*0\)') {
    throw 'The amplifier input must be held low until the I2S peripheral takes the pin over.'
}

# 2. I2S 的四个信号脚必须留在最低驱动能力。16 kHz 帧率、4.096 MHz MCLK 用不着更陡的
#    边沿，而边沿越陡，往模拟端串的高频成分越重，最终都落到那个常开着的功放上。
foreach ($pin in @('LUMIDOCK_I2S_MCLK_GPIO', 'LUMIDOCK_I2S_BCLK_GPIO', 'LUMIDOCK_I2S_LRCLK_GPIO', 'LUMIDOCK_I2S_AMP_DIN_GPIO')) {
    if ($driver -notmatch "gpio_set_drive_capability\($pin,\s*GPIO_DRIVE_CAP_0\)") {
        throw "I2S pin $pin must stay at GPIO_DRIVE_CAP_0 so clock edges do not couple into the analog path."
    }
}

# 3. 广播间隔落在 500~800 ms。放到 1000~2000 ms 手机上扫不出来（实测），放到 NimBLE
#    默认的 30~60 ms 又会让 WiFi 在共存调度里排队，开机那段的语音上行写不出去。
if ($ble -notmatch 'itvl_min = BLE_GAP_ADV_ITVL_MS\(500\)') {
    throw 'BLE advertising must start at 500 ms; slower than this is not discoverable on phones.'
}
if ($ble -notmatch 'itvl_max = BLE_GAP_ADV_ITVL_MS\(800\)') {
    throw 'BLE advertising must cap at 800 ms; the 30~60 ms default starves WiFi coexistence.'
}

Write-Output 'power-on noise and BLE advertising contract OK'
