$ErrorActionPreference = 'Stop'
$driver = Get-Content -Raw "$PSScriptRoot\..\main\Dri\i2s_audio_driver.c"
$header = Get-Content -Raw "$PSScriptRoot\..\main\Dri\i2s_audio_driver.h"
$iot = Get-Content -Raw "$PSScriptRoot\..\main\MID\system\voice_iot_command_service.c"
$radio = Get-Content -Raw "$PSScriptRoot\..\main\MID\system\radio_stream_service.c"
$play = Get-Content -Raw "$PSScriptRoot\..\main\MID\system\audio_playback_service.h"
$cmake = Get-Content -Raw "$PSScriptRoot\..\main\CMakeLists.txt"
$sdkconfig = Get-Content -Raw "$PSScriptRoot\..\sdkconfig"
$sdkconfigDefaults = Get-Content -Raw "$PSScriptRoot\..\sdkconfig.defaults"
if ($header -notmatch 'i2s_audio_driver_set_output_volume') { throw 'missing volume API' }
if ($driver -notmatch 'esp_codec_dev_set_out_vol') { throw 'volume must use codec API' }
if ($iot -notmatch 'volume') { throw 'voice volume commands missing' }
if ($iot -notmatch 'radio') { throw 'voice radio commands missing' }
$iot_hex = [BitConverter]::ToString([IO.File]::ReadAllBytes((Join-Path $PSScriptRoot '..\main\MID\system\voice_iot_command_service.c')))
foreach ($phraseHex in @('E5-A4-A7-E9-9F-B3-E9-87-8F','E5-B0-8F-E9-9F-B3-E9-87-8F','E6-94-BE-E7-94-B5-E5-8F-B0')) {
    if (!$iot_hex.Contains($phraseHex)) { throw "missing measured command phrase bytes: $phraseHex" }
}
if ($radio -notmatch 'esp_http_client|esp_audio_simple_dec_process') { throw 'radio must use HTTP and MP3 decoder' }
if ($play -notmatch 'AUDIO_PLAYOUT_SOURCE_RADIO') { throw 'radio playback source missing' }
if ($cmake -notmatch 'radio_stream_service.c') { throw 'radio service not compiled' }
# 电台开播门槛。实测 12 帧（≈290 ms）时开播后 580 ms 就 underrun（日志 43440
# 开播 queued=11，44020 饿死）：服务端按实时供流、播放也按实时消耗，开播那一刻
# 的水位就是整轮唯一的抖动缓冲。
if ($radio -notmatch '#define RADIO_PREBUFFER_FRAMES 40U') {
    throw 'The radio must buffer about one second before opening, or the first seconds stutter.'
}
# 拉流限速：分片是整片下载的，不压回内容码率就会瞬间灌满 WiFi 接收路径，逼着驱动
# 去要一大批只能落在内部 DMA 内存里的网络缓冲（eb）。拿不到就打 "wifi: m f null"
# 丢包，表现成电台一哑十几秒（日志 48687→58237 有 9.5 秒没任何 PCM 入队）。
if ($radio -notmatch '#define RADIO_HLS_TARGET_BYTES_PER_SEC 24576U') {
    throw 'The HLS segment download must be throttled back to the content bitrate.'
}
if ($radio -notmatch 'hls_throttle\(&hls, \(size_t\)n, epoch, armed\);') {
    throw 'The fill loop must throttle each read; otherwise the RX burst is unchanged.'
}
# WiFi 接收侧长期被削到底（静态 3 / 动态 6），而发送侧从默认的 16 个一个没削过。
# RX_BA_WIN 的运行时上限是 (静态+动态)/2 = 4，所以 4 是不加缓冲数时能取到的最大值；
# 再想加接收缓冲就得从内部 DMA 内存里抠，是反方向。所以改守发送侧：电台是纯下行，
# 上行只有 TCP ACK 和几十字节的 Opus 包，8 个静态 TX 缓冲（加 32 深的 cache 队列）
# 就够——省下的 8×1.6 KB 内部 DMA 内存正是驱动分配接收侧网络缓冲的地方。
# 两个文件都要守：defaults 是重新生成 sdkconfig 时的来源，只改一处会在下次
# 重新生成时静默回退。
foreach ($config in @($sdkconfig, $sdkconfigDefaults)) {
    if ($config -notmatch '(?m)^CONFIG_ESP_WIFI_RX_BA_WIN=4\s*$') {
        throw 'The WiFi AMPDU RX window must sit at its ceiling; the RX path is the radio bottleneck.'
    }
    if ($config -notmatch '(?m)^CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=8\s*$') {
        throw 'The radio is a pure downlink; the TX pool must give its internal DMA RAM back to RX.'
    }
    # HLS 分片是 TS 容器装 AAC：一个是容器解析器、一个是解码器，两个开关都在
    # menuconfig 里。缺任何一个，电台都会连得上、列表也解析通过，然后一片都解不出声。
    foreach ($switch in @('CONFIG_AUDIO_DECODER_AAC_SUPPORT', 'CONFIG_AUDIO_SIMPLE_DEC_TS_SUPPORT')) {
        if ($config -notmatch "(?m)^$switch=y\s*$") {
            throw "$switch must be compiled in: HLS puts AAC inside a TS container."
        }
    }
}
if ($radio -notmatch 'RADIO_MAX_RECONNECTS') { throw 'radio reconnect bound missing' }
# 建连阶段（DNS/TCP 握手、拉播放列表、开解码器、抢扬声器）失败也必须重试。
# 实测日志里一次 getaddrinfo() 202（EAI_AGAIN，DNS 临时故障）就让电台直接回 IDLE，
# 用户看到的是“电台自己关了、屏幕退回主页面”。断流有重连兜底，建连以前是一次判死的。
if ($radio -notmatch '#define RADIO_MAX_CONNECT_RETRIES') {
    throw 'Connect-stage failures (DNS/TCP/playlist/decoder/speaker) need their own retry bound instead of a single-shot failure.'
}
# 重试计数必须活在会话循环之外：建连失败走的是 continue，回到 for(;;) 顶部会把循环内
# 变量重新声明一遍，放在里面就等于永远归零，上限形同虚设。
$radioTaskPos = $radio.IndexOf('static void radio_task(')
$connectRetryPos = $radio.IndexOf('uint8_t connect_retries = 0U;', $radioTaskPos)
$sessionLoopPos = $radio.IndexOf('for (;;) {', $connectRetryPos)
if ($radioTaskPos -lt 0 -or $connectRetryPos -lt 0 -or $sessionLoopPos -lt 0 -or
    $connectRetryPos -gt $sessionLoopPos) {
    throw 'The connect-stage retry counter must be declared outside the session loop, otherwise every continue resets it.'
}
# 每个建连失败点都要先退避重试再 continue，绝不能直接落到清 s_play_requested + 回 IDLE。
# 退避从定值 500 ms 换成了 retry_backoff_ms()：出过声的流现在是无限重连的，固定间隔会在
# 源长期不可用时退化成每几百毫秒一轮的 DNS+TCP 风暴。断言的意图没变——先等，再重试。
$connectRetryGuards = [regex]::Matches($radio, 'connect_retries < RADIO_MAX_CONNECT_RETRIES \|\| s_played_audio')
if ($connectRetryGuards.Count -lt 4) {
    throw "Every connect-stage failure point (http/playlist/decoder/speaker) needs the retry guard; found $($connectRetryGuards.Count)."
}
if ($radio -notmatch '(?s)connect_retries < RADIO_MAX_CONNECT_RETRIES \|\| s_played_audio[\s\S]{0,400}?vTaskDelay\(pdMS_TO_TICKS\(retry_backoff_ms\(connect_retries\)\)\);[\s\S]{0,80}?continue;') {
    throw 'Connect-stage failures must back off and retry instead of falling straight through to IDLE.'
}
# 重试期间必须保留 s_play_requested：它同时是 UI 判断“还在电台模式”的唯一标志
# （ui_service.c 里 !radio_stream_service_is_requested() 会立刻翻回主页面），清掉等于
# 告诉用户这一轮结束了——这正是“电台断开后直接退回主页面”的根因。
if ($radio -notmatch '(?s)connect_retries < RADIO_MAX_CONNECT_RETRIES \|\| s_played_audio[\s\S]{0,400}?continue;') {
    throw 'A connect-stage retry must continue without dropping the radio play request.'
}
# 出过声之后不再放弃。电台是用户在听的直播，网络抖动是他的常态；三次重连就用完、
# record_failure + 清 s_play_requested 的代价实测就是“放着放着电台自己关了、屏幕退回
# 主页面”。上限只留给“刚说打开电台、一次都没响过”那种——它要尽快把失败报给语音侧。
if ($radio -notmatch 'static bool s_played_audio;') {
    throw 'The radio must track whether this listening session ever produced sound.'
}
if ($radio -notmatch '(?s)static bool stream_retry_after_stall\(uint8_t \*reconnects\)[\s\S]{0,200}?if \(\*reconnects < RADIO_MAX_RECONNECTS \|\| s_played_audio\)') {
    throw 'A stream stall must keep retrying once the radio has been heard.'
}
# 抢到扬声器＝这次收听算“出过声”。放在这里而不是“第一块 PCM 入队成功”，是为了也覆盖
# “刚出声就断”的那一档。
if ($radio -notmatch '(?s)armed = true;[\s\S]{0,400}?s_played_audio = true;') {
    throw 'Taking the speaker is what marks the session as audible; the flag must be set there.'
}
# 用户 stop() 是这一段收听真正的终点：预算必须重新记，否则下一次“打开电台”遇到永远连不
# 上的源会一直重连，失败永远报不到语音侧。
if ($radio -notmatch '(?s)radio_stream_service_stop\(void\)[\s\S]{0,600}?s_played_audio = false;') {
    throw 'Stopping the radio must re-arm the retry budget for the next listening session.'
}
# 直播边缘重拉列表失败不能判死。站在边缘本身就说明“新片还没生成”，而重拉走的是一次
# 全新的短超时（2.5 s）建连，网络抖一下就会失败；实测两次 http_read_eof 都是这么触发的
# （边缘只等了 3.3 s 和 7 s，离 30 s 上限差得远），代价是几十秒静音。
if ($radio -notmatch 'HLS 直播边缘重拉列表失败；继续等新片') {
    throw 'A failed playlist reload at the live edge must wait for the next round instead of killing the stream.'
}
if ($radio -match '(?s)HLS 直播边缘重拉列表失败[\s\S]{0,300}?record_failure') {
    throw 'A failed playlist reload at the live edge must not be recorded as a dead stream.'
}
if ($radio -notmatch 'audio_playback_service_abort') { throw 'radio failure cleanup missing' }
if ($radio -notmatch 'radio_stream_service_consume_failure') { throw 'radio failure status API missing' }

