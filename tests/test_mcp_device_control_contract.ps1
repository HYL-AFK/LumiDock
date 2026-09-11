$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$voice = Get-Content -Raw -LiteralPath (Join-Path $root 'main\MID\voice\xiaozhi_voice_service.c')
$iot = Get-Content -Raw -LiteralPath (Join-Path $root 'main\MID\system\voice_iot_command_service.c')
$iotHeader = Get-Content -Raw -LiteralPath (Join-Path $root 'main\MID\system\voice_iot_command_service.h')
$mcp = Get-Content -Raw -LiteralPath (Join-Path $root 'main\MID\system\mcp_service.c')
$mcpHeader = Get-Content -Raw -LiteralPath (Join-Path $root 'main\MID\system\mcp_service.h')
$settings = Get-Content -Raw -LiteralPath (Join-Path $root 'main\MID\system\device_settings_service.c')
$settingsHeader = Get-Content -Raw -LiteralPath (Join-Path $root 'main\MID\system\device_settings_service.h')
$app = Get-Content -Raw -LiteralPath (Join-Path $root 'main\APP\app_manager.c')
$cmake = Get-Content -Raw -LiteralPath (Join-Path $root 'main\CMakeLists.txt')

# ---------------------------------------------------------------- volume NVS
# The level the user hears after a reboot must be the level they left behind,
# so every path that changes it has to hand the value to the settings service.
@(
    @{ Content = $cmake; Pattern = 'MID/system/device_settings_service\.c'; Message = 'The settings service must be compiled.' },
    @{ Content = $settingsHeader; Pattern = 'device_settings_service_init\(void\)'; Message = 'The settings service must be initialised once at boot.' },
    @{ Content = $settingsHeader; Pattern = 'device_settings_service_apply_saved_volume\(void\)'; Message = 'The stored volume needs an explicit apply step.' },
    @{ Content = $settingsHeader; Pattern = 'device_settings_service_request_volume_save\(int percent\)'; Message = 'Volume changes need a non-blocking persist request.' },
    @{ Content = $settings; Pattern = '#define DEVICE_SETTINGS_NVS_NAMESPACE "device_cfg"'; Message = 'Device preferences must not share the Wi-Fi namespace.' },
    @{ Content = $settings; Pattern = 'nvs_set_i32\(handle, DEVICE_SETTINGS_KEY_VOLUME'; Message = 'The volume has to reach NVS.' },
    @{ Content = $settings; Pattern = 'nvs_get_i32\(handle, DEVICE_SETTINGS_KEY_VOLUME'; Message = 'The stored volume has to be read back at boot.' },
    @{ Content = $settings; Pattern = 'nvs_commit\(handle\)'; Message = 'The write must be committed or a reboot loses it.' },
    @{ Content = $settings; Pattern = 'MALLOC_CAP_INTERNAL \| MALLOC_CAP_8BIT'; Message = 'NVS I/O disables the SPI cache, so the writer stack must live in internal RAM.' },
    @{ Content = $app; Pattern = 'device_settings_service_init\(\)'; Message = 'Boot must load the stored volume.' },
    @{ Content = $app; Pattern = 'device_settings_service_apply_saved_volume\(\)'; Message = 'Boot must re-apply the stored volume to the codec.' },
    @{ Content = $mcp; Pattern = 'device_settings_service_request_volume_save\(target\)'; Message = 'The cloud volume tool must persist what it applied.' },
    @{ Content = $iot; Pattern = 'device_settings_service_request_volume_save'; Message = 'The local volume shortcut must persist what it applied.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

$codecUpAt = $app.IndexOf('i2s_audio_driver_init()')
$applyVolumeAt = $app.IndexOf('device_settings_service_apply_saved_volume()')
if ($codecUpAt -lt 0 -or $applyVolumeAt -lt 0 -or $codecUpAt -gt $applyVolumeAt) {
    throw 'The stored volume can only be applied after the codec is initialised.'
}

# ------------------------------------------------------- absolute volume (local)
# "把音量调到30" must work without the cloud as well, and "最大/最小" share the
# "音量调到" prefix with the numbered form, so they have to be matched first.
@(
    @{ Content = $iot; Pattern = 'static bool parse_volume_level'; Message = 'Absolute volume phrases need a local parser.' },
    @{ Content = $iot; Pattern = '"音量调到"'; Message = 'The absolute volume vocabulary must exist locally.' },
    @{ Content = $iot; Pattern = '百分之'; Message = 'Both "30" and "百分之三十" must parse.' },
    @{ Content = $iot; Pattern = 'static const char \*const volume_max_phrases'; Message = '"音量调到最大" needs its own phrase table.' },
    @{ Content = $iot; Pattern = 'static const char \*const volume_min_phrases'; Message = '"音量调到最小" needs its own phrase table.' },
    @{ Content = $iot; Pattern = 'if \(value > 100U\) value = 100U;'; Message = 'A parsed level above 100 must be clamped.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

$maxBranchAt = $iot.IndexOf('return apply_local_volume(transcript, 100.0f);')
$numberedBranchAt = $iot.IndexOf('return apply_local_volume(transcript, target);')
if ($maxBranchAt -lt 0 -or $numberedBranchAt -lt 0 -or $maxBranchAt -gt $numberedBranchAt) {
    throw 'The max/min branches must run before the numbered form or "音量调到最大" is parsed as a level.'
}

# ------------------------------------------------- cloud-owned timer + radio
$tools = @(
    'self.timer.set_alarm', 'self.timer.cancel_alarm', 'self.timer.set_countdown',
    'self.timer.start_stopwatch', 'self.timer.pause_stopwatch', 'self.timer.stop_stopwatch',
    'self.timer.stop_ringing', 'self.radio.play', 'self.radio.pause',
    'self.radio.resume', 'self.radio.stop'
)
$macros = @(
    'MCP_TOOL_SET_ALARM', 'MCP_TOOL_CANCEL_ALARM', 'MCP_TOOL_SET_COUNTDOWN',
    'MCP_TOOL_START_STOPWATCH', 'MCP_TOOL_PAUSE_STOPWATCH', 'MCP_TOOL_STOP_STOPWATCH',
    'MCP_TOOL_STOP_RINGING', 'MCP_TOOL_RADIO_PLAY', 'MCP_TOOL_RADIO_PAUSE',
    'MCP_TOOL_RADIO_RESUME', 'MCP_TOOL_RADIO_STOP'
)
$catalogueStart = $mcp.IndexOf('static const char k_tools_list_result_json')
$assertStart = $mcp.IndexOf('_Static_assert(sizeof(k_tools_list_result_json)')
if ($catalogueStart -lt 0 -or $assertStart -lt 0 -or $catalogueStart -gt $assertStart) {
    throw 'The static tool catalogue and its compile-time size guard are both required.'
}
$catalogue = $mcp.Substring($catalogueStart, $assertStart - $catalogueStart)

$callStart = $mcp.IndexOf('static const char *handle_tools_call')
if ($callStart -lt 0) { throw 'The tool dispatcher is missing.' }
$dispatch = $mcp.Substring($callStart)

for ($i = 0; $i -lt $tools.Count; $i++) {
    if ($mcp -notmatch [regex]::Escape($tools[$i])) {
        throw "MCP must advertise the device tool $($tools[$i])."
    }
    if ($catalogue -notmatch [regex]::Escape($macros[$i])) {
        throw "The tools/list catalogue must list $($macros[$i])."
    }
    if ($dispatch -notmatch "strcmp\(tool, $($macros[$i])\)") {
        throw "tools/call must dispatch $($macros[$i])."
    }
}

# The tools have to reach the real services, not just answer "true".
@(
    @{ Content = $mcp; Pattern = 'timer_service_set_alarm'; Message = 'The alarm tool must drive the timer service.' },
    @{ Content = $mcp; Pattern = 'timer_service_cancel_alarm'; Message = 'The cancel-alarm tool must drive the timer service.' },
    @{ Content = $mcp; Pattern = 'timer_service_prepare_countdown'; Message = 'The countdown tool must reserve the countdown, not start it under the answer.' },
    @{ Content = $mcp; Pattern = 'timer_service_start_stopwatch\(\)'; Message = 'The stopwatch tools must drive the timer service.' },
    @{ Content = $mcp; Pattern = 'timer_service_stop_ringing\(\)'; Message = 'The stop-ringing tool must silence the chime immediately.' },
    @{ Content = $mcp; Pattern = 'timer_service_is_time_synced\(\)'; Message = 'An alarm without a synchronized clock must be refused.' },
    @{ Content = $mcp; Pattern = 'radio_stream_service_pause\(\)'; Message = 'The radio tools must drive the radio service.' },
    @{ Content = $mcp; Pattern = 'radio_stream_service_resume\(\)'; Message = 'The radio tools must drive the radio service.' },
    @{ Content = $mcp; Pattern = 'radio_stream_service_stop\(\)'; Message = 'The radio tools must drive the radio service.' },
    @{ Content = $mcp; Pattern = [regex]::Escape('\"radio\":{\"playing\":%s}'); Message = 'get_device_status must report the radio state.' },
    @{ Content = $mcp; Pattern = [regex]::Escape('\"countdown_remaining_seconds\":%lu'); Message = 'get_device_status must report the countdown remaining time.' },
    @{ Content = $mcp; Pattern = [regex]::Escape('\"stopwatch_seconds\":%lu'); Message = 'get_device_status must report the stopwatch elapsed time.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

# 只有一块闹钟槽位，设新的会顶掉旧的。工具返回值是模型唯一能看到的执行报告，
# 必须带上被顶掉的那个时间，并在描述里硬要求模型转述——否则用户设第二个闹钟时
# 前一个会无声消失，而设备自己说不出动态文本。
if ($mcp -notmatch 'timer_service_get_snapshot\(&previous\)') {
    throw 'The alarm tool must read the pending alarm before replacing it.'
}
if ($mcp -notmatch 'The previous alarm at %02d:%02d was cancelled') {
    throw 'The set-alarm result must name the alarm that was replaced.'
}
if ($mcp -notmatch 'you must tell the user which alarm was cancelled') {
    throw 'The set-alarm description must require the model to announce the replaced alarm.'
}
if ($mcp -notmatch 'result=%d replaced=%d') {
    throw 'The set-alarm log must make the replacement visible in field logs.'
}

# ------------------------------------------------------- deferred media start
foreach ($fn in @('mcp_service_pending_countdown', 'mcp_service_clear_pending_countdown',
                  'mcp_service_pending_radio', 'mcp_service_clear_pending_radio')) {
    if ($mcpHeader -notmatch $fn) { throw "The MCP header must expose $fn." }
    if ($mcp -notmatch "$fn\(void\)") { throw "The MCP service must implement $fn." }
}

# A tool call always lands before the acknowledgement TTS, so a deferred start
# may never be dropped: the drain handler, the teardown and the idle tick all
# have to pick the request up.
$countdownSites = ([regex]::Matches($voice, 'mcp_service_pending_countdown\(\)')).Count
$radioSites = ([regex]::Matches($voice, 'mcp_service_pending_radio\(\)')).Count
if ($countdownSites -lt 4) {
    throw "Deferred countdowns need a consumer on every exit (TTS drain, wake abort, close, idle tick): found $countdownSites."
}
if ($radioSites -lt 3) {
    throw "Deferred radio starts need a consumer on every exit (TTS drain, close, idle tick): found $radioSites."
}
if ($voice -notmatch 'mcp_service_clear_pending_countdown\(\)' -or
    $voice -notmatch 'mcp_service_clear_pending_radio\(\)') {
    throw 'Every consumer must clear the flag it took, or the start replays on the next tick.'
}

$helperAt = $voice.IndexOf('static void consume_deferred_media_starts')
$helperEnd = $voice.IndexOf('static void close_channel(bool', $helperAt)
if ($helperAt -lt 0 -or $helperEnd -lt 0) { throw 'The idle backstop for deferred starts is missing.' }
$helper = $voice.Substring($helperAt, $helperEnd - $helperAt)
foreach ($state in @('XIAOZHI_VOICE_WAIT_RESPONSE', 'XIAOZHI_VOICE_TTS_BUFFERING',
                     'XIAOZHI_VOICE_SPEAKING', 'XIAOZHI_VOICE_TTS_DRAINING')) {
    if ($helper -notmatch $state) {
        throw "The idle backstop must stay out of $state, where the cloud still owns the speaker."
    }
}
if ($helper -notmatch 'radio_stream_service_play_default\(\)' -or
    $helper -notmatch 'timer_service_start_pending_countdowns\(\)') {
    throw 'The idle backstop must actually start what was deferred.'
}
if ($voice -notmatch 'consume_deferred_media_starts\(\);') {
    throw 'The idle tick must run the deferred-start backstop.'
}

# ------------------------------------------------------------ single owner
# The cloud tools and the local shortcuts must never both apply a command.
if ($iot -notmatch '!stop_ring_requested') {
    throw 'Silencing a ringing timer must stay local; the websocket round trip is too slow.'
}
if ($iot -notmatch 'timer_requested') {
    throw 'Timer phrases must stand down for the cloud tools exactly like the volume does.'
}
# A stand-down still has to report a command, otherwise the voice service falls
# through to the chat/query confirmation and answers "正在思考" for a command.
if ($iot -notmatch 'VOICE_IOT_COMMAND_DEFERRED') {
    throw 'A deferred command must return VOICE_IOT_COMMAND_DEFERRED so it confirms with "正在执行".'
}
if ($iotHeader -notmatch 'VOICE_IOT_COMMAND_DEFERRED') {
    throw 'VOICE_IOT_COMMAND_DEFERRED must be declared in the IoT command header.'
}
# The radio never stands down: the cloud model answered "打开电台" with a chat
# about genres instead of calling self.radio.play, so the station must start
# from the local shortcut whatever the cloud offers.
if ($iot -match 'volume_requested \|\| timer_requested \|\| radio_requested') {
    throw 'The radio must not stand down for the cloud tools; the station has to start locally.'
}
if ($iot -notmatch 'return VOICE_IOT_COMMAND_RADIO_PLAY;') {
    throw 'The local radio shortcut must start the station through VOICE_IOT_COMMAND_RADIO_PLAY.'
}

# 相对音量必须留在本地。让位出去会掉进一个两边都不管的缝：设备让位后不再步进，
# 而 set_volume 的工具描述写着 "instead of stepping the volume up or down"，云端
# 因此也不会调它。实测在最大音量下说“调高音量”就是没有任何动静，几秒后只剩退出语。
$standDownStart = $iot.IndexOf('const bool volume_absolute_requested')
$standDownEnd = $iot.IndexOf('const bool timer_requested', $standDownStart)
if ($standDownStart -lt 0 -or $standDownEnd -lt 0) {
    throw 'The volume stand-down set must stay explicit and named.'
}
$volumeStandDown = $iot.Substring($standDownStart, $standDownEnd - $standDownStart)
if ($volumeStandDown -match 'volume_up_phrases|volume_down_phrases') {
    throw 'Relative volume must stay local: the cloud tool forbids stepping, so neither side would move the speaker.'
}
# 到顶和到底要给回一句，否则“已经在最大声了”永远等不到。
if ($iot -notmatch 'volume already at maximum' -or $iot -notmatch 'volume already at minimum') {
    throw 'The local volume step needs boundary handling so the top and bottom of the range are visible.'
}

# ------------------------------------------- the local radio command wins the turn
# The cloud transcript is lossy: "关闭电台" reached the device as "电台。", every
# phrase table missed, and the model then picked self.radio.play for the very same
# utterance - restarting the station the local shortcut had just stopped.
if ($iot -notmatch 'static bool radio_keyword_only') {
    throw 'A verb-less "电台。" transcript needs its own keyword matcher.'
}
# ASCII 不能再被当成实词：实测“关闭电台”被云端 ASR 吐成“B电台。”，那个凭空
# 多出来的字母曾经把兜底挡在门外（每一个字母都直接否决停止），云端模型于是
# 把刚停掉的电台又拉了起来。
$keywordFn = $iot.Substring($iot.IndexOf('static bool radio_keyword_only'), 700)
if ($keywordFn -match "byte >= '0'") {
    throw 'The keyword fallback must treat ASCII as ASR noise; a stray "B" in "B电台。" may not veto the stop.'
}
# 噪声字不止 ASCII：实测“停止电台”被吐成“市电台。”（切开头的那个音节被 ASR 猜
# 成了一个不相干的汉字），一个字节一个字节看的旧规则同样直接否决。规则放宽成
# “电台前面允许挂少量噪声字，电台之后不许再有实词”，仍然只兜底停止。
if ($iot -notmatch '#define RADIO_KEYWORD_LEADING_NOISE_MAX 3U') {
    throw 'The keyword fallback needs a bounded leading-noise budget: "市电台。" is the observed CJK form of the swallowed verb.'
}
if ($iot -notmatch 'if \(keyword_seen \|\| leading_noise >= RADIO_KEYWORD_LEADING_NOISE_MAX\) return false;') {
    throw 'Only leading noise may pass; a real word after "电台" (e.g. "电台关小点") must still go to the cloud.'
}
if ($iot -notmatch 'if \(punctuation != 0U\)') {
    throw 'Punctuation must be consumed before the leading-noise budget, or "电台。" spends it on the full stop.'
}
# 云端会吃掉动词首字，短语表必须收下观测到的短形式，否则本地表现与云端一样瞎。
foreach ($phrase in @('"开电台"', '"关电台"')) {
    if ($iot -notmatch [regex]::Escape($phrase)) {
        throw "The local radio vocabulary must cover the truncated form $phrase the ASR actually produces."
    }
}
if ($iot -notmatch 'radio_keyword_only\(transcript\) && radio_stream_service_is_requested\(\)') {
    throw 'The keyword fallback may only stop a station that is actually playing; is_requested covers the parked phase.'
}
$radioStopAt = $iot.IndexOf('contains_any(transcript, radio_stop_phrases')
$keywordAt = $iot.IndexOf('if (radio_keyword_only(transcript)')
if ($radioStopAt -lt 0 -or $keywordAt -lt 0 -or $keywordAt -lt $radioStopAt) {
    throw 'The keyword fallback must sit behind the explicit radio phrase tables.'
}
if ($iotHeader -notmatch 'VOICE_IOT_COMMAND_RADIO_HANDLED') {
    throw 'A locally applied radio command needs its own result code.'
}
if ($iot -notmatch 'VOICE_IOT_COMMAND_RADIO_HANDLED : VOICE_IOT_COMMAND_FAILED') {
    throw 'The local radio pause/stop must report VOICE_IOT_COMMAND_RADIO_HANDLED.'
}

# The cloud tools for the radio must be muted for the rest of that turn, and the
# mute has to answer "true": an error only makes the cloud retry the call.
if ($mcpHeader -notmatch 'mcp_service_set_radio_tools_blocked\(bool blocked\)') {
    throw 'The MCP header must expose the radio-tool mute.'
}
if ($mcp -notmatch 'void mcp_service_set_radio_tools_blocked\(bool blocked\)') {
    throw 'The MCP service must implement the radio-tool mute.'
}
if ($dispatch -notmatch 'if \(s_radio_tools_blocked && is_radio_tool\(tool\)\)') {
    throw 'The radio tools must be muted before the dispatcher reaches them.'
}
if ($dispatch -notmatch 'tool call ignored: %s already handled locally') {
    throw 'A muted radio tool must say so instead of failing silently.'
}
if ($voice -notmatch 'mcp_service_set_radio_tools_blocked\(true\);') {
    throw 'A handled local radio command must mute the cloud radio tools for the turn.'
}
if ($voice -notmatch 'mcp_service_set_radio_tools_blocked\(false\);') {
    throw 'The radio-tool mute must be lifted when the turn tears down.'
}
if ($voice -notmatch 'result == VOICE_IOT_COMMAND_RADIO_PLAY \|\|\s*result == VOICE_IOT_COMMAND_RADIO_HANDLED') {
    throw 'The mute must cover the stopped radio too, not only the deferred start.'
}

Write-Output 'MCP device control contract passed.'
