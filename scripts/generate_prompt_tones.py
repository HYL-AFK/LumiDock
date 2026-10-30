#!/usr/bin/env python3
"""Generate short local "confirmation prompt" PCM assets.

The device plays one of these phrases *immediately* after it recognises the
STT intent (before the cloud reply arrives), so it feels responsive:

  prompt_executing_16k_mono_s16le.pcm  <- "正在执行"   (本地命令: 音量/闹钟/计时器)
  prompt_querying_16k_mono_s16le.pcm   <- "正在查询"    (查询类: 天气/时间/日期)
  prompt_thinking_16k_mono_s16le.pcm   <- "正在思考"    (聊天/其余)

It also pre-renders every prompt that used to be a cloud [device_call] TTS
request.  The official xiaozhi.me server rejects [device_call], so these are
fixed local PCM assets chosen on the device:

  prompt_greet_*.pcm                   <- 开机问候 · 时段问候段
  prompt_hour_NN_*.pcm                 <- 开机问候 · 报时段 (tm_hour 直接索引)
  prompt_wx_*.pcm                      <- 开机问候 · 天气段
  prompt_greet_closing_*.pcm           <- 开机问候 · 固定收尾句
  prompt_exit_16k_mono_s16le.pcm       <- 会话退出语
  prompt_timeout_16k_mono_s16le.pcm    <- 云端应答超时提示
  prompt_fallback_16k_mono_s16le.pcm   <- 上行失败兜底提示

The boot greeting is spoken as four concatenated segments, because that keeps
24 hours x 7 weather conditions inside a few dozen short files instead of
hundreds of whole sentences:

  [时段问候] + [现在是X点] + [天气描述] + [有什么我可以帮你的吗？]

Every segment keeps its own time modifier ("上午7点" vs "晚上7点"): a shared
"现在是7点" clip is exactly what made the cloud-TTS greeting ambiguous.

Output format: 16 kHz mono signed 16-bit little-endian PCM.
The three intent confirmations stay under three seconds because they hold the
turn they acknowledge.  The session prompts may run longer, but they still have
to fit inside the ten-second session watchdog.

TTS priority:
  1. edge-tts   (offline cloud engine, MP3  ->  decoded/resampled to 16k s16le)
  2. pyttsx3    (local engine, WAV          ->  decoded/resampled to 16k s16le)
  3. placeholder beep (so CMake's embed_files still resolves even with no TTS
     engine; re-run once edge-tts/pyttsx3 is installed to replace it)

The voice is deliberately the same one the cloud agent speaks with (see
TTS_VOICE below).  The device plays these clips back-to-back with the cloud
reply, so any voice mismatch is heard as the speaker changing mid-conversation.
Rate matters just as much as the voice: at +0% the clips drag audibly slower
than the cloud reply and the whole greeting reads as sluggish, so both rates
carry a positive offset.  The three intent confirmations run faster than the
rest (see INTENT_RATE): the cloud's first audio packet lands about 1.4 s in on
average, and a four-syllable phrase has to fit comfortably inside that window
or it gets hard-cut mid-word.  Changing any of these constants requires
re-running this script - the .pcm files are the deliverable, not the text
above them.

Resampling relies on ffmpeg if present; otherwise pydub/av.  If every decoder
is missing the script still writes a placeholder tone so the build never
breaks on a missing asset.
"""
import array
import asyncio
import io
import math
import os
import shutil
import subprocess
import sys
import tempfile