# 电台曾经在输入缓冲满时直接 break：既不重同步也不上报，实机表现就是
# “没有声音、也没有任何报错”。下面几条把这个坑钉住。
if ($radio -notmatch 'record_failure\("parser_stall"') {
    throw 'An MP3 parser stall must be reported, not silently abandoned.'
}
if ($radio -notmatch 'dropped %u bytes to resync') {
    throw 'A full input buffer with no progress must resync instead of killing the stream.'
}
if ($radio -notmatch 'esp_audio_simple_dec_get_info') {
    throw 'The radio must read the decoded format before handing PCM to the 16 kHz I2S.'
}
if ($radio -notmatch 'radio_to_16k_mono') {
    throw 'The decoded stream must be converted to the 16 kHz mono I2S format.'
}
# 下混必须原地做：输出缓冲复用 pcm，不再单独开一块。立体声→单声道、
# 44.1/48 kHz→16 kHz 合起来是缩小约 6 倍，写指针永远追不上读指针（而且读语句
# 在写语句之前求值，第一帧读写同一位置也安全）；而多开一块独立输出缓冲就是白占
# 8 KB PSRAM。
if ($radio -notmatch '(?s)radio_to_16k_mono\(pcm, out\.decoded_size,[\s\S]{0,200}?info\.channel, info\.bits_per_sample, pcm,') {
    throw 'The 16 kHz mono conversion must run in place over the decode buffer instead of carrying a second 8 KB output buffer.'
}
if ($radio -notmatch 'enqueue_pcm_chunked') {
    throw 'Radio PCM must be enqueued in chunks; one MP3 frame exceeds the playout input limit.'
}
if ($radio -notmatch 'RADIO_ENQUEUE_CHUNK_BYTES 2048U') {
    throw 'The radio enqueue chunk must stay at the AUDIO_PLAYOUT_PCM_INPUT_MAX_BYTES limit.'
}
# decoded_size == 0 且返回 OK 是解析器在缓存数据（esp_audio_simple_dec.h 明确
# 写了这条语义），不是解码失败；否则流开头几次空返回就把整条流判死。
if ($radio -notmatch '(?s)decoded_size == 0[\s\S]{0,240}?decode_failures = 0U;') {
    throw 'decoded_size == 0 with ESP_AUDIO_ERR_OK must be treated as a normal intermediate state.'
}

