$ErrorActionPreference = 'Stop'

$svc = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\timer_service.c"
$hdr = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\timer_service.h"

# Slot limits: exactly one one-shot alarm and one countdown.  A second timer of
# the same kind replaces the pending one instead of being rejected.
if ($hdr -notmatch 'TIMER_SERVICE_MAX_ALARMS 1U' -or
    $hdr -notmatch 'TIMER_SERVICE_MAX_COUNTDOWNS 1U') {
    throw 'Timer slot limits must be 1 alarm + 1 countdown.'
}
if ($hdr -match 'TIMER_SERVICE_SLOT_COUNTDOWN1') {
    throw 'The second countdown slot must be gone so only one countdown can exist.'
}

# Ring behavior: 2 s chime heartbeat, 30 s auto-stop.
if ($hdr -notmatch 'TIMER_SERVICE_RING_HEARTBEAT_MS 2000U' -or
    $hdr -notmatch 'TIMER_SERVICE_RING_TIMEOUT_MS 30000U') {
    throw 'Ring heartbeat (2 s) and auto-stop (30 s) must be present.'
}
# 倒计时不再有落盘节流常量——它根本不落盘。
if ($hdr -match 'TIMER_SERVICE_COUNTDOWN_PERSIST_TICKS') {
    throw 'Countdown progress must not be flushed to NVS at all, so the wear-limiting constant has to go.'
}

# 持久化的范围只有闹钟——它是唯一必须跨下电存活的定时器。倒计时和正计时都是内存态。
if ($svc -notmatch 'TIMER_NVS_NAMESPACE "lumidock_timer"' -or
    $svc -notmatch 'TIMER_NVS_KEY_ALARMS "alarms"' -or
    $svc -notmatch 'nvs_set_blob' -or
    $svc -notmatch 'nvs_get_blob') {
    throw 'The alarm must persist through NVS blobs in a dedicated namespace.'
}
if ($svc -match 'nvs_set_blob\(handle, TIMER_NVS_KEY_LEGACY') {
    throw 'Only the alarm may be written; the countdown blob must never come back.'
}
# 旧固件把倒计时也写进过 NVS。开机清一次，否则那个没人读的键会一直占着。
if ($svc -notmatch 'nvs_erase_key\(handle, TIMER_NVS_KEY_LEGACY_COUNTDOWNS\)') {
    throw 'The countdown blob written by older firmware must be dropped once at boot.'
}

# The stopwatch must be in-memory only: no stopwatch state may reach NVS.
if ($svc -notmatch 'Stopwatch: in-memory only, resets on power loss') {
    throw 'The stopwatch must be documented as in-memory only.'
}
$persistAlarmStart = $svc.IndexOf('static esp_err_t persist_alarms')
$loadStart = $svc.IndexOf('static void load_persisted_state')
if ($persistAlarmStart -lt 0 -or $loadStart -lt 0) {
    throw 'Persistence function boundaries are missing.'
}
$persistBlock = $svc.Substring($persistAlarmStart, $loadStart - $persistAlarmStart)
if ($persistBlock -match 'stopwatch') {
    throw 'Stopwatch state must never be persisted to NVS.'
}
if ($persistBlock -match 's_countdowns') {
    throw 'No countdown state may pass through the persistence helpers.'
}

# One-shot semantics: ringing deletes the slot immediately and persists.
$ringStart = $svc.IndexOf('static void start_ringing')
$pollStart = $svc.IndexOf('void timer_service_poll')
if ($ringStart -lt 0 -or $pollStart -lt 0) { throw 'start_ringing/poll boundaries are missing.' }
$ringBlock = $svc.Substring($ringStart, $pollStart - $ringStart)
if ($ringBlock -notmatch 'Delete one-shot sources immediately: the alarm/countdown is consumed') {
    throw 'A ringing timer must delete its one-shot source immediately.'
}
if ($ringBlock -notmatch 's_alarms\[slot - TIMER_SERVICE_SLOT_ALARM0\]\.valid = false' -or
    $ringBlock -notmatch 'persist_alarms\(\);' -or
    $ringBlock -notmatch 's_countdowns\[slot - TIMER_SERVICE_SLOT_COUNTDOWN0\]\.valid = false') {
    throw 'Ringing must clear the consumed slot and flush the alarm to NVS.'
}
# 倒计时响完只清内存：它没有落盘路径，混进来一个 persist 调用就说明又持久化了。
if ($ringBlock -match 'persist_countdowns') {
    throw 'A spent countdown must not be persisted; it is memory-only.'
}

# One chime at a time: a second expiry queues behind the current ring.
if ($ringBlock -notmatch 's_ring_pending = true' -or
    $ringBlock -notmatch 's_pending_slot = slot' -or
    $ringBlock -notmatch 'One chime at a time: queue a second expiry while the first rings') {
    throw 'A second timer expiry must queue behind the current ring instead of overlapping audio.'
}

