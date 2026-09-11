param([int]$Seconds = 25, [string]$Port = 'COM6')

# 抓 ESP32 串口日志到文件，供 AI 直接读取。
#
# 两个必须的细节：
#   1. Open() 前把 DTR/RTS 置低。否则会触发板子的自动复位电路，抓一次日志就
#      把设备重启一次。
#   2. Encoding 设成 UTF8。默认是 ASCII，云端说的中文会全变成 '?'，
#      而问候语、STT 文本这些恰恰是最需要看清的部分。
#
# 串口是独占的：跑这个之前必须关掉 idf.py monitor。

$p = New-Object System.IO.Ports.SerialPort $Port,115200,None,8,one
$p.DtrEnable = $false
$p.RtsEnable = $false
$p.Encoding = [System.Text.Encoding]::UTF8
$p.ReadTimeout = 500

$out = Join-Path $PSScriptRoot '..\tmp\com6.log'
$out = [System.IO.Path]::GetFullPath($out)
$dir = Split-Path $out -Parent
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }

try {
    $p.Open()
} catch {
    "CANNOT OPEN ${Port}: $($_.Exception.Message)"
    exit 1
}

# ReadLine 在设备静默时会抛超时异常、而且丢行；ReadExisting 把缓冲区里现成的字节
# 一次取走，配合短暂 sleep 轮询更稳。积满后一次性 UTF-8 写出。
$sb = New-Object System.Text.StringBuilder
$sw = [System.Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    Start-Sleep -Milliseconds 200
    $s = $p.ReadExisting()
    if ($s.Length -gt 0) { [void]$sb.Append($s) }
}
$p.Close()

[System.IO.File]::WriteAllText($out, $sb.ToString(),
    (New-Object System.Text.UTF8Encoding $false))
$lines = ($sb.ToString() -split "`n").Count
"captured $lines lines in $Seconds s -> $out"
