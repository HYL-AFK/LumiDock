# Generates the 2 s local "error tone" PCM asset played before a failed
# voice turn closes and returns to the main page.
#   Format: 16 kHz mono signed 16-bit little-endian, 64000 bytes.
#   Pattern: four 150 ms beeps alternating 880 Hz / 587 Hz (A-B-A-B),
#            each with 10 ms raised-cosine ramps, then silence to 2 s.
import math
import os

SAMPLE_RATE = 16000
DURATION_S = 2.0
BEEP_LEN_S = 0.150
GAP_S = 0.100
FREQS = [880.0, 587.0, 880.0, 587.0]
AMP = 0.30
RAMP_S = 0.010

TOTAL = int(SAMPLE_RATE * DURATION_S)
samples = [0.0] * TOTAL


def add_beep(start_s: float, freq: float) -> None:
    start = int(start_s * SAMPLE_RATE)
    length = int(BEEP_LEN_S * SAMPLE_RATE)
    ramp = int(RAMP_S * SAMPLE_RATE)
    for i in range(length):
        idx = start + i
        if idx >= TOTAL:
            break
        env = 1.0
        if i < ramp:
            env = 0.5 * (1.0 - math.cos(math.pi * i / ramp))
        elif i >= length - ramp:
            env = 0.5 * (1.0 - math.cos(math.pi * (length - i) / ramp))
        samples[idx] += AMP * env * math.sin(2.0 * math.pi * freq * i / SAMPLE_RATE)


t = 0.0
for freq in FREQS:
    add_beep(t, freq)
    t += BEEP_LEN_S + GAP_S

out = bytearray()
for s in samples:
    v = max(-1.0, min(1.0, s))
    out += int(v * 32767.0).to_bytes(2, "little", signed=True)

out_path = os.path.join(os.path.dirname(__file__), "..", "main", "assets", "audio",
                        "error_tone_16k_mono_s16le.pcm")
out_path = os.path.normpath(out_path)
with open(out_path, "wb") as f:
    f.write(out)
print("wrote %s: %d bytes" % (out_path, len(out)))
