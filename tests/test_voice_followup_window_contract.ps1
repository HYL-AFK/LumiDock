$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"
$header = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.h"

@(
    @{ Content = $voice; Pattern = 'XIAOZHI_FOLLOWUP_WINDOW_US \(6400LL \* 1000LL\)'; Message = 'The post-TTS follow-up window must be 6.4 seconds.' },
    @{ Content = $voice; Pattern = 'XIAOZHI_VOICE_WAIT_FOLLOWUP'; Message = 'Voice state machine must distinguish a follow-up window from normal wake-word idle.' },
    @{ Content = $voice; Pattern = 'xiaozhi_audio_pipeline_arm_followup\(\);'; Message = 'TTS completion must arm local VAD for a follow-up question.' },
    @{ Content = $voice; Pattern = 's_followup_deadline_us = esp_timer_get_time\(\) \+ XIAOZHI_FOLLOWUP_WINDOW_US;'; Message = 'TTS completion must set a bounded follow-up deadline.' },
    @{ Content = $voice; Pattern = '#define XIAOZHI_FOLLOWUP_DIRECT_REPLY_MIN_PEAK 2500U'; Message = 'A follow-up VAD start must clear a close-mic energy floor before it consumes the bounded window.' },
    @{ Content = $voice; Pattern = 'follow-up VAD start gated as ambient noise: peak=%u threshold=%u'; Message = 'Ambient noise inside the follow-up window must be logged and ignored so the window can expire and close the session.' },
    @{ Content = $voice; Pattern = 'const uint32_t bypass_peak = followup_reply \? XIAOZHI_FOLLOWUP_DIRECT_REPLY_MIN_PEAK'; Message = 'The post-playback gate must apply the higher follow-up floor instead of the boot-greeting bypass floor.' },
    @{ Content = $voice; Pattern = 'follow-up window expired; requesting generated exit prompt'; Message = 'Follow-up timeout must request the cloud-generated farewell.' },
    @{ Content = $pipeline; Pattern = 'void xiaozhi_audio_pipeline_arm_followup\(void\)'; Message = 'Audio pipeline must expose a safe VAD-only follow-up arm function.' },
    @{ Content = $header; Pattern = 'void xiaozhi_audio_pipeline_arm_followup\(void\);'; Message = 'Follow-up arm function must be declared for the voice state machine.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

# The follow-up branch must reject ambient noise before it can consume the
# bounded window: only a real close-mic reply may start the next cloud turn,
# and the energy floor has to be checked before start_followup_listen().
$followStart = $voice.IndexOf('else if (s_state == XIAOZHI_VOICE_WAIT_FOLLOWUP)')
$followEnd = $voice.IndexOf('case VOICE_EVENT_VAD_END:', $followStart)
if ($followStart -lt 0 -or $followEnd -lt 0) {
    throw 'The follow-up VAD start branch is missing.'
}
$followBranch = $voice.Substring($followStart, $followEnd - $followStart)
if ($followBranch -notmatch 'if \(s_wait_for_turn_silence\)' -or
    $followBranch -notmatch 'XIAOZHI_FOLLOWUP_DIRECT_REPLY_MIN_PEAK' -or
    $followBranch -notmatch 'follow-up VAD start gated as ambient noise' -or
    $followBranch -notmatch 'start_followup_listen\(\);') {
    throw 'After residual TTS audio reaches silence, speech inside the follow-up window must initiate direct cloud streaming without a wake word.'
}
$floorPos = $followBranch.IndexOf('XIAOZHI_FOLLOWUP_DIRECT_REPLY_MIN_PEAK')
$listenPos = $followBranch.IndexOf('start_followup_listen();')
if ($floorPos -lt 0 -or $listenPos -lt 0 -or $listenPos -lt $floorPos) {
    throw 'The follow-up energy floor must be checked before start_followup_listen() consumes the window.'
}

# 第一段聆听窗口没等到人说话时，先播一次“我没听清，可以再说一遍吗？”再开一轮窗口，
# 第二次到期才走原来的退出流程。三条不变式缺一不可：
#   1. 重开必须是一次性的（s_followup_retry_used），否则设备永远不回待机；
#   2. 重开必须排在 exit 分支前面，否则永远走不到；
#   3. 重开必须驱动 timeout prompt，不能驱动 exit prompt——exit prompt 播完
#      enter_followup_after_playback() 会直接 close_channel，窗口根本不会重开。
if ($voice -notmatch 'static bool s_followup_retry_used;') {
    throw 'The follow-up retry must be a one-shot latch, not an unbounded re-arm.'
}
$retryPos = $voice.IndexOf('else if (!s_followup_retry_used)')
$exitPos = $voice.IndexOf('follow-up window expired; requesting generated exit prompt')
if ($retryPos -lt 0 -or $exitPos -lt 0 -or $retryPos -gt $exitPos) {
    throw 'The one-shot retry must be tested before the exit path, or a silent window still exits immediately.'
}
if ($voice -notmatch 's_followup_retry_used = true;[\s\S]{0,200}?if \(!request_cloud_timeout_prompt\(\)\)') {
    throw 'A silent follow-up window must replay the did-not-catch prompt, which is what re-arms the window.'
}
if ($voice -notmatch 'follow-up window silent; playing the retry prompt and re-arming once') {
    throw 'The follow-up retry must be observable in the log.'
}
# 复位两处：会话收尾（否则一次沉默永久吃掉后续每次重开）、用户真的接话了
# （否则一段会话里只有第一次沉默能重开）。
if ($voice -notmatch 's_session_exit_clean = false;[\s\S]{0,200}?s_followup_retry_used = false;') {
    throw 'close_channel must reset the retry latch so the next session gets its own retry.'
}
if ($voice -notmatch 'start_followup_listen\(\);[\s\S]{0,200}?s_followup_retry_used = false;') {
    throw 'A real follow-up reply must reset the retry latch for the next silence.'
}

Write-Output 'Voice follow-up window contract passed.'