# ---------------------------------------------------- 电台与语音共存
# 让位（语音占用扬声器）与用户暂停是两回事：让位把电台整段让出去（停拉流、停解码、
# 关 HTTP），用户暂停是静音保活（继续拉流解码、只丢 PCM）。这样语音回合里电台就
# 不再和它抢 socket、WiFi 接收缓冲和 core 1 上的解码时间。
if ($radio -notmatch 'static bool s_yielded;' -or $radio -notmatch 'static bool s_user_paused;') {
    throw 'The radio needs separate yield and pause states.'
}
if ($radio -notmatch 'radio_stream_service_yield\(' -or
    $radio -notmatch 'radio_stream_service_regain\(') {
    throw 'The radio must expose yield()/regain() so the voice side can borrow the speaker.'
}
# 让位必须真的退出会话循环：关掉 HTTP 与解码器，把资源让给语音链路。
if ($radio -notmatch 'if \(yielded\) \{ parked = true; break; \}') {
    throw 'Yielding must leave the streaming loop instead of only muting the output.'
}
if ($radio -notmatch 'if \(!requested \|\| yield_parked\) \{') {
    throw 'A parked radio must not reconnect the stream until regain().'
}
# 让位不等于终止：s_play_requested 必须保持为真，答完才能自己接回直播。
if ($radio -match '(?s)if \(parked\) \{[\s\S]{0,400}?s_play_requested = false;') {
    throw 'Parking for a voice turn must keep s_play_requested so the radio comes back.'
}
# 让位解除要立刻唤醒待机中的任务，不能等 100 ms 轮询周期。
if ($radio -notmatch '(?s)radio_stream_service_regain\(void\)[\s\S]{0,500}?xTaskNotifyGive\(s_task\);') {
    throw 'regain() must wake the parked radio task immediately.'
}
# 用户暂停仍是静音保活：继续拉流解码，只把 PCM 丢掉。
if ($radio -notmatch 'const bool muted = yielded \|\| s_user_paused;') {
    throw 'A paused radio must keep decoding and drop PCM instead of stopping.'
}
# 入队失败必须透传真实错误码：INVALID_ARG 是通道易主（被语音抢了）＝让位，
# TIMEOUT 才是队列满了。旧实现一律写成 ESP_ERR_TIMEOUT，于是日志里把“被让位”
# 报成了电台故障 code=263，看起来像电台自己坏了。
if ($radio -notmatch 'record_failure\("pcm_enqueue", enq, s_reconnects\)') {
    throw 'The radio must report the real enqueue error code, not a hardcoded timeout.'
}
# 窗口放宽到 360：断言的本意是“这两件事在同一条分支上且相邻”，不是精确字符距离。
# 中间那段解释“为什么让位不是故障”的注释会随改动变长，200 早已装不下（实测 247）。
if ($radio -notmatch '(?s)enq == ESP_ERR_INVALID_ARG[\s\S]{0,360}?s_yielded = true;') {
    throw 'A preempted radio must switch to yielded instead of being recorded as a failure.'
}
# 队列满＝回放端跟不上直播，是背压不是故障。把它和真错误合并处理的话，一次积压
# 就会把整个电台会话拆掉重连（用户听到的是断流卡顿）。
if ($radio -notmatch '(?s)enq == ESP_ERR_TIMEOUT[\s\S]{0,400}?queue_full_rounds') {
    throw 'A full playout queue must be treated as backpressure, not as a radio failure.'
}
if ($radio -notmatch 'RADIO_MAX_QUEUE_FULL_ROUNDS 8U') {
    throw 'Continuous queue pressure must still escalate to a failure after a bounded run.'
}
# 暂停是静音保活：不能再打断会话循环，否则“继续播放”只能重连（要重新缓冲）。
if ($radio -match 'const bool keep = s_play_requested && !s_paused') {
    throw 'Pause must not break the stream loop: it is a mute, not a stop.'
}

