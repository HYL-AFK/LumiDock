$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$voice = Get-Content -Raw -LiteralPath (Join-Path $root 'main\MID\voice\xiaozhi_voice_service.c')
$iot = Get-Content -Raw -LiteralPath (Join-Path $root 'main\MID\system\voice_iot_command_service.c')
$mcp = Get-Content -Raw -LiteralPath (Join-Path $root 'main\MID\system\mcp_service.c')
$header = Get-Content -Raw -LiteralPath (Join-Path $root 'main\MID\system\mcp_service.h')
$cmake = Get-Content -Raw -LiteralPath (Join-Path $root 'main\CMakeLists.txt')

@(
    @{ Content = $cmake; Pattern = 'MID/system/mcp_service\.c'; Message = 'The MCP service must be compiled.' },
    @{ Content = $header; Pattern = 'mcp_service_handle'; Message = 'The MCP service must expose its payload entry point.' },
    @{ Content = $voice; Pattern = 'strcmp\(type->valuestring, "mcp"\)'; Message = 'Voice service must route cloud MCP frames.' },
    @{ Content = $voice; Pattern = 'mcp_service_handle\(payload\)'; Message = 'Voice service must delegate MCP payloads to the MCP service.' },
    @{ Content = $voice; Pattern = 'send_ws_text_raw\(s_mcp_tx_frame\)'; Message = 'MCP replies must leave through the shared websocket sender.' },
    @{ Content = $voice; Pattern = '\\"mcp\\":true'; Message = 'Hello must keep advertising the mcp feature or discovery never starts.' },
    @{ Content = $mcp; Pattern = '"initialize"'; Message = 'MCP must answer initialize.' },
    @{ Content = $mcp; Pattern = '"tools/list"'; Message = 'MCP must answer tools/list.' },
    @{ Content = $mcp; Pattern = '"tools/call"'; Message = 'MCP must answer tools/call.' },
    @{ Content = $mcp; Pattern = '\\"inputSchema\\"'; Message = 'Every tool needs an inputSchema for the cloud to build its call.' },
    @{ Content = $mcp; Pattern = '2024-11-05'; Message = 'MCP must report the protocol version the cloud expects.' },
    @{ Content = $mcp; Pattern = 'self\.audio_speaker\.set_volume'; Message = 'The volume tool must keep its upstream name or the cloud prompt will not match it.' },
    @{ Content = $mcp; Pattern = 'self\.get_device_status'; Message = 'The status tool is the cloud''s first step before it changes the volume.' },
    @{ Content = $mcp; Pattern = 'i2s_audio_driver_set_output_volume'; Message = 'The volume tool must drive the real audio output.' },
    @{ Content = $mcp; Pattern = 'cJSON_IsNumber\(id\)'; Message = 'Notifications without an id must never be answered.' },
    @{ Content = $mcp; Pattern = 'MCP_REPLY_MAX_BYTES'; Message = 'The reply buffer size must stay explicit.' },
    @{ Content = $mcp; Pattern = 'mcp_service_is_ready'; Message = 'The MCP service must expose its discovery state.' },
    @{ Content = $voice; Pattern = 'mcp_service_reset()'; Message = 'MCP discovery must be scoped to a single websocket session.' },
    @{ Content = $iot; Pattern = 'mcp_service_is_ready()'; Message = 'The local volume shortcut must stand down once the cloud can drive the volume.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

# The volume tool must reach the driver before it reports success, otherwise the
# cloud hears "true" for a change that never reached the codec.
$setVolumeAt = $mcp.IndexOf('handle_set_volume')
$driveAt = $mcp.IndexOf('i2s_audio_driver_set_output_volume')
$successAt = $mcp.IndexOf('return reply_tool_text(id, "true");')
if ($setVolumeAt -lt 0 -or $driveAt -lt 0 -or $successAt -lt 0) {
    throw 'The volume tool must keep its dispatch, driver call and success reply.'
}
if ($driveAt -lt $setVolumeAt -or $driveAt -gt $successAt) {
    throw 'The volume tool must drive the audio output before reporting success.'
}

# The stand-down has to run before the local shortcut, otherwise a request the
# cloud already handles would still be applied a second time here.
$deferAt = $iot.IndexOf('mcp_service_is_ready()')
$localVolumeAt = $iot.IndexOf('i2s_audio_driver_set_output_volume')
if ($deferAt -lt 0 -or $localVolumeAt -lt 0 -or $deferAt -gt $localVolumeAt) {
    throw 'The cloud-owned volume path must be checked before the local shortcut runs.'
}

# 工具描述必须和实际分工一致。旧描述写着 "...a specific level instead of stepping
# the volume up or down"，等于告诉云端“调高音量”不归它管；而设备那边又已经把这类
# 命令让位出去——命令落在两边都不管的缝里，实测就是说了没任何反应。
if ($mcp -match 'instead of stepping the volume up or down') {
    throw 'The old wording told the cloud to ignore relative volume requests, which left them to nobody.'
}
if ($mcp -notmatch 'must not be forwarded here') {
    throw 'The volume tool description must tell the cloud that relative requests stay on the device.'
}
# 猜值会被原样应用，必须要求先读当前值。
if ($mcp -notmatch 'Always call ' -or $mcp -notmatch 'self\.get_device_status first') {
    throw 'The volume tool must require reading the current level before changing it.'
}

# A truncated tools/list reply would silently disable device control, so the
# frame must stay inside the buffer it is printed into.
$frameBytes = [int]([regex]::Match($voice, 'XIAOZHI_MCP_FRAME_MAX_BYTES\s+(\d+)U').Groups[1].Value)
$replyBytes = [int]([regex]::Match($mcp, 'MCP_REPLY_MAX_BYTES\s+(\d+)U').Groups[1].Value)
if ($frameBytes -le $replyBytes) {
    throw "The MCP envelope ($frameBytes bytes) must be larger than the JSON-RPC payload ($replyBytes bytes)."
}

Write-Output 'MCP protocol contract passed.'
