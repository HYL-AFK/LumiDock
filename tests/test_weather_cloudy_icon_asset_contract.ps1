$ErrorActionPreference = 'Stop'

$svg = Get-Content -Raw -Encoding UTF8 -LiteralPath "$PSScriptRoot\..\main\assets\ui\weather_icon_cloudy.svg"
$preview = Get-Content -Raw -Encoding UTF8 -LiteralPath "$PSScriptRoot\..\preview\assets\weather\cloudy.svg"
$asset = Get-Item -LiteralPath "$PSScriptRoot\..\main\assets\ui\weather_icon_cloudy.rgb565a8"

foreach ($content in @($svg, $preview)) {
    if ($content -notmatch 'fill="#FFFFFF"' -or $content -notmatch 'fill="#C1C1C1"') {
        throw 'Cloudy weather icon must use the approved white and light-gray layered artwork.'
    }
    if ($content -match '<rect|<background|fill="white"') {
        throw 'Cloudy weather icon must not add an opaque background canvas.'
    }
}

if ($svg -notmatch 'width="72" height="72"') {
    throw 'Firmware cloudy weather SVG must be normalized to 72x72.'
}
if ($asset.Length -ne (72 * 72 * 2 + 72 * 72)) {
    throw 'Firmware cloudy weather asset must be a 72x72 RGB565A8 image.'
}

Write-Output 'Cloudy weather icon asset contract passed.'
