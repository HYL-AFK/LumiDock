$ErrorActionPreference = 'Stop'
$root = Join-Path $PSScriptRoot '..'
$voice = [System.IO.File]::ReadAllText((Join-Path $root 'main\MID\voice\xiaozhi_voice_service.c'), [System.Text.Encoding]::UTF8)
$generator = [System.IO.File]::ReadAllText((Join-Path $root 'scripts\generate_prompt_tones.py'), [System.Text.Encoding]::UTF8)
$cmake = [System.IO.File]::ReadAllText((Join-Path $root 'main\CMakeLists.txt'), [System.Text.Encoding]::UTF8)

# The farewell used to be a cloud [device_call] TTS request.  The official
# xiaozhi.me server rejects that prefix, so the wording is frozen into an
# embedded PCM asset; the spoken copy now lives in the generator that renders
# that asset, not in the C source.
if ($generator.IndexOf('那我们下次再聊，祝你顺利。') -lt 0) {
    throw 'Missing fixed exit phrase in the tone generator.'
}
if ($generator.IndexOf('prompt_exit_16k_mono_s16le.pcm') -lt 0) {
    throw 'The tone generator must own the exit asset.'
}
if ($cmake.IndexOf('assets/audio/prompt_exit_16k_mono_s16le.pcm') -lt 0) {
    throw 'The exit asset must be embedded into the firmware image.'
}
if ($voice.IndexOf('_binary_prompt_exit_16k_mono_s16le_pcm_start') -lt 0) {
    throw 'The voice service must reference the embedded exit asset.'
}
if ($voice -notmatch 'play_local_prompt_pcm\(LOCAL_PROMPT_EXIT, prompt_exit_pcm_start,') {
    throw 'The exit prompt must play the embedded PCM asset.'
}
if ($voice -match '"\[device_call\]') {
    throw 'The voice service must not build a [device_call] direct-TTS request any more.'
}
if ($voice -match '请自然地说一句简短的告别语') {
    throw 'Exit prompt must not ask the cloud to freely generate a farewell.'
}
foreach ($marker in @('exit_tts_start_deadline_us', 'exit_tts_first_packet_deadline_us', 'exit_tts_playback_deadline_us')) {
    if ($voice -notmatch $marker) { throw "Missing independent exit deadline: $marker" }
}
if ($voice -notmatch 'exit prompt PCM drained; closing voice session') {
    throw 'Exit session must close only after PCM drain.'
}
Write-Output 'Fixed exit prompt contract passed.'
