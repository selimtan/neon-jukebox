"""Extract the two user-supplied reference effects without changing sample rate."""

import argparse
import array
import io
from pathlib import Path
import subprocess
import sys
import wave

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=Path)
parser.add_argument("--ffmpeg", default="ffmpeg")
parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "assets/sounds/retro")
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)

for name, start, end in [("coin.wav", 36.35, 37.10), ("page-turn.wav", 47.77, 48.27)]:
    decoded = subprocess.run([
        args.ffmpeg, "-hide_banner", "-loglevel", "error", "-i", str(args.source),
        "-vn", "-af", f"atrim=start={start}:end={end},asetpts=PTS-STARTPTS",
        "-c:a", "pcm_s16le", "-f", "wav", "pipe:1",
    ], check=True, capture_output=True).stdout
    with wave.open(io.BytesIO(decoded)) as source:
        rate, channels = source.getframerate(), source.getnchannels()
        samples = array.array("h", source.readframes(source.getnframes()))
    if sys.byteorder != "little":
        samples.byteswap()
    frames = len(samples) // channels
    fade = min(round(rate * .003), frames // 2)
    for frame in range(fade):
        gain = frame / max(1, fade - 1)
        for channel in range(channels):
            for offset in (frame * channels + channel, (frames - frame - 1) * channels + channel):
                samples[offset] = round(samples[offset] * gain)
    if sys.byteorder != "little":
        samples.byteswap()
    with wave.open(str(args.output / name), "wb") as target:
        target.setnchannels(channels)
        target.setsampwidth(2)
        target.setframerate(rate)
        target.writeframes(samples.tobytes())
    print(f"{name}: {frames / rate:.3f}s, {rate} Hz, {channels} channels, 3 ms fades")
