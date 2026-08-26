param(
    [string]$InputFile = "D:\Project\LumiDock_docs\音效候选\LumiDock_启动完成提示音_已选.mp3",
    [string]$OutputFile = "$PSScriptRoot\..\main\assets\audio\startup_ready_16k_mono_s16le.pcm",
    [string]$Ffmpeg = "D:\app\EVCapture\ffmpeg.exe"
)

$ErrorActionPreference = 'Stop'

if (!(Test-Path -LiteralPath $InputFile)) { throw "Input audio is missing: $InputFile" }
if (!(Test-Path -LiteralPath $Ffmpeg)) { throw "ffmpeg is missing: $Ffmpeg" }

$outDir = Split-Path -Parent $OutputFile
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
& $Ffmpeg -hide_banner -i $InputFile -af 'atempo=0.589,atrim=duration=4.0,afade=t=out:st=3.75:d=0.20' `
    -ar 16000 -ac 1 -f s16le -y $OutputFile
if ($LASTEXITCODE -ne 0) { throw "ffmpeg conversion failed: $LASTEXITCODE" }

$bytes = (Get-Item -LiteralPath $OutputFile).Length
if ($bytes -eq 0 -or $bytes -gt 128000 -or ($bytes % 2) -ne 0) {
    throw "Invalid output PCM: $bytes bytes"
}
$durationMs = [math]::Round($bytes * 1000.0 / (16000 * 2), 1)
Write-Output "Startup PCM ready: $bytes bytes, ${durationMs} ms, 16 kHz mono s16le"
