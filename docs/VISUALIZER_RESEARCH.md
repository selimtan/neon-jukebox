# Visualizer research and implementation notes

Research snapshot: 2 September 2026. Star counts naturally change over time.
The Neon Jukebox renderer is an original SDL_Renderer implementation; no source
files, shaders, presets, or artwork from the projects below are distributed.

## Primary projects inspected

| Project | Popularity at review | License | Techniques studied |
| --- | ---: | --- | --- |
| [CAVA](https://github.com/karlstav/cava) | 6,389 stars | MIT | Log-frequency bars, frame-rate-aware gravity falloff, integral smoothing, automatic sensitivity and Monstercat neighbour spread. |
| [projectM](https://github.com/projectM-visualizer/projectm) | 4,412 stars | LGPL-2.1 | PCM/FFT feature pipeline, bass/mid/treble envelopes, beat response, preset-driven layered rendering and motion meshes. |
| [Butterchurn](https://github.com/jberg/butterchurn) | 1,933 stars | MIT | MilkDrop-style warp meshes, motion vectors, decay/feedback concepts, layered waveforms and multi-pass glow. |
| [GLava](https://github.com/jarcode-foss/glava) | 1,273 stars | GPL-3.0 | Radial bars, circular waveforms, filled spectrum graphs, channel mirroring and gravity/average shader transforms. |
| [audioMotion-analyzer](https://github.com/hvianna/audioMotion-analyzer) | 946 stars | AGPL-3.0 | Peak hold/fade, logarithmic bands, LED/luminance bars, floor reflections, radial/inverted radial layouts and channel gradients. |
| [Pulse Visualizer](https://github.com/Audio-Solutions/pulse-visualizer) | 162 stars | GPL-3.0 | Phosphor persistence, Lissajous phase plots, triggered oscilloscopes, CQT/FFT spectrograms, peak/LUFS layouts and mastering dashboards. |
| [CAVA SourceForge mirror](https://sourceforge.net/projects/cava.mirror/) | SourceForge mirror | MIT | SDL presentation and configurable bar-spectrum behaviour. |
| [AUDio MEasurement System](https://sourceforge.net/projects/audmes/) | 11 reviews | Project repository | Measurement-style oscilloscope, spectrum grid and frequency-response presentation. |
| [sirdarides/VUMeter](https://github.com/sirdarides/VUMeter) | 9 stars at review | No license declared | Dual flat channel layout, green/red trapezoid scale, grey live needle and red maximum needle returning at silence. Reviewed as a visual reference only; no code or assets are copied. |
| [MarcoRavich/OWLevelMeter](https://github.com/MarcoRavich/OWLevelMeter) | 0 stars at review | No license declared | OpenWrt-oriented stereo -60–0 dB bars, green/orange/red zones, 2.5-second peak hold, clipping alert, numeric level, spectrum, phase and analysis cards. Reviewed as a visual reference only; no code or assets are copied. |

Popularity was used as a discovery signal, not as permission to copy. Only
general signal-processing and visualization techniques were reimplemented.

## Neon Jukebox modes

1. Aurora Spectrum — 64-band layered gradient with timed peak hold.
2. Reference VU — dual analog ballistic meters with calibrated tick geometry.
3. Neon Arc VU — segmented stereo arcs with inertial level response.
4. Mirror Stage — center-split spectrum and subdued floor reflection.
5. Chromatic Waterfall — scrolling frequency history with depth fade.
6. Orbit Vinyl — radial spectrum around a rotating record treatment.
7. Stereo Vector — L/R goniometer for stereo width and phase.
8. Signal Ribbon — dual-channel waveform with edge envelope and glow.
9. Studio LED — classic green/amber/red real-time analyzer matrix.
10. Precision Levels — broadcast RMS bars with true-peak markers.
11. CAVA Gravity — gravity falloff and Monstercat-style spectral spreading.
12. Prism Reflection — luminance-driven rainbow bars and floor reflection.
13. Phosphor Oscilloscope — zero-crossing-stabilized waveform with persistence.
14. Lissajous Studio — stereo phase portrait with directional coloring/trails.
15. Radial Inferno — beat-reactive radial FFT with perceptual heat gradient.
16. Circular Wave — dual-channel circular waveform plus spectral modulation.
17. Magma Spectrogram — 64-band time history with perceptual magma palette.
18. MilkDrop Motion Mesh — bass/mid-reactive warped mesh and central waveform.
19. Particle Galaxy — beat-driven spectral particles, depth and orbital trails.
20. Mastering Dashboard — scope, spectrum, phase plot and stereo meters together.
21. Vintage Flat VU — dual black flat meters with a tapered green/red scale,
    weighted grey live needles, persistent red maximum needles, calibrated dB
    markings and a restrained glass reflection.
22. OW Level Meter — responsive OpenWrt-style monitoring dashboard with stereo
    -60–0 dB bars, 2.5-second peak hold, clip memory, numeric RMS, a 48-band
    spectrum, stereo phase correlation and signal analysis cards.
23. Rackmount Spectrum — a black 1U hardware faceplate based on the supplied visual
    reference, with a metal level knob, 31-band rainbow LED matrix, dark inactive
    segments, and per-band peak caps.
24. Green dB Meter — a minimal black-and-green analyzer based on the supplied visual
    reference, with nine logarithmic bands from 60 Hz to 16 kHz, stacked phosphor
    segments and retro frequency legends.
25. Spectrum Skyline — a black-stage LED analyzer based on the supplied visual
    reference, with green/amber/red level zones, narrow segmented columns and a
    dark perspective floor reflection.
26. Neon Mosaic — a tightly cropped multicolor analyzer based on the supplied visual
    reference, with independent column hues, dark upper segments and vivid lower
    chroma on a pure black field.
27. Triple Sound Meter — a three-channel instrument panel based on the supplied
    visual reference, combining miniature tri-color spectrum displays with metal
    analog dB bezels, calibrated scales and responsive red needles.
28. Warm Twin VU — a classic two-channel rack meter based on the supplied visual
    reference, with thick black bezels, warm ivory backlighting, calibrated black
    scales, red overload markings and weighted black needles.

## Signal pipeline

- The audio callback only copies PCM into a fixed-size lock-free publication
  buffer; rendering, FFT-like Goertzel analysis and allocations remain outside
  the real-time callback.
- A 1,024-sample Hann window feeds 64 logarithmic bands from 45 Hz to 18 kHz.
- Display bands have independent attack/release smoothing, CAVA-style gravity
  state and 480 ms peak hold followed by accelerating falloff.
- Bass, mid and treble envelopes drive rotation/warp intensity. A transient
  bass detector generates a decaying beat pulse for modes that need it.
- Seventy-two spectrum history rows and ten stereo waveform history frames feed
  spectrogram, waterfall and phosphor-persistence modes.
