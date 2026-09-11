$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"
$pipelineHeader = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.h"
$playoutHeader = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\audio_playback_service.h"
$cmake = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\CMakeLists.txt"
$generator = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\scripts\generate_prompt_tones.py"

# 1. A dedicated local-prompt playout source must exist so it is not confused
#    with the cloud TTS stream (which would otherwise truncate the prompt).
if ($playoutHeader -notmatch 'AUDIO_PLAYOUT_SOURCE_LOCAL_PROMPT') {
    throw 'audio_playback_service.h must expose AUDIO_PLAYOUT_SOURCE_LOCAL_PROMPT.'
}

# 2. The pipeline forwards the local-prompt finish as its own event so the voice
#    service can release a queued cloud reply (a TTS FINISHED would be ignored).
if ($pipelineHeader -notmatch 'XIAOZHI_LOCAL_PROMPT_PLAYBACK_FINISHED') {
    throw 'xiaozhi_audio_pipeline.h must expose XIAOZHI_LOCAL_PROMPT_PLAYBACK_FINISHED.'
}
if ($pipeline -notmatch 'AUDIO_PLAYOUT_SOURCE_LOCAL_PROMPT') {
    throw 'xiaozhi_audio_pipeline.c playout_event must dispatch the local-prompt source.'
}

# 3. Intent classification must distinguish query intents so the correct prompt
#    phrase is chosen for weather/time/date prompts.
if ($voice -notmatch 'is_query_intent') {
    throw 'xiaozhi_voice_service.c must implement is_query_intent().'
}
if ($voice -notmatch '"天气"') {
    throw 'is_query_intent must include the weather query keyword.'
}
if ($voice -notmatch '"几点"') {
    throw 'is_query_intent must include the time query keyword.'
}
if ($voice -notmatch '"时间"') {
    throw 'is_query_intent must include the time query keyword.'
}

# 4. The three embedded prompt symbols must be referenced from the STT branch:
#    local command -> "正在执行", query -> "正在查询", chat -> "正在思考".
if ($voice -notmatch '_binary_prompt_executing_16k_mono_s16le_pcm_start') {
    throw 'The local-command branch must play prompt_executing.'
}
if ($voice -notmatch '_binary_prompt_querying_16k_mono_s16le_pcm_start') {
    throw 'The query branch must play prompt_querying.'
}
if ($voice -notmatch '_binary_prompt_thinking_16k_mono_s16le_pcm_start') {
    throw 'The chat branch must play prompt_thinking.'
}

# 5. The playout must not begin TTS while the local prompt is still active; the
#    cloud reply is held behind the prompt and released on its FINISHED event.
if ($voice -notmatch 's_pending_cloud_tts_after_prompt') {
    throw 'The tts start handler must defer the cloud reply behind a local prompt.'
}
if ($voice -notmatch 's_local_prompt_active && response_turn') {
    throw 'Deferring must happen only while the local prompt is still active.'
}
if ($voice -notmatch 'VOICE_EVENT_LOCAL_PROMPT_FINISHED') {
    throw 'The LOCAL_PROMPT_FINISHED event must be defined and handled.'
}
if ($voice -notmatch 'resume_deferred_cloud_tts') {
    throw 'The finished prompt must resume the held cloud reply.'
}
if ($voice -notmatch 'XIAOZHI_LOCAL_PROMPT_DEADLINE_US') {
    throw 'A watchdog must bound the local prompt so it cannot block the reply.'
}

# 5b. 云端回复的音频真的到了就必须让位，不能把包丢掉等提示音放完。
#     实测云端回得比提示音快得多：日志 67530 提示音开始、67560 第一个音频包就到，
#     到 68380 提示音放完为止一共丢了约 40 个包（约 2.4 s）；恢复播放时只剩 10 帧，
#     “电台已经在播放中啦～”只剩尾巴，前两个字整段没出声，紧接着就 underrun，
#     听起来就是卡卡的。
if ($voice -notmatch 'cloud reply audio arrived; local prompt yields') {
    throw 'The prompt hand-off to an arriving cloud reply must be visible in COM6 logs.'
}
$binaryAt = $voice.IndexOf('case VOICE_EVENT_WSS_BINARY:')
if ($binaryAt -lt 0 -or
    $voice.Substring($binaryAt, 200) -notmatch 'release_deferred_cloud_tts_on_audio\(event->value\);') {
    throw 'A binary packet that carries the deferred cloud reply must release it instead of being discarded.'
}
if ($voice -notmatch 'audio_playback_service_abort\(AUDIO_PLAYOUT_SOURCE_LOCAL_PROMPT, s_local_prompt_epoch\)') {
    throw 'Yielding to the cloud reply must abort the local prompt it interrupts.'
}