# Ring heartbeat and 30 s auto-stop inside poll.
$pollBlock = $svc.Substring($pollStart)
if ($pollBlock -notmatch 'TIMER_SERVICE_RING_TIMEOUT_MS \* 1000LL' -or
    $pollBlock -notmatch 'ring auto-stopped') {
    throw 'poll must auto-stop the ring after the 30 s timeout.'
}
if ($pollBlock -notmatch 'TIMER_SERVICE_RING_HEARTBEAT_MS \* 1000LL' -or
    $pollBlock -notmatch 'notify_event\(TIMER_SERVICE_EVENT_RING, s_ringing_slot\);') {
    throw 'poll must emit a RING heartbeat every 2 s while ringing.'
}
if ($pollBlock -notmatch 'if \(!s_ringing && s_ring_pending\) \{') {
    throw 'A queued ring must start as soon as the current ring ends (auto-stop handoff).'
}

# Countdown ticks use real wall-clock elapsed seconds.
if ($pollBlock -notmatch 'Real elapsed seconds since the previous poll' -or
    $pollBlock -notmatch 'elapsed = now - s_last_poll_time' -or
    $pollBlock -notmatch 'remaining_seconds -= \(int32_t\)elapsed') {
    throw 'Countdown progress must follow real elapsed time, not fixed 1 s decrements.'
}

# 倒计时进度不再产生 NVS 磨损：tick 循环只动内存。
if ($pollBlock -match 'persist_countdowns|s_persist_tick') {
    throw 'Countdown progress must stay in memory; a power cut is allowed to lose it.'
}

# User stop clears both the current ring and any queued ring.
$stopStart = $svc.IndexOf('void timer_service_stop_ringing')
$stopBlock = $svc.Substring($stopStart, $pollStart - $stopStart)
if ($stopBlock -notmatch 's_ringing = false' -or
    $stopBlock -notmatch 's_ring_pending = false' -or
    $stopBlock -notmatch 'notify_event\(TIMER_SERVICE_EVENT_STOPPED, s_ringing_slot\);') {
    throw 'User stop must clear ringing + pending and emit a STOPPED event.'
}

# Alarm clock gate: needs a synchronized SNTP clock and rolls to tomorrow.
$alarmStart = $svc.IndexOf('timer_service_cmd_result_t timer_service_set_alarm')
$countStart = $svc.IndexOf('timer_service_cmd_result_t timer_service_set_countdown')
$alarmBlock = $svc.Substring($alarmStart, $countStart - $alarmStart)
if ($alarmBlock -notmatch 'timer_service_is_time_synced' -or
    $alarmBlock -notmatch 'mktime\(&local\)' -or
    $alarmBlock -notmatch 'target \+= 24 \* 3600' -or
    $alarmBlock -notmatch 'alarm replaced: slot=%u previous_target=%lld') {
    throw 'Alarm set must require a synced clock, roll to tomorrow when passed, and replace the pending alarm.'
}
if ($svc -notmatch 'time\(NULL\) > TIMER_TIME_SYNCED_EPOCH') {
    throw 'The synced-clock gate must compare against the SNTP convergence epoch.'
}

# Restore: only the alarm comes back after a power cut.
if ($svc -notmatch 'alarm restored: slot=' -or
    $svc -notmatch 'load_persisted_state\(\);') {
    throw 'init must restore the persisted alarm.'
}
if ($svc -match 'countdown restored: slot=') {
    throw 'A countdown must never be restored after a power cut.'
}
if ($svc -notmatch 'timer_load_task' -or
    $svc -notmatch 'MALLOC_CAP_INTERNAL \| MALLOC_CAP_8BIT' -or
    $svc -notmatch 'timer_load_done') {
    throw 'Persisted timer state must be loaded on an internal-RAM stack task.'
}

# Single-countdown semantics: both the direct and the voice path replace the
# previous countdown instead of rejecting the request.
$prepareStart = $svc.IndexOf('timer_service_cmd_result_t timer_service_prepare_countdown')
$startPendingStart = $svc.IndexOf('timer_service_cmd_result_t timer_service_start_pending_countdowns')
if ($countStart -lt 0 -or $prepareStart -lt 0 -or $startPendingStart -lt 0) {
    throw 'Countdown setter boundaries are missing.'
}
$countBlock = $svc.Substring($countStart, $prepareStart - $countStart)
if ($countBlock -notmatch 'countdown replaced: slot=%u previous=%ld') {
    throw 'Setting a countdown must replace the previous countdown instead of rejecting it.'
}
$prepareBlock = $svc.Substring($prepareStart, $startPendingStart - $prepareStart)
if ($prepareBlock -notmatch 'countdown replaced: slot=%u previous=%ld') {
    throw 'A voice countdown must replace the previous countdown instead of rejecting it.'
}

# A voice countdown waiting for its acknowledgement TTS must already be visible
# to the main page so the countdown covers any pending alarm immediately.
if ($svc -notmatch 's_pending_countdowns\[i\]\.valid && s_pending_countdowns\[i\]\.remaining_seconds > 0[\s\S]*?out->countdown_active = true;') {
    throw 'A pending voice countdown must be reported as active in the timer snapshot.'
}

Write-Output 'Timer service contract passed.'
