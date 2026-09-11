$ErrorActionPreference = 'Stop'

$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

@(
    @{ Content = $pipeline; Pattern = 'get_feed_channel_num\(s_pipeline\.afe_data\)'; Message = 'AFE feed buffers must be sized for the actual configured input channel count.' },
    @{ Content = $pipeline; Pattern = 'audio_detect_task started'; Message = 'The AFE fetch task must report that it has started.' },
    @{ Content = $pipeline; Pattern = 'audio_detect_task received first AFE frame'; Message = 'The AFE fetch task must report its first consumed frame.' },
    @{ Content = $pipeline; Pattern = 'AFE input: rms=%u peak=%u feed=%d/%u'; Message = 'The AFE feed task must emit rate-limited live PCM energy diagnostics.' },
    @{ Content = $pipeline; Pattern = 'AFE state: wake=%d vad=%d data=%d cache=%d'; Message = 'The AFE fetch task must emit rate-limited WakeNet and VAD state diagnostics before wakeup.' },
    @{ Content = $pipeline; Pattern = 'last_input_diagnostic_us'; Message = 'PCM energy diagnostics must be rate limited.' },
    @{ Content = $pipeline; Pattern = 'last_afe_diagnostic_us'; Message = 'AFE state diagnostics must be rate limited.' },
    @{ Content = $pipeline; Pattern = '#define AFE_DIAGNOSTIC_INTERVAL_US'; Message = 'Both AFE heartbeat logs must share one tunable interval.' },
    @{ Content = $pipeline; Pattern = 'audio task created: voice_feed'; Message = 'Voice task creation must be reported per task.' },
    @{ Content = $pipeline; Pattern = 'audio task created: voice_detect'; Message = 'Voice detection task creation must be reported per task.' },
    @{ Content = $voice; Pattern = 'WakeNet detected; queuing voice session'; Message = 'A UI voice session must have an auditable WakeNet trigger.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

# 两条日志原来是 1 秒一条，实测占掉一份 247 行日志的 25%。现在心跳拉到 5 秒，
# 所以 AFE state 必须是“变了就打 + 心跳兜底”：5 秒的粒度会把真正的状态跃迁
# （vad 0->1、cache 0->8192）盖过去，而那些恰好是排查时最想看的东西。
if ($pipeline -notmatch 'diagnostic_changed \|\| now_us - last_afe_diagnostic_us >= AFE_DIAGNOSTIC_INTERVAL_US') {
    throw 'The AFE state log must fire on change as well as on the slower heartbeat.'
}
if ($pipeline -notmatch 'if \(now_us - last_input_diagnostic_us >= AFE_DIAGNOSTIC_INTERVAL_US\)') {
    throw 'The PCM energy heartbeat must use the shared interval, not a hard-coded 1 s.'
}

Write-Output 'AFE diagnostics contract passed.'