# 5c. 但让位是硬切：audio_playback_service_abort() 走 discard_frames() + 直接静音
#     codec，没有淡出。切在“正在执”和“行”之间就是一声闷响加半句话。实测云端首个
#     音频包平均 1.39 s 就位（23 个样本，中位数 1.07 s，最快 0.32 s），而提示音
#     只有 0.95 s——多数时候能放完，但总有场景正好切在句子中间。所以留一道 250 ms
#     的宽限：剩得不多就让最后两个字说完，由自然收尾去 resume 云端 TTS。
if ($voice -notmatch '#define XIAOZHI_LOCAL_PROMPT_GRACE_US \(250LL \* 1000LL\)') {
    throw 'The hard cut needs a bounded grace window so a nearly-finished prompt can complete.'
}
$gracePos = $voice.IndexOf('remaining_us <= XIAOZHI_LOCAL_PROMPT_GRACE_US')
$yieldPos = $voice.IndexOf('cloud reply audio arrived; local prompt yields')
if ($gracePos -lt 0 -or $yieldPos -lt 0 -or $gracePos -gt $yieldPos) {
    throw 'The grace check must run before the yield, or every arrival still hard-cuts the prompt.'
}
if ($voice -notmatch 'remaining_us <= XIAOZHI_LOCAL_PROMPT_GRACE_US\)[\s\S]{0,400}?return;') {
    throw 'The grace path must return early so the prompt can drain on its own.'
}
# 宽限分支绝不能清 s_pending_cloud_tts_after_prompt：清掉就再没有东西能被 resume，
# 提示音说完了云端却永远不出声。
$graceBlockStart = $voice.IndexOf('const int64_t remaining_us = s_local_prompt_end_us')
$yieldBlockStart = $voice.IndexOf('ESP_LOGI(TAG, "cloud reply audio arrived; local prompt yields')
if ($graceBlockStart -lt 0 -or $yieldBlockStart -lt 0 -or $yieldBlockStart -le $graceBlockStart) {
    throw 'The grace branch boundaries are missing.'
}
if ($voice.Substring($graceBlockStart, $yieldBlockStart - $graceBlockStart) -match
    's_pending_cloud_tts_after_prompt = false') {
    throw 'The grace path must leave the deferred cloud reply armed for the natural finish to resume.'
}
# 剩余时长必须由 PCM 字节数换算，不能拿 3 秒看门狗当音频结束点——两者差三倍，
# 拿看门狗算会让每个到达都落进宽限，等于永远不提前让位。
if ($voice -notmatch 's_local_prompt_end_us =[\s\S]{0,200}?XIAOZHI_LOCAL_PROMPT_BYTES_PER_MS') {
    throw 'The remaining-audio estimate must come from the PCM byte count, not the watchdog deadline.'
}
$clearSites = ([regex]::Matches($voice, 's_local_prompt_deadline_us = 0;[\s\S]{0,80}?s_local_prompt_end_us = 0;')).Count
if ($clearSites -lt 5) {
    throw "Every prompt teardown (close, greeting failure, yield, finish, watchdog) must clear the estimate: found $clearSites of 5."
}

# 5d. 语速：三个意图提示音必须和开机问候/退出语分开跑。它们只有四个音节却要和
#     云端争那 1.4 s，所以自成更快的快档；其余资产维持在调好的 +18%。
if ($generator -notmatch 'INTENT_RATE = "\+35%"') {
    throw 'The three intent confirmations need their own, faster rate constant.'
}
if ($generator -notmatch 'INTENT_RATE if name in INTENT_ASSETS else TTS_RATE') {
    throw 'The generator must pick the faster rate for the intent assets only, leaving the greeting at TTS_RATE.'
}
if ($generator -notmatch 'def _edge_tts_pcm\(text, rate\)') {
    throw 'The TTS engine call must take the per-asset rate, not read the module-wide constant.'
}

# 6. The three PCM assets must be embedded so the device-image symbols resolve.
foreach ($name in @(
        'assets/audio/prompt_executing_16k_mono_s16le.pcm',
        'assets/audio/prompt_querying_16k_mono_s16le.pcm',
        'assets/audio/prompt_thinking_16k_mono_s16le.pcm')) {
    if ($cmake -notmatch ([regex]::Escape($name))) {
        throw "CMakeLists.txt must embed $name."
    }
}

# 7. The generated assets must exist as 16 kHz mono s16le: a non-zero byte count
#    divisible by 2 (samples) that is a plausible short phrase (< 2 s).
foreach ($name in @(
        'prompt_executing_16k_mono_s16le.pcm',
        'prompt_querying_16k_mono_s16le.pcm',
        'prompt_thinking_16k_mono_s16le.pcm')) {
    $path = Join-Path $PSScriptRoot "..\main\assets\audio\$name"
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Prompt asset is missing: $name"
    }
    $len = (Get-Item -LiteralPath $path).Length
    if ($len -le 0) {
        throw "Prompt asset is empty: $name"
    }
    if (($len % 2) -ne 0) {
        throw "Prompt asset is not 16-bit aligned s16le: $name"
    }
    # 16 kHz mono -> 32000 bytes per second. 提示音必须在云端首个音频包到达前就放完，
    # 否则只能被硬切。实测云端平均 1.39 s（中位数 1.07 s），扣掉 250 ms 宽限窗口后，
    # 本体不应超过 900 ms——这是它存在的意义，不是随便定的上限。
    # 重新生成：python scripts/generate_prompt_tones.py
    if ($len -ge (32000 * 0.9)) {
        throw "Prompt asset is $([int]($len / 32)) ms, over the 900 ms ceiling; the cloud's reply will hard-cut it. Re-run scripts/generate_prompt_tones.py: $name"
    }
}

Write-Output 'Voice immediate-prompt contract passed.'
