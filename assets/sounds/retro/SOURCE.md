# Retro sound references

Extracted at the user's request from their supplied `Retro-Jukebox beta 1.mp4`.
Reference: Jérôme Boulinguez, [Retro-Jukebox beta 1](https://www.youtube.com/watch?v=8GAOxwznX5w).

| File | Source interval | Use |
| --- | --- | --- |
| coin.wav | 00:36.35–00:37.10 | Successful credit insertion |
| page-turn.wav | 00:47.77–00:48.27 | Metal page closing |

These intervals precede music playback. PCM WAV preserves the source's
44,100 Hz stereo format, with a 3 ms linear fade at each cut boundary.
`scripts/extract_retro_effects.py` reproduces these files from the supplied MP4
using FFmpeg. The source video itself is not included in the application.
