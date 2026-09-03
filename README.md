# Neon Jukebox

A touch-first, full-screen Windows music-and-video jukebox built with C++20,
SDL3, SDL_mixer, SDL_image, SDL_ttf, TagLib, and Windows Media Foundation.

## Build

Run PowerShell from the repository root:

```powershell
.\scripts\build.ps1
```

The script checks for Visual Studio 2022 C++ Build Tools and CMake, configures
a 64-bit Release build, runs the unit tests, and creates a portable ZIP in
`build/package`.

If the prerequisites are missing, install them with:

```powershell
winget install --id Kitware.CMake --exact
winget install --id Git.Git --exact
winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --override "--wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

The first configure downloads pinned source releases for the dependencies.
The packaged application runs without internet access. When a connection is
available, it can enrich missing artwork in the background as described below.

## First run

1. Create a 4-8 digit administrator PIN.
2. Add one or more **MUSIC SOURCES** containing MP3, OGG, FLAC, or WAV files.
3. Optionally add separate **VIDEO SOURCES** containing MP4, M4V, MOV, AVI,
   WMV, MKV, WebM, MPEG, or MPG files, then press **CONTINUE**.
4. Press **INSERT COIN +1**, tap a music track or video, and choose
   **ADD TO QUEUE**.

After each startup/library scan, tracks without embedded or nearby cover files
are grouped by album and searched through a six-source fallback chain:
MusicBrainz/Cover Art Archive, Apple Search, Deezer, TheAudioDB, Wikimedia
Commons, and Internet Archive. A miss or service error at one source continues
to the next source. Candidate artist and album/track names are scored before an
image is accepted, with accent-insensitive matching and fallbacks for edition
labels such as `Deluxe`, `Remastered`, and `Disc 1`. Featured-artist suffixes
are removed for album grouping so one album is not searched repeatedly.

Matching covers are stored in the application's `artwork` cache. Local artwork
always has priority and downloads never block browsing or playback. Only artist
and album/title search text is sent; audio files are never uploaded. A complete
six-source miss is cached for 14 days, while temporary network failures remain
eligible for the next scan. Provider limits are respected with per-service
request pacing. Details and official API references are in
`docs/ARTWORK_SOURCES.md`.

The same validated online match fills only missing library metadata: artist,
album, genre, and album year. Existing file tags always win and source MP3 files
are never rewritten. Genre choices appear incrementally while local scanning and
online enrichment continue in the background.

Move the pointer to the extreme top-left corner to reveal the **ADMIN** button,
then click it and enter the administrator PIN.
The kiosk lock is application-level protection, not Windows Assigned Access.

## Controls

- Café shuffle starts automatically and keeps playing whenever the manual
  request queue is empty.
- The on-screen **INSERT COIN +1** button simulates the future coin mechanism.
  Every press grants exactly one track request; two presses grant two requests.
- Visitor search, the paged **ALL GENRES** dropdown, Music/Video filters,
  paging, media selection, and **ADD TO QUEUE** stay
  disabled until at least one credit is available. Duplicate requests remain
  allowed when the visitor has enough credits.
- Genres are read from each file's TagLib metadata. Choosing a genre filters
  the complete media pool; **MUSIC** or **VIDEO** can then narrow that result.
  Tracks without genre metadata remain available under **ALL GENRES**.
- When a request is added during café shuffle, the visitor can either start it
  immediately or wait for the background track to finish. Manual requests then
  play in FIFO order before café shuffle resumes.
- Music and video requests use the same FIFO queue. A playing video appears in
  the square **NOW PLAYING** area; tap the video to expand it to the whole screen
  and tap it again to return to the compact view. Video decoding uses the codecs
  included with Windows; unsupported or damaged files are skipped safely. During
  video playback, WASAPI loopback feeds the video's actual stereo output into the
  same VU, spectrum, waveform, and oscilloscope analyzer used for music.
- Tap the compact meter below **NOW PLAYING** to open the full-screen visualizer.
  Twenty-eight live, audio-reactive styles are included: Aurora Spectrum, Reference
  VU, Neon Arc VU, Mirror Stage, Chromatic Waterfall, Orbit Vinyl, Stereo
  Vector, Signal Ribbon, Studio LED, Precision Levels, CAVA Gravity, Prism
  Reflection, Phosphor Oscilloscope, Lissajous Studio, Radial Inferno, Circular
  Wave, Magma Spectrogram, MilkDrop Motion Mesh, Particle Galaxy, and Mastering
  Dashboard, Vintage Flat VU, OW Level Meter, Rackmount Spectrum, Green dB Meter, Spectrum Skyline, Neon Mosaic, Triple Sound Meter, and Warm Twin VU. Use **PREVIOUS**/**NEXT** or swipe horizontally; the selected style
  is remembered after restart. Their open-source research provenance and DSP
  design notes are recorded in `docs/VISUALIZER_RESEARCH.md`.
- Administrators can pause/resume, seek, skip, set volume, reorder or clear the
  queue, manage favorites, rescan the library, add multiple independent music
  and video source folders, change the PIN, and safely exit. Both source groups
  are pooled for the **ALL** view; duplicate file paths are indexed once. Café
  shuffle and continuous playback remain enabled.
- The administrator can choose **ARTWORK** or **SPINNING CD** for the music
  **NOW PLAYING** panel. Spinning CD maps each track's own artwork onto a
  circular disc with a metallic rim and centre hub, rotates slowly only while
  playback is active, pauses with the track, and is remembered after restart.

Settings and library state are stored under the directory returned by
`SDL_GetPrefPath("NeonJukebox", "NeonJukebox")`.
Downloaded artwork is stored in its `artwork` subdirectory. Unreadable files
are skipped and recorded in `jukebox.log` in that directory.