SAMPLE_RATE = 16000
# Voice for every local prompt.  The line was switched to Xiaoxiao (曉曉),
# the mainstream Edge zh-CN female voice: it matches the voice the cloud
# agent is expected to speak with, so the local clips no longer read as a
# different speaker next to the reply.  TTS_RATE is deliberately above zero:
# at +0% every segment drags and the whole greeting reads as slow.
# Re-run this script after changing either constant, and keep *every* clip on
# the same voice - they are played back-to-back with the cloud reply.
TTS_VOICE = "zh-CN-XiaoxiaoNeural"
# 语速分两档，因为两类提示音的约束完全不同：
#   TTS_RATE    —— 开机问候 / 退出语 / 超时 / 兜底。+18% 是调出来的：原来 +0% 时
#                  四段问候拖到 7.6 s，紧挨云端回复显得迟钝。
#   INTENT_RATE —— 三个意图确认提示音专用。"正在执行"只有四个音节，却要在云端
#                  出声前占满约 0.95 s；实测云端首个音频包平均 1.39 s 就位
#                  （23 个样本，中位数 1.07 s，最快 0.32 s），余量太小，一旦被
#                  云端音频硬切就是半句话。提到 +35% 后约 0.78 s，余量够用，
#                  又不必牺牲三档区分度。
TTS_RATE = "+18%"
INTENT_RATE = "+35%"
# 整点报时专用语速。报时只播一句，不像四段问候那样需要赶节奏：+18% 时
# “现在是上午11点”只有 1.37 s，听感偏急；-33% 拉长到 2.45 s 才从容。
# 这个值只作用于 prompt_chime_hour_* 这一套素材，开机问候不受影响。
CHIME_RATE = "-33%"
CHIME_BITRATE = 24000
CHIME_FRAME_MS = 60
CHIME_FIRST_HOUR = 7
CHIME_LAST_HOUR = 21
OUT_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "main", "assets", "audio"))
# A local prompt must stay short so it never blocks the turn it acknowledges.
MAX_PHRASE_SECONDS = 3.0
MAX_PHRASE_BYTES = int(SAMPLE_RATE * 2 * MAX_PHRASE_SECONDS)
# The exit / timeout / fallback prompts are not turn confirmations: they play
# while the session is already closing, under the ten-second session watchdog
# (XIAOZHI_LOCAL_SESSION_PROMPT_DEADLINE_US), so they get a wider budget.
MAX_SESSION_PHRASE_SECONDS = 5.0

# Segment 1 - period greeting.  The BOOT_PERIOD_* enum in
# xiaozhi_voice_service.c maps these onto the live hour; "generic" is the
# degraded copy used while the clock has not been refreshed yet.
PERIOD_PHRASES = [
    ("prompt_greet_forenoon_16k_mono_s16le.pcm", "上午好"),
    ("prompt_greet_noon_16k_mono_s16le.pcm", "中午好"),
    ("prompt_greet_afternoon_16k_mono_s16le.pcm", "下午好"),
    ("prompt_greet_evening_16k_mono_s16le.pcm", "晚上好"),
    ("prompt_greet_night_16k_mono_s16le.pcm", "夜深了"),
    ("prompt_greet_generic_16k_mono_s16le.pcm", "你好呀"),
]

# Segment 2 - spoken clock.  The index is struct tm::tm_hour, so the firmware
# uses the live hour as a direct table index.  File names are spelled out one by
# one so CMake and the contract tests can verify them literally.
HOUR_PHRASES = [
    ("prompt_hour_00_16k_mono_s16le.pcm", "现在是半夜12点"),
    ("prompt_hour_01_16k_mono_s16le.pcm", "现在是凌晨1点"),
    ("prompt_hour_02_16k_mono_s16le.pcm", "现在是凌晨2点"),
    ("prompt_hour_03_16k_mono_s16le.pcm", "现在是凌晨3点"),
    ("prompt_hour_04_16k_mono_s16le.pcm", "现在是凌晨4点"),
    ("prompt_hour_05_16k_mono_s16le.pcm", "现在是凌晨5点"),
    ("prompt_hour_06_16k_mono_s16le.pcm", "现在是凌晨6点"),
    # 07 点说「凌晨」是错的：凌晨只覆盖 0-5 点，7 点天已经大亮。这条文案
    # 最早只服务开机问候，那时 7 点档没人细究；接进整点报时之后，每天 7:00
    # 都会把这句话播一遍，措辞错得很显眼。
    ("prompt_hour_07_16k_mono_s16le.pcm", "现在是早上7点"),
    ("prompt_hour_08_16k_mono_s16le.pcm", "现在是上午8点"),
    ("prompt_hour_09_16k_mono_s16le.pcm", "现在是上午9点"),
    ("prompt_hour_10_16k_mono_s16le.pcm", "现在是上午10点"),
    ("prompt_hour_11_16k_mono_s16le.pcm", "现在是上午11点"),
    ("prompt_hour_12_16k_mono_s16le.pcm", "现在是中午12点"),
    ("prompt_hour_13_16k_mono_s16le.pcm", "现在是下午1点"),
    ("prompt_hour_14_16k_mono_s16le.pcm", "现在是下午2点"),
    ("prompt_hour_15_16k_mono_s16le.pcm", "现在是下午3点"),
    ("prompt_hour_16_16k_mono_s16le.pcm", "现在是下午4点"),
    ("prompt_hour_17_16k_mono_s16le.pcm", "现在是下午5点"),
    ("prompt_hour_18_16k_mono_s16le.pcm", "现在是晚上6点"),
    ("prompt_hour_19_16k_mono_s16le.pcm", "现在是晚上7点"),
    ("prompt_hour_20_16k_mono_s16le.pcm", "现在是晚上8点"),
    ("prompt_hour_21_16k_mono_s16le.pcm", "现在是晚上9点"),
    ("prompt_hour_22_16k_mono_s16le.pcm", "现在是晚上10点"),
    ("prompt_hour_23_16k_mono_s16le.pcm", "现在是晚上11点"),
]

