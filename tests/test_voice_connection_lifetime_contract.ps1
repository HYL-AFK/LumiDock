$ErrorActionPreference = 'Stop'
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

if ($voice -notmatch 's_wss_connection_seq') {
    throw 'WSS diagnostics must identify each client connection instance.'
}
if ($voice -notmatch 's_state == XIAOZHI_VOICE_LISTENING') {
    throw 'The voice service must identify an active listening turn when transport drops.'
}
if ($voice -notmatch 'WSS callback event_id=%ld connection_seq=%u.*terminal_gen=%u') {
    throw 'WSS callback diagnostics must include raw event id, connection sequence, and terminal generation.'
}
# 但这条日志不能每个事件都打：WEBSOCKET_EVENT_DATA 在 TTS 期间每 40~90 ms 就来一次，
# 实测 247 行的日志里它独占 52 行（21%），把 TTS/电台的关键行全淹了。
# 现在只有“不是 DATA 事件”或“五元组变了”才输出。
if ($voice -notmatch 'if \(event_id != WEBSOCKET_EVENT_DATA \|\| state_changed\)') {
    throw 'The WSS callback trace must stay silent for steady-state DATA events.'
}
if ($voice -notmatch 'uplink_operation_equal') {
    throw 'The op name must be compared by content; string literal addresses are not guaranteed unique.'
}
if ($voice -match 'failed_uplink_packet_len|recovered failed Opus packet|preserve_session_for_wss_reconnect') {
    throw 'A failed Opus packet must not cross a WSS reconnect.'
}
# "已启动但已断开"的陈旧客户端必须在新的唤醒会话前被替换掉。而"还没握完手"和
# "握手早就卡死了"在 is_connected() 上无法区分——半开连接可以一直返回 true，于是
# 每次唤醒都复用一条等不到 hello 的死链路。所以除了状态判定还必须有时间兜底。
if ($voice -notmatch 'replacing stale started WSS client' -or $voice -notmatch 'handshake_stale') {
    throw 'A stale started client must be replaced before a new wake session, including one whose handshake has been stuck past its timeout.'
}
# 抢不到 tx 锁时绝不能让 s_client 留在原地：紧接着的 esp_websocket_client_init()
# 会覆盖这个句柄，旧的 client 连同 task/socket/TLS 全部泄漏；内部 RAM 本来就紧，
# LWIP_MAX_SOCKETS 又只有 10，漏几次之后新连接一律建不起来，表现就是"断连之后再
# 怎么唤醒都失败、断电重启才好"。必须交给 websocket 任务退出时自销毁。
if ($voice -notmatch '(?s)esp_websocket_client_destroy_on_exit\(s_client\);[\s\S]{0,120}?s_client = NULL;') {
    throw 'A client that cannot be stopped under the tx lock must be handed to destroy-on-exit instead of being leaked.'
}
# 上行失败必须收场。注意这里锁的是"失败 → 收场"的因果，不是某句具体措辞：
# 代码把云端兜底改成了本地错误音（链路已标记 FAILED，再等云端只会让设备停住），
# 契约要跟着行为走，不能反过来把已经废弃的措辞当成必须存在的证据。
if ($voice -notmatch 'voice turn transport failure') {
    throw 'A transport failure must be logged, or the session ends silently.'
}
if ($voice -notmatch '(?s)voice turn transport failure; no cloud fallback, entering bounded cleanup[\s\S]{0,300}?play_error_tone_then_close') {
    throw 'A failed listening session must end cleanly and require a new wake word.'
}
Write-Output 'Voice connection lifetime contract passed.'