$voice = Get-Content -Raw "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$pipeline = Get-Content -Raw "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"
# TTS 抢扬声器时只能让位，不能 stop：旧实现每答一句就把整条电台终止掉。
if ($pipeline -match 'radio_stream_service_stop\(') {
    throw 'TTS playback must yield the radio instead of stopping it.'
}
if ($pipeline -notmatch 'radio_stream_service_yield\(\)') {
    throw 'TTS playback must yield the radio before taking the speaker.'
}
# 唤醒即让位：这一步之后麦克风环境才干净（AEC 关闭，电台外放必然被自己收进去）。
if ($voice -notmatch '(?s)radio_stream_service_yield\(\);[\s\S]{0,200}?begin_channel\(\);') {
    throw 'The wake handler must yield the radio before opening the microphone.'
}
# 打断正在播的电台时，答完必须先把 6.4 s 听音窗口留满再归还扬声器。窗口期间电台
# 保持让位（停拉流、关解码），麦克风收到的是安静的房间，所以不存在旧版那种
# "对着麦克风放电台"的自激上行；旧实现是答完立即 regain，把窗口整个吃掉了。
# 这里必须用 is_requested 而不是 is_active：后者在"电台已 regain、还在重连尚未
# 出声"时为假，会让那一刻唤醒的对话答完立刻把扬声器交回去。
if ($voice -notmatch 's_radio_on_before_turn = radio_stream_service_is_requested\(\);') {
    throw 'The wake handler must remember whether the radio was already in radio mode.'
}
# 上面那份标记会被"本轮启动电台"的路径清掉，而换台走的正是同一条路径（换一个台
# 也是重新起流，只是从下一格开始）。所以判定"要不要留听音窗口"必须另用一份
# 当场拍下、整轮不变的副本：少了它，一句"换个台"就会被当成"本轮才开的电台"，
# close_channel() 立刻收场，用户看到的是"说一句换台，对话直接断了"。
# 判定"收场停不停电台"继续用 s_radio_on_before_turn，那是用户拍板的规则，两者不能合并。
if ($voice -notmatch 's_radio_requested_before_turn = s_radio_on_before_turn;') {
    throw 'The wake handler must snapshot the pre-turn radio state into a copy the turn itself cannot rewrite.'
}
# 反向断言：本轮的电台启动路径只许清 s_radio_on_before_turn，绝不能顺手把这份副本
# 也清掉——那正是上面要防的回归。
if ($voice -match '(?s)mcp_service_clear_pending_radio\(\);[\s\S]{0,200}?s_radio_requested_before_turn\s*=\s*false') {
    throw 'A radio start path must not clear the pre-turn snapshot, or a plain station switch tears the conversation down.'
}
if ($voice -notmatch '(?s)if \(s_radio_requested_before_turn\)\s*\{[\s\S]{0,400}?s_state = XIAOZHI_VOICE_WAIT_FOLLOWUP;') {
    throw 'Interrupting a playing radio must arm the follow-up window before handing the speaker back.'
}
# 窗口期间电台必须一直让位：这一段里不能出现 regain，否则窗口形同虚设。
if ($voice -match '(?s)if \(s_radio_requested_before_turn\)\s*\{[\s\S]{0,600}?radio_stream_service_regain\(\);') {
    throw 'The radio must stay parked for the whole follow-up window instead of being handed back early.'
}
# 电台是这一轮才被拉起来的（用户说"开电台"）时不能压着窗口，必须立即归还。
# 窗口放宽到 900：断言的本意是“else 分支里立即 close_channel 并 return”，不是精确距离。
# 中间解释“为什么打断电台要留满听音窗口”的注释会随改动变长，500 早已装不下（实测 592）。
if ($voice -notmatch '(?s)if \(s_radio_requested_before_turn\)[\s\S]{0,900}?\}\s*else\s*\{[\s\S]{0,400}?close_channel\(false\);\s*return;') {
    throw 'A radio started by this turn must take the speaker back immediately instead of waiting out the window.'
}
# 窗口到期后的收场一律走完整链条。用户明确要求：主动唤醒小智对话时，无论如何都要有
# 本地询问和第二轮的聆听窗口。旧实现按“本轮到底开过口没有”分叉，开过口就直接
# close_channel 把扬声器交回电台，本地询问和第二轮窗口被整个跳过——那个分叉必须彻底
# 不存在，否则电台在播时唤醒就永远听不到“我没听清”。
if ($voice -match 's_radio_on_before_turn && s_turn_had_speech') {
    throw 'The expired-window teardown must not branch on whether the user spoke; every wake turn needs the local prompt plus a second listen window.'
}
# 收场链的顺序也要钉死：开机问候（设备主动发起，允许跳过重试）-> 本地询问重试
# （播提示音并再开一轮 6.4 秒窗口）-> 告别语 -> radio_played_through_turn 停电台。
# 少了中间一段，用户就只剩一声“再见”。
$expiryPos = $voice.IndexOf('if (s_state == XIAOZHI_VOICE_WAIT_FOLLOWUP && s_followup_deadline_us != 0')
$bootSkipPos = if ($expiryPos -ge 0) { $voice.IndexOf('if (s_boot_greeting_question)', $expiryPos) } else { -1 }
$followupRetryPos = if ($expiryPos -ge 0) { $voice.IndexOf('else if (!s_followup_retry_used)', $expiryPos) } else { -1 }
$exitPromptPos = if ($followupRetryPos -ge 0) { $voice.IndexOf('request_cloud_exit_prompt()', $followupRetryPos) } else { -1 }
if ($expiryPos -lt 0 -or $bootSkipPos -lt 0 -or $followupRetryPos -lt 0 -or $exitPromptPos -lt 0 -or
    $followupRetryPos -lt $bootSkipPos) {
    throw 'The follow-up expiry chain must be: boot-greeting skip -> did-not-catch retry (local prompt + second window) -> goodbye.'
}
# 本地询问分支必须真的把提示音请求出去，不能只开窗口不吭声。
if ($voice -notmatch '(?s)else if \(!s_followup_retry_used\)[\s\S]{0,700}?request_cloud_timeout_prompt\(\)') {
    throw 'The did-not-catch branch must play the local prompt before re-arming the listen window.'
}
# “本轮到底开过口没有”仍然要真实跟踪：它是电台收场与日志判定的依据。
if ($voice -notmatch 'static bool s_turn_had_speech;' -or
    $voice -notmatch '(?s)static void send_detect\(bool start_current_speech\)[\s\S]{0,900}?s_turn_had_speech = false;') {
    throw 'A wake turn must clear the spoke-this-turn latch before it decides how to close a parked radio.'
}
if ($voice -notmatch '(?s)s_state = XIAOZHI_VOICE_LISTENING;[\s\S]{0,80}?s_turn_had_speech = true;') {
    throw 'Opening the Opus uplink is what marks the turn as actually spoken; the latch must be set there.'
}
# 会话收尾必须归还扬声器：错误音、超时关闭、唤醒后没说话就退出都要走到这里。
if ($voice -notmatch '(?s)void\)radio_stream_service_regain\(\);[\s\S]{0,80}?s_client_cleanup_in_progress = false;') {
    throw 'Every session teardown must release the radio yield.'
}
# 用户拍板：唤醒时电台就在播的那段对话，收场时电台跟着结束（下次要听重新说“打开
# 电台”）；本轮自己打开的电台是用户当场要的结果，收场只把扬声器还回去。
if ($voice -notmatch 'const bool radio_played_through_turn = s_radio_on_before_turn;') {
    throw 'The teardown must remember whether the radio was already playing when the turn began.'
}
if ($voice -notmatch '(?s)else if \(radio_played_through_turn\)[\s\S]{0,200}?radio_stream_service_stop\(\);') {
    throw 'A conversation that ran over a playing radio must stop it on teardown.'
}
# 本轮才打开的电台不能被同一段收尾逻辑误杀。窗口要容得下"先判断这次是换台还是默认
# 台、再归还扬声器"这整条分支——窗口卡在 400 的话，下次只是多打一行日志就会假失败。
if ($voice -notmatch '(?s)if \(release_radio_after_close\)[\s\S]{0,700}?\(void\)radio_stream_service_regain\(\);') {
    throw 'A radio started by this turn must survive the teardown.'
}
# 电台还挂着时屏幕停在语音页：在播算，"暂停但没退出"也算；一轮对话结束就把屏幕
# 翻回时钟，用户既看不到自己停的是哪个台，也找不到"继续播放"的入口。只有"退出电台"
# 真正终止它（stop 会把两个标志一起清掉）之后，回主页面的分支才会生效。
$ui = Get-Content -Raw "$PSScriptRoot\..\main\APP\ui_service.c"
if ($ui -notmatch 'radio_stream_service_is_requested') {
    throw 'The UI must know whether the radio is playing.'
}
if ($ui -notmatch '(?s)if \(!radio_stream_service_is_requested\(\) &&[\s\S]{0,80}?radio_stream_service_is_user_paused\(\)\)[\s\S]{0,120}?set_page\(charging_active\(snapshot\) \? UI_PAGE_CHARGING : UI_PAGE_MAIN\);') {
    throw 'The UI must stay on the voice page while the radio is playing or merely paused.'
}
# 电台驻留时**不再**冒用"说话"的脸。大眼加脉冲嘴是"小智在长篇回答"的样子，而电台
# 一放就是半小时，用户分不清设备到底在干什么，也看不到自己停的是哪个台。它有自己的
# 显示：眯着的眼 + 电平表 + 一行"台名 | 状态"。反向断言卡住的是"再把它映射回说话"
# 这条老路——当初就是这么写的，而它正是用户提出要改掉的东西。
if ($ui -match 'state = VOICE_UI_SPEAKING;') {
    throw 'The radio must not be mapped onto the cloud speaking face any more.'
}
if ($ui -notmatch 'const bool radio_idle = state == VOICE_UI_IDLE &&') {
    throw 'The voice page needs its own radio-idle state instead of faking a speaking face.'
}
if ($ui -notmatch 'update_voice_eyes\(state, radio_idle\);') {
    throw 'The radio-idle state must reach the expression renderer.'
}
if ($ui -notmatch 'update_radio_strip\(radio_idle\);') {
    throw 'The level meter and the channel label must follow that same state.'
}
# 只换脸不写字，用户还是不知道放的是哪个台、停没停。两样都得有。
if ($ui -notmatch 'radio_stream_service_station_name\(\)') {
    throw 'The radio strip must name the current channel.'
}
if ($ui -notmatch 'radio_stream_service_is_user_paused\(\)') {
    throw 'The radio strip must ask for the user pause explicitly; is_requested() is false by then.'
}
# 三种状态都要说出来。第三个（正在接流）不是凑数：断流重连要十几秒到几十秒，屏幕这时
# 还写着“播放中”的话，用户只会以为设备死了——实测日志里那三次 http_read_eof 正是这么
# 被用户描述成“闪退”的。
foreach ($state in @('"已暂停"', '"播放中"', '"连接中"')) {
    if ($ui -notmatch [regex]::Escape($state)) {
        throw "The radio strip must be able to say $state."
    }
}
if ($ui -notmatch [regex]::Escape('const bool flowing = !paused && radio_stream_service_is_active();')) {
    throw 'The radio strip must derive playing from is_active(), not from the play request.'
}
if ($ui -notmatch [regex]::Escape('UI_RADIO_BAR_COUNT')) {
    throw 'The level meter must be built from the shared bar geometry.'
}
# 电台驻留时不画脸：屏幕上只留图标、电平表和台名。反向断言卡的是"又把眼睛画回来"
# ——那是上一版的做法，用户明确要求去掉。
if ($ui -notmatch '(?s)if \(radio_idle\)\s*\{\s*lv_obj_add_flag\(s_scene\.voice_eye_left, LV_OBJ_FLAG_HIDDEN\);') {
    throw 'The radio screen must not draw the voice face.'
}
# 眼睛是全项目唯一会被隐藏又复用的表情元素：恢复必须和隐藏写在同一条路径上，
# 漏清一次就是"下次唤醒没有脸"。
if ($ui -notmatch '(?s)static void update_voice_eyes\(voice_ui_state_t state, bool radio_idle\)\s*\{[\s\S]{0,900}?lv_obj_remove_flag\(s_scene\.voice_eye_left, LV_OBJ_FLAG_HIDDEN\);') {
    throw 'The eyes must be restored on every non-radio path, or the next wake has no face.'
}
# 左右两侧各一个"下一台"图标。
foreach ($side in @('left', 'right')) {
    if ($ui -notmatch "lv_image_set_src\(s_scene\.radio_icon_$side, &ui_icon_radio_next(?:_left)?\);") {
        throw "The radio screen must show the next-channel icon on the $side."
    }
}
# 左边那份是右边的水平镜像，不是同一个符号：两个都朝右就把"左右都能走"说成了"一直往后"。
if ($ui -notmatch 'lv_image_set_src\(s_scene\.radio_icon_left, &ui_icon_radio_next_left\);') {
    throw 'The left next-channel icon must use the mirrored asset, not the right-facing one.'
}
# 图标是新增资产：SVG、CMake 的 EMBED_FILES、ui_assets 的符号三者缺一，构建或
# 链接期才会炸，而那时已经离改动很远了。左右两份都要查。
$assetsC = Get-Content -Raw "$PSScriptRoot\..\main\APP\ui_assets.c"
$assetsH = Get-Content -Raw "$PSScriptRoot\..\main\APP\ui_assets.h"
foreach ($icon in @('radio_next_icon', 'radio_next_left_icon')) {
    if (-not (Test-Path "$PSScriptRoot\..\main\assets\ui\$icon.svg")) {
        throw "The next-channel icon must keep its vector source next to the other UI assets ($icon)."
    }
    if ($cmake -notmatch [regex]::Escape("assets/ui/$icon.rgb565a8")) {
        throw "The icon binary must be embedded, or the symbol is missing at link time ($icon)."
    }
    if ($assetsC -notmatch "${icon}_rgb565a8_start") {
        throw "ui_assets.c must bind the embedded icon ($icon)."
    }
}
if ($assetsC -notmatch 'UI_IMAGE_RGB565A8\(ui_icon_radio_next, ui_icon_radio_next_data, 32, 32\);') {
    throw 'The icon needs a descriptor with its real size; a wrong width shears the bitmap.'
}
if ($assetsH -notmatch 'extern const lv_image_dsc_t ui_icon_radio_next;') {
    throw 'ui_assets.h must declare the icon so the UI can use it.'
}
if ($assetsC -notmatch 'UI_IMAGE_RGB565A8\(ui_icon_radio_next_left, ui_icon_radio_next_left_data, 32, 32\);') {
    throw 'The mirrored icon needs its own 32x32 descriptor; a wrong width shears the bitmap.'
}
if ($assetsH -notmatch 'extern const lv_image_dsc_t ui_icon_radio_next_left;') {
    throw 'ui_assets.h must declare the mirrored icon so the left side can use it.'
}
# 镜像不是"再导出一份长得差不多的图"，而是逐字节可验证的关系：把右边那张按行翻转，要
# 和左边那张完全一致。某一次只重新导出了其中一侧，用户看到的就是两个都朝右的箭头——
# 那种错误在 320x170 上只有几像素差别，靠肉眼在真机上是抓不住的。
$rightIconBytes = [IO.File]::ReadAllBytes("$PSScriptRoot\..\main\assets\ui\radio_next_icon.rgb565a8")
$leftIconBytes = [IO.File]::ReadAllBytes("$PSScriptRoot\..\main\assets\ui\radio_next_left_icon.rgb565a8")
if ($rightIconBytes.Length -ne 3072 -or $leftIconBytes.Length -ne 3072) {
    throw "Both next-channel icons must be 32x32 RGB565A8 (3072 bytes); got $($rightIconBytes.Length) and $($leftIconBytes.Length)."
}
for ($row = 0; $row -lt 32; $row++) {
    for ($column = 0; $column -lt 32; $column++) {
        $mirrored = $row * 32 + (31 - $column)
        $straight = $row * 32 + $column
        # 前 2048 字节是 RGB565 颜色平面，后 1024 字节是 alpha 平面，两个都要翻。
        if ($rightIconBytes[$mirrored] -ne $leftIconBytes[$straight] -or
            $rightIconBytes[$mirrored + 1] -ne $leftIconBytes[$straight + 1] -or
            $rightIconBytes[$mirrored + 2048] -ne $leftIconBytes[$straight + 2048]) {
            throw "The left next-channel icon must be the exact horizontal mirror of the right one (pixel $column,$row)."
        }
    }
}
# 台名/状态/电平表要大一号：14px 的字和 8px 宽的条在 320x170 上太小（用户反馈）。
# 注意不能靠 ui_cjk_font_17 充数——它和 14px 共用同一张字形位图，只改了行距，
# 字还是 14px 那么大。
if ($ui -notmatch 'make_label\(s_scene\.voice_page, "", &ui_cjk_font_radio,') {
    throw 'The radio name/status line must use the dedicated 20px CJK face, not the 14px body font.'
}
$barWidth = [int]([regex]::Match($ui, '#define UI_RADIO_BAR_WIDTH (\d+)').Groups[1].Value)
$barGap = [int]([regex]::Match($ui, '#define UI_RADIO_BAR_GAP (\d+)').Groups[1].Value)
$barCount = [int]([regex]::Match($ui, '#define UI_RADIO_BAR_COUNT (\d+)U').Groups[1].Value)
$barLeft = [int]([regex]::Match($ui, '#define UI_RADIO_BAR_LEFT (\d+)').Groups[1].Value)
$barBaseY = [int]([regex]::Match($ui, '#define UI_RADIO_BAR_BASE_Y (\d+)').Groups[1].Value)
$barMinHeight = [int]([regex]::Match($ui, '#define UI_RADIO_BAR_MIN_HEIGHT (\d+)').Groups[1].Value)
$labelTop = [int]([regex]::Match($ui, '#define UI_RADIO_LABEL_TOP (\d+)').Groups[1].Value)
if ($barWidth -lt 12 -or $barMinHeight -lt 12) {
    throw "The radio level bars must stay enlarged; got width $barWidth and minimum height $barMinHeight."
}
# 整组必须仍然水平居中：改了条宽或间隔却忘了改 UI_RADIO_BAR_LEFT，这一组就会偏向一边。
$barSpan = $barCount * $barWidth + ($barCount - 1) * $barGap
if ($barLeft -ne [int]([math]::Floor((320 - $barSpan) / 2))) {
    throw "The radio level bars are no longer centred: span $barSpan px at left $barLeft."
}
# 电平表在上、台名在下，两行不能叠在一起。
if ($labelTop -le $barBaseY) {
    throw "The radio name/status line must sit below the level bars (bars end at $barBaseY, label at $labelTop)."
}
# 状态栏下面那条横线由共享构造函数画，但**不是每页都要**：用户明确要求主页面不要它。
# 所以三个调用点必须各说各的，谁也不能靠"构造函数顺手给了"。
if ($ui -notmatch 'lv_obj_set_pos\(bar->divider, 0, UI_STATUS_BAR_DIVIDER_Y\);') {
    throw 'The status bar rule must sit right under the icons it belongs to.'
}
if ($ui -notmatch '(?s)bar->divider = NULL;\s*if \(divider\)') {
    throw 'A page that does not want the rule must leave bar->divider null instead of building one.'
}
if ($ui -notmatch 'create_status_bar\(s_scene\.main_page, &s_scene\.main_status, false, false\);') {
    throw 'The main page must not draw the status bar rule.'
}
if ($ui -notmatch 'create_status_bar\(s_scene\.voice_page, &s_scene\.voice_status, true, true\);') {
    throw 'The radio screen relies on the shared status bar for that rule.'
}
if ($ui -notmatch 'create_status_bar\(s_scene\.charging_page, &s_scene\.charging_status, true, true\);') {
    throw 'The charging page must ask for the rule explicitly too.'
}
# ---------------------------------------------------------------- HLS 电台
# 蜻蜓FM 的 MP3 长连接路径是故意删掉的（它的取流方式、错误处理和 HLS 完全不同，
# 两条并存要维护两套）。蜻蜓那版留在 git 标签 qtfm-radio-checkpoint 上，真要用就切
# 回那个版本，不要在这里重新长出第二条路径。
if ($radio -match 'RADIO_SOURCE_MP3|RADIO_URL_MP3|ESP_AUDIO_SIMPLE_DEC_TYPE_MP3|esp_http_client_read\(http') {
    throw 'The Qtfm MP3 path was removed on purpose; every channel now goes through HLS.'
}
# 频道表是“设备能放什么”的唯一真相。加台只能改这张表，不能在别处硬编码地址——
# 那样 MCP 描述、本地短语表和实际能播的台会三方对不上。
if ($radio -notmatch '#define RADIO_STATION_COUNT 4U') {
    throw 'The channel table is the single source of truth for what the radio can play.'
}
foreach ($channel in @('zgzs', 'jjzs', 'yyzs', 'wyzs')) {
    if ($radio -notmatch "live/$channel/index\.m3u8") {
        throw "Missing channel $channel; probe the URL before adding it, hzzs is a known 404."
    }
}
if ($radio -notmatch 'ESP_AUDIO_SIMPLE_DEC_TYPE_TS') {
    throw 'HLS carries AAC inside a TS container; the MP3 decoder cannot read it.'
}
if ($radio -notmatch 'esp_ts_dec_cfg_t ts_cfg = \{\.aac_plus_enable = true\}') {
    throw 'Open the TS decoder with AAC+ enabled: HE-AAC segments stay silent otherwise.'
}
if ($radio -notmatch 'RADIO_HLS_PLAYLIST_MAX 1024U') {
    throw 'The playlist is tiny but must stay bounded; never allocate it per fetch.'
}
# 每片分片必须开新连接，不能复用上一片的 socket。分片服务器常在响应完就关连接，而
# state 已到 HTTP_STATE_CONNECTED 的 esp_http_client_open() 会跳过重连、把请求写进一条
# 死 socket，之后整段卡在 timeout_ms 上——实测电台一次哑掉 11.7 秒（日志 49990 → 61710
# 没有任何 PCM 入队，队列从 46 帧耗干后 underrun）。建连实测只要约 240 ms，而分片是
# 10 秒一个，这点握手开销被 1 秒的播放预缓冲完全吸收。
if ($radio -notmatch '(?s)hls_open_segment[\s\S]{0,1200}?esp_http_client_set_url\(hls->http, url\);\s*\n\s*esp_http_client_close\(hls->http\);') {
    throw 'Each segment must get a fresh connection; a reused dead socket costs a whole timeout.'
}
# 超时是“一个坏分片能把电台静音多久”的上界，必须按一片的时间来定（分片是 10 s 一个）。
# 4 s → 10 s 那次放宽把静音从 4 秒级变成 12.8 秒（日志 61707 → 74487），改回 6 s：
# 比原来宽 50% 保住弱网下不误判，又不会让一次超时超过一整个分片。
if ($radio -notmatch '\.timeout_ms = RADIO_HLS_SEGMENT_TIMEOUT_MS,' -or
    $radio -notmatch '#define RADIO_HLS_SEGMENT_TIMEOUT_MS 6000') {
    throw 'The socket timeout is what bounds how long one bad segment can silence the radio.'
}
# 分片会随滑动窗口过期。拉不到时先跳过这一片试下一片，只有连下一片也拿不到才重拉列表：
# 一次失败的 open 已经吃掉整个超时，再赌两次同样的超时会把一次抖动放大成三倍静音。
if ($radio -notmatch '(?s)\+\+hls->next_index;[\s\S]{0,400}?hls_read_playlist\(hls, true\)') {
    throw 'A segment that aged out of the sliding window must be skipped, then trigger a playlist re-read.'
}
# 追平直播不是网络故障。设备把播放列表里的分片读完就无片可拉，下一片还没生成；老实现会
# 去 open 一个不存在的分片，CNR 回的是带 keep-alive 的空体 404，read 一直等到超时才返回
# 0，然后被当成“这片读完了”静默跳片——两次超时就是日志里 51517→64017 的 12.5 秒静音。
# 四条不变式：记住列表末尾、越过末尾就等新片而不是重连、open 时先看状态码、read 返回 0
# 时要分辨“读完”与“读超时”。
if ($radio -notmatch 'uint64_t last_index;' -or
    $radio -notmatch 'hls->last_index = last_index;' -or
    $radio -notmatch 'hls->next_index > hls->last_index') {
    throw 'The HLS reader must know where the playlist ends, or it will open segments that do not exist yet.'
}
if ($radio -notmatch 'esp_http_client_get_status_code\(hls->http\) != 200') {
    throw 'A 404 segment body keeps the socket alive, so the status code must be checked instead of waiting out the whole timeout.'
}
if ($radio -notmatch 'esp_http_client_is_complete_data_received\(hls->http\)') {
    throw 'A zero-length read is either a drained segment or a read timeout; they must be told apart.'
}
if ($radio -notmatch 'if \(hls\.at_live_edge\)') {
    throw 'Catching up with the live edge must wait for the next segment instead of reconnecting.'
}
if ($radio -notmatch 'RADIO_HLS_READ_TIMEOUT_MS') {
    throw 'Reading the segment body must use the short timeout so one stalled read cannot cost a whole segment.'
}
# 让位必须立刻生效：填满 16 KB 的内层循环里也要回头看状态，否则唤醒瞬间电台还在读网络、
# 占着 WiFi 接收缓冲，正撞在 WSS 建连上（实测建 seq=3 时 largest 只剩 1984 B）。
# 而且收手后必须回循环顶部，不能带着旧 epoch 继续往下入队：那会拿到 INVALID_ARG，
# 被当成“被语音抢占”而误置 s_yielded，把一次切台锁成永远等不到 regain() 的让位态。
if ($radio -notmatch 'relinquish = s_yielded \|\| !s_play_requested \|\| s_epoch != epoch;' -or
    $radio -notmatch 'if \(relinquish\) continue;') {
    throw 'A yield during the read-ahead fill must break out before decoding with a stale epoch.'
}
if ($radio -notmatch '\+\+hls->next_index;') {
    throw 'Finishing a segment must advance to the next one.'
}
# 换片是有界的：循环里每轮都要发一次 HTTP 请求，无界重试会把任务转死在网络错误上。
if ($radio -notmatch 'RADIO_HLS_MAX_ADVANCE_PER_READ') {
    throw 'Segment advancing must be bounded so a dead stream cannot spin the task.'
}
# 取窗口里倒数第二片：落后直播约 20 s，换来同量级的抖动余量。取最后一片只剩 10 s，
# 一抖就掉出窗口；取最旧一片虽然余量最大，但落后 60 s 太夸张。
if ($radio -notmatch 'hls->next_index = prev_index != 0U \? prev_index : last_index;') {
    throw 'Start from the second-newest segment: the newest one leaves no jitter margin.'
}
# 解码/重采样/入队/背压必须完全共用，分歧只允许出现在“字节从哪来”这一处。
if ($radio -notmatch '\(s_station_index \+ 1U\) % RADIO_STATION_COUNT') {
    throw 'Next-channel must wrap around instead of running off the end of the table.'
}
# 换台只剩"下一个"这一条路。按台名匹配曾被当成容错，实际是把 ASR 的错都堆到设备侧
# 去猜：实测"关闭电台"都会被切成"电台。"，台名更是随时多一个字少一个字，而猜错的
# 表现是"用户说了没反应"。所以名字不许再参与选台。
if ($radio -match 'strstr\(query') {
    throw 'Station selection must not match names: ASR mangles them and a wrong guess is silent.'
}
if ($radio -match 'play_station_by_name') {
    throw 'The name-based switch entry point must stay deleted; only next-channel may switch.'
}
# 设备状态仍要带上当前台名：那是给云端回话用的（"现在放的是中国之声"），
# 不再是云端推算下一个台的依据——它推出来的名字只会再走一次匹配。
$mcp = Get-Content -Raw "$PSScriptRoot\..\main\MID\system\mcp_service.c"
if ($mcp -notmatch '\\"radio\\":\{\\"playing\\":%s,\\"station\\":\\"%s\\"\}\}') {
    throw 'The device status must still report the current channel.'
}
# 读和消化必须解耦。esp_audio_simple_dec_process 一次只消化能填满输出缓冲的那几帧：
# 输出缓冲 8 KB，而 48 kHz 立体声 AAC 一帧就是 4 KB，所以一次只吃得下约两帧（几百
# 字节）；而 HLS 的分片响应体在 socket 里已经就绪，一次 esp_http_client_read 能把整个
# 16 KB 输入缓冲拉满。只喂一次就回头再读的话，读入是消化的三十几倍，输入缓冲必然一直
# 满、一直整块丢掉——实机就是响一两秒之后再也没有声音，而且这个循环会持续烧 core 1
# 和 PSRAM 带宽（AFE 也在 PSRAM 里跑）。旧的 MP3 长连接没暴露它，只因为服务端按实时
# 供流，一次 read 通常只有一两 KB。
if ($radio -notmatch 'size_t last_consumed = 0U;') {
    throw 'The parser must be drained between reads: one process call consumes only a couple of frames.'
}
if ($radio -notmatch '\(input_len == 0U \|\| last_consumed == 0U\)') {
    throw 'Reads must be gated on the parser having drained its buffer.'
}
if ($radio -notmatch '(?s)last_consumed = raw\.consumed;') {
    throw 'The drain decision needs the byte count the parser actually consumed.'
}
Write-Output 'Audio radio and volume contract passed.'