# Segment 3 - weather copy, matched against weather_service.c's weather_text().
WEATHER_PHRASES = [
    ("prompt_wx_clear_16k_mono_s16le.pcm", "天气晴朗"),
    ("prompt_wx_cloudy_16k_mono_s16le.pcm", "天气多云"),
    ("prompt_wx_fog_16k_mono_s16le.pcm", "天气有雾"),
    ("prompt_wx_rain_16k_mono_s16le.pcm", "有雨记得带伞"),
    ("prompt_wx_snow_16k_mono_s16le.pcm", "下雪注意保暖"),
    ("prompt_wx_thunder_16k_mono_s16le.pcm", "有雷雨注意安全"),
    ("prompt_wx_unknown_16k_mono_s16le.pcm", "天气暂时未知"),
]

# Segment 4 - the fixed help offer appended to every greeting.  It has no
# variants, so unlike the tables above it is a single entry.
CLOSING_PHRASES = [
    ("prompt_greet_closing_16k_mono_s16le.pcm", "有什么我可以帮你的吗？"),
]

BOOT_GREETING_PHRASES = PERIOD_PHRASES + HOUR_PHRASES + WEATHER_PHRASES + CLOSING_PHRASES

# (output file, spoken text).  Keep order stable so CMake/contracts stay fixed.
PHRASES = [
    ("prompt_executing_16k_mono_s16le.pcm", "正在执行"),
    ("prompt_querying_16k_mono_s16le.pcm", "正在查询"),
    ("prompt_thinking_16k_mono_s16le.pcm", "正在思考"),
] + BOOT_GREETING_PHRASES + [
    # Session prompts that formerly used the cloud [device_call] direct-TTS path.
    ("prompt_exit_16k_mono_s16le.pcm", "那我们下次再聊，祝你顺利。"),
    ("prompt_timeout_16k_mono_s16le.pcm", "我没听清，可以再说一遍吗？"),
    ("prompt_fallback_16k_mono_s16le.pcm", "刚刚有点问题，再说一次吧。"),
]

# The three prompts that play while the turn is still open: the only ones held
# to the tight three-second budget.
INTENT_ASSETS = {
    "prompt_executing_16k_mono_s16le.pcm",
    "prompt_querying_16k_mono_s16le.pcm",
    "prompt_thinking_16k_mono_s16le.pcm",
}


def _remove(path):
    try:
        os.remove(path)
    except OSError:
        pass


def _decode_to_s16le(media_bytes, suffix):
    """Best-effort conversion of arbitrary media bytes to 16k mono s16le PCM."""
    decoded = _decode_with_ffmpeg(media_bytes, suffix)
    if decoded is not None:
        return decoded
    decoded = _decode_with_pydub(media_bytes)
    if decoded is not None:
        return decoded
    return _decode_with_av(media_bytes)


