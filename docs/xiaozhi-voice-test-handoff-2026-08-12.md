# XiaoZhi Voice Test Handoff

## Purpose

This record preserves the current XiaoZhi voice-test state for the next hardware session.
It intentionally excludes Wi-Fi passwords, activation tokens, device IDs, and websocket tokens.

## Hardware And Build Profile

- Board: ESP32-S3 N8R2 with 2 MiB PSRAM.
- Console: USB Serial/JTAG on COM6. UART0 remains reserved for the STC link.
- ES8311: I2C SDA GPIO17, SCL GPIO18, MCLK GPIO38; I2S BCLK GPIO10, LRCLK GPIO9,
  microphone data GPIO8, amplifier data GPIO7.
- Audio format: 16 kHz, mono, signed 16-bit PCM.
- Current bench flags in `main/APP/app_manager.c`:
  - `LUMIDOCK_NETWORK_UI_TEST_MODE=1`
  - `LUMIDOCK_VOICE_TEST_MODE=1`
  - Button and BMS remain disabled while this profile is enabled.

## Current Voice Architecture

- Startup waits for activation, the main page to become stable, and the initial location/weather
  requests to settle or reach their 15-second deadline.
- ES8311 initialization and one-second microphone self-test occur before the voice service starts.
- WakeNet and VAD run in the AFE path. AEC, NS, and SE are intentionally disabled for the 2 MiB
  PSRAM target.
- Opus uses 16 kHz mono, 60 ms frames, 32 kbps. The encoder and decoder are owned by one
  `voice_codec` task on CPU0 with a 32 KiB PSRAM stack; feed runs on CPU0 and AFE detection runs on CPU1.
- PCM crosses from AFE detection to the codec task through a four-frame fixed queue. It is no
  longer assembled from a shared byte ringbuffer.
- The voice session/WebSocket task runs on CPU0 with a PSRAM stack. The follow-up window is five
  seconds after cloud TTS stops, and it waits for a VAD silence edge before accepting another
  question without a second wake word.
- ES8311 output remains hardware-muted except while cloud TTS PCM is being played. Output level is
  configured at 25 percent.

## Required Startup Logs

After network, activation, and the main page are stable, the expected sequence is:

```text
ES8311 ready: 16 kHz mono PCM ... output ... muted
ES8311 microphone self-test: samples=16000 ...
WSS prewarm ready before AFE allocation
voice codec ready: pcm=... opus=...
voice pipeline ready: WakeNet, VAD, single Opus codec (AEC/NS disabled)
ES8311 and XiaoZhi voice services ready
```

Useful periodic diagnostics are `AFE input`, `AFE state`, and the internal/PSRAM memory logs.
The microphone should show a material RMS/peak increase while speaking.

## Required Conversation Logs

For one normal turn:

```text
WakeNet detected; queuing voice session
voice wake accepted; waiting for speech
voice VAD: speech started; Opus uplink enabled
voice stream start
PCM frame queued
Opus encode begin
Opus encode end
Opus packet queued
voice uplink
cloud JSON received: ...
cloud JSON processed: ...
cloud TTS stopped; speaker muted, follow-up window=5s
```

For a follow-up within five seconds, first see `follow-up silence observed; ready for next question`,
then `follow-up speech detected; starting next cloud turn` and another `voice uplink`.

## 连续追问修复状态

已完成 TTS 播放队列的单一所有权修复：

- AFE 控制队列不再承载播放队列清理事件。
- 语音状态机在 TTS 停止或中断时只递增 `playback_epoch`、立即硬件静音并设置清理请求。
- 只有 `voice_codec_task` 接收、归还和清空 `tts_queue`；`audio_detect_task` 只控制 AFE。

这消除了 TTS 结束、进入 5 秒追问窗口时两个任务并发访问同一 ringbuffer 的已知风险。任务栈和内存预算没有扩大。

## Pre-Flash Audit Update

The following additional risks were found by source and ESP-IDF lifecycle review, then corrected:

- `voice_codec` no longer keeps a 2 KiB `pcm_frame_t` on its stack. Its codec frame is PSRAM-backed and has pipeline-owned cleanup.
- The AFE detect task no longer constructs full PCM queue items on its 6 KiB stack. Submit and dropped-frame scratch items are persistent PSRAM buffers.
- Tasks, queues, and ringbuffers created with `*WithCaps()` are released through their required matching `*WithCaps()` API during a failed initialization retry.
- Codec work buffers now belong to the pipeline state, so an initialization failure followed by the 10-second retry does not strand PSRAM allocations.
- Binary WebSocket payloads are accepted only while the service is in `SPEAKING`; a delayed packet after cloud `tts stop` is logged and dropped instead of re-opening the speaker during the follow-up window.

The first decoded TTS packet now emits one diagnostic line:

```text
tts decode format: stream_rate=... channels=... bits=... frame=... pcm=...
```

Record that line during the first reply. It verifies the decoder's actual PCM format against the ES8311 16 kHz mono output path before making any sample-rate change.

## Latest Hardware Failure And Fix

The August 12 serial log proves the previous 16 KiB codec task stack is insufficient for the
ESP32-S3 Opus encoder implementation: immediately before `esp_opus_enc_process()` the task had
14,268 bytes remaining, then FreeRTOS reported `A stack overflow in task voice_codec`.
The PCM queue handoff had already completed successfully (`PCM frame queued: bytes=1920`), so this
is not an AFE queue or follow-up-state failure. `VOICE_CODEC_STACK_BYTES` is therefore 32 KiB,
still allocated from PSRAM. The log showed approximately 1.37 MiB PSRAM free at the failure point,
so this measured 16 KiB increase remains within the board's confirmed memory budget.

## Tonight's Test Order

1. Flash the resulting firmware and capture COM6 from boot through `voice pipeline ready`.
2. Say the wake word, then ask one short question; capture logs through the full TTS stop event.
3. After a silence edge, ask a second question inside five seconds; repeat for three full turns.
4. Report the complete COM6 log if any of these occurs: restart, Guru Meditation, watchdog reset,
   missing `Opus encode`/`voice uplink`, unexpected speaker unmute, or premature return to the main page.

## Deferred Daytime Work

- BMS is now prepared for UART2 GPIO39/40 hardware testing and remains disabled by the current
  bench profile.
- Button GPIO12 and BMS can be enabled together only after their wiring is ready.
- STC UART0 on GPIO43/44 stays independent of the BMS and voice tests.
