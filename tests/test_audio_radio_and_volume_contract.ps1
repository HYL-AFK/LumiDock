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
# WiFi 接收侧长期被削到底（静态 3 / 动态 6），而发送侧一个没动。RX_BA_WIN 的
# 运行时上限是 (静态+动态)/2 = 4，所以 4 是不加缓冲数时能取到的最大值。
# 两个文件都要守：defaults 是重新生成 sdkconfig 时的来源，只改一处会在下次
# 重新生成时静默回退成 3。
foreach ($config in @($sdkconfig, $sdkconfigDefaults)) {
    if ($config -notmatch '(?m)^CONFIG_ESP_WIFI_RX_BA_WIN=4\s*$') {
        throw 'The WiFi AMPDU RX window must sit at its ceiling; the RX path is the radio bottleneck.'
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
if ($radio -notmatch '(?s)enq == ESP_ERR_INVALID_ARG[\s\S]{0,200}?s_yielded = true;') {
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
if ($voice -notmatch '(?s)if \(s_radio_on_before_turn\)\s*\{[\s\S]{0,400}?s_state = XIAOZHI_VOICE_WAIT_FOLLOWUP;') {
    throw 'Interrupting a playing radio must arm the follow-up window before handing the speaker back.'
}
# 窗口期间电台必须一直让位：这一段里不能出现 regain，否则窗口形同虚设。
if ($voice -match '(?s)if \(s_radio_on_before_turn\)\s*\{[\s\S]{0,600}?radio_stream_service_regain\(\);') {
    throw 'The radio must stay parked for the whole follow-up window instead of being handed back early.'
}
# 电台是这一轮才被拉起来的（用户说"开电台"）时不能压着窗口，必须立即归还。
if ($voice -notmatch '(?s)if \(s_radio_on_before_turn\)[\s\S]{0,500}?\}\s*else\s*\{[\s\S]{0,400}?close_channel\(false\);\s*return;') {
    throw 'A radio started by this turn must take the speaker back immediately instead of waiting out the window.'
}
# 窗口到期时电台还在等：不请求告别语，直接 close_channel -> regain 把直播接回来。
if ($voice -notmatch '(?s)s_radio_on_before_turn && radio_stream_service_is_requested\(\)[\s\S]{0,200}?close_channel\(false\);') {
    throw 'An expired radio follow-up window must regain the speaker directly instead of asking for a farewell prompt.'
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
# 本轮才打开的电台不能被同一段收尾逻辑误杀。
if ($voice -notmatch '(?s)if \(release_radio_after_close\)[\s\S]{0,400}?radio_stream_service_regain\(\);') {
    throw 'A radio started by this turn must survive the teardown.'
}
# 电台在播时屏幕停在语音页：电台是同一轮对话拉起来的“小智在说话”，只是音源不同；
# 一轮对话结束就把屏幕翻回时钟，实机上看起来像电台被打断了。只有“退出电台”终止
# 电台之后，回主页面的分支才会生效。
$ui = Get-Content -Raw "$PSScriptRoot\..\main\APP\ui_service.c"
if ($ui -notmatch 'radio_stream_service_is_requested') {
    throw 'The UI must know whether the radio is playing.'
}
if ($ui -notmatch '(?s)if \(!radio_stream_service_is_requested\(\)\)[\s\S]{0,120}?set_page\(charging_active\(snapshot\) \? UI_PAGE_CHARGING : UI_PAGE_MAIN\);') {
    throw 'The UI must stay on the voice page while the radio is playing.'
}
# 电台在播时脸要是“小智在说话”（眼睛配一张会动的嘴），不是安静闭眼：这一轮对话
# 已经收尾，voice_session.state 回落成 IDLE，必须在这里把“电台出声中”当成说话。
if ($ui -notmatch 'state == VOICE_UI_IDLE && radio_stream_service_is_requested\(\)[\s\S]{0,60}?state = VOICE_UI_SPEAKING;') {
    throw 'The voice page must show the speaking face while the radio is playing.'
}
# ---------------------------------------------------------------- HLS 电台
# 蜻蜓FM 已经下架中国之声的静态 MP3 源（live/386 实测 404，而同域名的 20500038 仍正常
# 吐流，所以是单个 ID 下架、不是域名失效）。官方源是 HLS，想听“正规”电台就只剩这一条
# 路。两条取流路径都留在文件里，切回去只改 RADIO_SOURCE 一个宏。
if ($radio -notmatch '#define RADIO_SOURCE RADIO_SOURCE_HLS') {
    throw 'The radio must prefer the HLS source unless the Qtfm static stream is restored.'
}
if ($radio -notmatch '#define RADIO_URL_HLS "http://ngcdn001\.cnr\.cn/live/zgzs/index\.m3u8"') {
    throw 'The HLS playlist URL must point at the CNR stream that was verified on the bench.'
}
if ($radio -notmatch '#define RADIO_URL_MP3 "http://lhttp\.qtfm\.cn/live/20500038/64k\.mp3"') {
    throw 'Keep the verified Qtfm URL in the file so switching back stays a one-line change.'
}
if ($radio -notmatch 'RADIO_SOURCE == RADIO_SOURCE_HLS \? ESP_AUDIO_SIMPLE_DEC_TYPE_TS') {
    throw 'HLS carries AAC inside a TS container; the MP3 decoder cannot read it.'
}
if ($radio -notmatch 'esp_ts_dec_cfg_t ts_cfg = \{\.aac_plus_enable = true\}') {
    throw 'Open the TS decoder with AAC+ enabled: HE-AAC segments stay silent otherwise.'
}
if ($radio -notmatch 'RADIO_HLS_PLAYLIST_MAX 1024U') {
    throw 'The playlist is tiny but must stay bounded; never allocate it per fetch.'
}
# 每 10 s 一个分片。IDF 的 esp_http_client_connect() 只在 state < HTTP_STATE_CONNECTED
# 时才真的重连（esp_http_client.c:1469），所以只要不提前 close()，换 URL 再 open() 就是
# 在原 socket 上发下一个 GET。实测首次建连要 7.3 s，每片付一次根本来不及。
if ($radio -notmatch '(?s)hls_open_segment[\s\S]{0,900}?esp_http_client_set_url\(hls->http, url\);[\s\S]{0,120}?esp_http_client_open\(hls->http, 0\)') {
    throw 'Segment requests must reuse the open socket instead of reconnecting every 10 seconds.'
}
# 分片会随滑动窗口过期。拉不到就重拉列表拿新序号，否则电台会卡在窗口外面再也不出声。
if ($radio -notmatch '(?s)hls_open_segment\(hls, hls->next_index\)[\s\S]{0,400}?hls_read_playlist\(hls, true\)') {
    throw 'A segment that aged out of the sliding window must trigger a playlist re-read.'
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
if ($radio -notmatch '(?s)RADIO_SOURCE == RADIO_SOURCE_HLS\s*\?\s*hls_read\(&hls,') {
    throw 'Both sources must share one decode path; only the byte source may differ.'
}
Write-Output 'Audio radio and volume contract passed.'