def _decode_with_ffmpeg(media_bytes, suffix):
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        return None
    in_path = None
    out_path = None
    try:
        fd, in_path = tempfile.mkstemp(suffix=suffix)
        with os.fdopen(fd, "wb") as f:
            f.write(media_bytes)
        out_path = in_path + ".pcm"
        subprocess.run(
            [ffmpeg, "-y", "-i", in_path, "-ar", str(SAMPLE_RATE),
             "-ac", "1", "-f", "s16le", out_path],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        with open(out_path, "rb") as f:
            return f.read()
    except Exception:
        return None
    finally:
        _remove(in_path)
        _remove(out_path)


def _decode_with_pydub(media_bytes):
    try:
        from pydub import AudioSegment
        audio = AudioSegment.from_file(io.BytesIO(media_bytes))
        audio = audio.set_frame_rate(SAMPLE_RATE).set_channels(1).set_sample_width(2)
        return audio.raw_data
    except Exception:
        return None


def _decode_with_av(media_bytes):
    try:
        import av
        out = io.BytesIO()
        container = av.open(io.BytesIO(media_bytes))
        stream = container.streams.audio[0]
        resampler = None
        try:
            resampler = av.AudioResampler(format="s16", layout="mono", rate=SAMPLE_RATE)
        except Exception:
            resampler = None
        for frame in container.decode(stream):
            if resampler is not None:
                for rframe in resampler.resample(frame):
                    out.write(rframe.to_ndarray().astype("int16").tobytes())
            else:
                out.write(frame.to_ndarray().astype("int16").tobytes())
        container.close()
        return out.getvalue()
    except Exception:
        return None


# Concatenated segments would otherwise carry the whole TTS tail silence into
# every joint, which reads as a hesitation.  Keep a short, natural gap instead.
TRIM_HEAD_MS = 40
TRIM_TAIL_MS = 140
TRIM_THRESHOLD = 320


def _trim_silence(pcm):
    """Trims leading/trailing silence so segments join without long gaps."""
    if not pcm or len(pcm) % 2 != 0:
        return pcm
    samples = array.array("h")
    samples.frombytes(pcm)
    if sys.byteorder != "little":
        samples.byteswap()
    if not samples:
        return pcm
    head = 0
    while head < len(samples) and abs(samples[head]) <= TRIM_THRESHOLD:
        head += 1
    if head >= len(samples):
        return pcm
    tail = len(samples) - 1
    while tail > head and abs(samples[tail]) <= TRIM_THRESHOLD:
        tail -= 1
    head_keep = head - int(SAMPLE_RATE * TRIM_HEAD_MS / 1000)
    if head_keep < 0:
        head_keep = 0
    tail_keep = tail + 1 + int(SAMPLE_RATE * TRIM_TAIL_MS / 1000)
    if tail_keep > len(samples):
        tail_keep = len(samples)
    if tail_keep <= head_keep:
        return pcm
    samples = samples[head_keep:tail_keep]
    if sys.byteorder != "little":
        samples.byteswap()
    return samples.tobytes()


def _rate_multiplier(rate):
    """'+35%' -> 1.35, so the pyttsx3 fallback honours the same setting."""
    return 1.0 + float(rate.strip().rstrip("%")) / 100.0


def _edge_tts_pcm(text, rate):
    try:
        import edge_tts
    except Exception:
        return None
    try:
        communicate = edge_tts.Communicate(text, TTS_VOICE, rate=rate)

        async def _collect():
            buf = io.BytesIO()
            async for chunk in communicate.stream():
                if chunk["type"] == "audio":
                    buf.write(chunk["data"])
            return buf.getvalue()

        mp3 = asyncio.run(_collect())
        if not mp3:
            return None
        return _decode_to_s16le(mp3, ".mp3")
    except Exception:
        return None


def _pyttsx3_pcm(text, rate):
    try:
        import pyttsx3
    except Exception:
        return None
    wav_path = None
    try:
        engine = pyttsx3.init()
        # pyttsx3 没有倍率概念，把百分比映射到它的 wpm 旋钮上。这里原来是硬编码的
        # 190，等于无论资产该用什么语速都静默地按 190 合成。
        engine.setProperty("rate", int(190 * _rate_multiplier(rate)))
        engine.setProperty("volume", 1.0)
        fd, wav_path = tempfile.mkstemp(suffix=".wav")
        os.close(fd)
        engine.save_to_file(text, wav_path)
        engine.runAndWait()
        with open(wav_path, "rb") as f:
            wav = f.read()
        return _decode_to_s16le(wav, ".wav")
    except Exception:
        return None
    finally:
        _remove(wav_path)


def _placeholder_beep():
    """~1.1 s 660 Hz beep with raised-cosine edges, used only when no TTS works."""
    duration_s = 1.1
    freq = 660.0
    amp = 0.30
    ramp_s = 0.015
    total = int(SAMPLE_RATE * duration_s)
    length = total
    ramp = int(ramp_s * SAMPLE_RATE)
    out = bytearray()
    for i in range(total):
        env = 1.0
        if i < ramp:
            env = 0.5 * (1.0 - math.cos(math.pi * i / ramp))
        elif i >= length - ramp:
            env = 0.5 * (1.0 - math.cos(math.pi * (length - i) / ramp))
        v = amp * env * math.sin(2.0 * math.pi * freq * i / SAMPLE_RATE)
        v = max(-1.0, min(1.0, v))
        out += int(v * 32767.0).to_bytes(2, "little", signed=True)
    return bytes(out)


def _encode_opus_asset(pcm):
    """PCM -> 设备端可直接播放的裸 Opus 资产。

    格式：[u16 包数][u16 x N 包长][包数据连续存放]。

    设备端 esp_opus_dec 吃的是裸包，不解析 Ogg 容器，所以这里把容器拆掉
    只留包体。包长是变长的（静音段能被压到 3 字节），必须自带长度表。
    60 ms 一包解出来正好 1920 字节，压在 audio_playback_service 单次入队
    上限 2048 以内，所以设备端可以一包一入队。

    av 不是硬依赖：装不上就返回 None，由调用方跳过这一条。
    """
    try:
        import av
    except Exception:
        return None
    try:
        samples = SAMPLE_RATE * CHIME_FRAME_MS // 1000
        stride = samples * 2
        buf = io.BytesIO()
        # libopus 默认按 20 ms 切帧，设备端的解码器配的是 60 ms，帧长必须对齐。
        output = av.open(buf, "w", format="ogg")
        stream = output.add_stream("libopus", rate=SAMPLE_RATE,
                                   options={"frame_duration": str(CHIME_FRAME_MS)})
        stream.layout = "mono"
        stream.bit_rate = CHIME_BITRATE
        pos = 0
        while pos < len(pcm):
            chunk = pcm[pos:pos + stride]
            pos += stride
            if len(chunk) < stride:
                chunk += b"\x00" * (stride - len(chunk))
            frame = av.AudioFrame(format="s16", layout="mono", samples=samples)
            frame.sample_rate = SAMPLE_RATE
            frame.planes[0].update(chunk)
            for packet in stream.encode(frame):
                output.mux(packet)
        for packet in stream.encode(None):
            output.mux(packet)
        output.close()

        packets = []
        container = av.open(io.BytesIO(buf.getvalue()))
        for packet in container.demux():
            if packet.size > 0:
                packets.append(bytes(packet))
        container.close()
        if not packets:
            return None

        asset = bytearray()
        asset += len(packets).to_bytes(2, "little")
        for packet in packets:
            asset += len(packet).to_bytes(2, "little")
        for packet in packets:
            asset += packet
        return bytes(asset)
    except Exception:
        return None


def _write_chime_assets():
    """整点报时素材：单独一套慢速 Opus，与开机问候的 PCM 互不影响。"""
    for hour in range(CHIME_FIRST_HOUR, CHIME_LAST_HOUR + 1):
        text = HOUR_PHRASES[hour][1]
        name = "prompt_chime_hour_%02d_16k_mono_opus.bin" % hour
        data = _edge_tts_pcm(text, CHIME_RATE)
        if data is None:
            data = _pyttsx3_pcm(text, CHIME_RATE)
        if data is None:
            print("warning: no TTS engine for '%s'; skipping" % text)
            continue
        pcm = _trim_silence(data)
        asset = _encode_opus_asset(pcm)
        if asset is None:
            print("warning: Opus encode failed for '%s'; skipping" % text)
            continue
        out = os.path.join(OUT_DIR, name)
        with open(out, "wb") as f:
            f.write(asset)
        print("wrote %s: %d bytes (%.2f s PCM, %d packets)"
              % (out, len(asset), len(pcm) / float(SAMPLE_RATE * 2),
                 int.from_bytes(asset[0:2], "little")))


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    for name, text in PHRASES:
        rate = INTENT_RATE if name in INTENT_ASSETS else TTS_RATE
        data = None
        for engine in (_edge_tts_pcm, _pyttsx3_pcm):
            data = engine(text, rate)
            if data:
                break
        if data is None:
            print("warning: no TTS engine for '%s'; writing placeholder beep" % text)
            data = _placeholder_beep()
        else:
            data = _trim_silence(data)
        out = os.path.join(OUT_DIR, name)
        with open(out, "wb") as f:
            f.write(data)
        print("wrote %s: %d bytes (%.2f s)" % (out, len(data),
                                                len(data) / float(SAMPLE_RATE * 2)))
        limit_s = (MAX_PHRASE_SECONDS if name in INTENT_ASSETS
                   else MAX_SESSION_PHRASE_SECONDS)
        if len(data) > int(SAMPLE_RATE * 2 * limit_s):
            print("warning: '%s' is %.2f s and exceeds the %.1f s budget; shorten the phrase"
                  % (text, len(data) / float(SAMPLE_RATE * 2), limit_s))

    # 整点报时单独一套慢速 Opus：同一句话裸 PCM 要 78 KB，15 条就是 1.12 MB，
    # Opus 只要 5.5 KB 一条。这套素材跟上面的问候片段完全独立。
    _write_chime_assets()


if __name__ == "__main__":
    main()
