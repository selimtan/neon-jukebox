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

The build produces two standalone Windows applications:

- `neon_jukebox.exe` — the full-screen music and video jukebox.
- `neon_recorder.exe` — a companion recorder for the sound currently playing
  through the default Windows output device.

Jukebox allows only one running instance per Windows session, including when
launched from different folders. Opening the EXE again restores the existing
window and asks Windows to bring it forward. The startup lock is automatically
released on exit or a crash, so the application can be reopened normally.

## Recorder

Turn automatic mode off, press **KAYDI BAŞLAT**, and play the desired track in
Neon Jukebox. During a manual recording, enter the track title and optional
artist, album, and album year; the fields remain editable throughout capture. Press **DURDUR**, then choose
**WAV KAYDET** or **MP3 KAYDET**. WAV produces uncompressed 44.1 kHz,
16-bit stereo PCM (standard CD format). MP3 produces a high-quality 44.1 kHz
stereo file at 320 kbps. Title, artist, album, and year are written into WAV
`LIST/INFO` metadata using the Windows system code page (with a matching `CSET`
chunk) and UTF-8 ID3v2 tags. This keeps Turkish characters readable in Windows
File Explorer. When artwork is available it is embedded in both MP3 and WAV.

When either format is saved, the recorder scans the captured PCM and
automatically removes leading and trailing digital silence. A short safety
margin is retained around the detected audio so the first attack and final
decay of the music are not clipped. An entirely silent recording is rejected
instead of producing an empty output file.

The recorder captures the default Windows playback device through WASAPI
loopback. It does not record the microphone, but other sounds routed to that
same playback device (notifications, browser audio, and so on) are included.
For a clean recording, disable notification sounds while recording. Audio is
streamed to a temporary file and removed after saving or discarding, so long
tracks do not consume increasing amounts of memory.

### Automatic Spotify, Chrome, and YouTube recording

**OTOMATİK KAYIT** is enabled by default with Spotify selected. The source list
offers **SPOTIFY**, **CHROME**, **YOUTUBE**, and **AKTİF UYGULAMA**. The last option follows
whichever supported app owns the active Windows media session. Playback starts
capture automatically. A pause/stop ends the current capture after a short
debounce; a title change or playback timeline reset queues the old track and
immediately begins a new recording. The selected app's reported title, artist,
album, and artwork are copied into read-only fields. Spotify's artwork comes
directly from the Windows media session and does not require a web request.
Spotify's branded thumbnail frame is detected and removed before the clean
square cover is embedded. Jukebox also crops that frame while displaying older
recordings that already contain a branded thumbnail, without rewriting them.

For YouTube or YouTube Music, select **YOUTUBE** and play the video in **Chrome**.
This preset follows Chrome's Windows media session for playback, title, artist,
and available artwork, using the same automatic MP3/WAV recording flow. Windows
identifies the browser rather than the website, so the preset also follows other
media playing in Chrome; pause other tabs while recording YouTube. The audio
capture still includes all sound on the default Windows output device.

Spotify may publish a new title, artist, and timeline in closely spaced updates.
The recorder coalesces those updates into one track boundary. Artist-only
metadata changes never split a recording, and automatic fragments shorter than
three audible seconds are discarded without creating duplicate `(2)` files.

Automatic recordings are encoded on a background thread so the next track can
start capturing without waiting for the previous file. Choose automatic MP3
(320 kbps) or WAV (CD format) and use **KLASÖR SEÇ** to set the destination.
The default is `Music\Neon Recorder`. Raw recordings are created in the
selected folder's `_TMP` subfolder and removed after a successful save or
discard; a raw PCM file is retained there if automatic conversion fails. Each
finished recording is placed under artist and
album subfolders, for example `Neon Recorder\Haluk Levent\Bu Ateş
Sönmez\Haluk Levent - Yollarda Bulurum Seni.mp3`. Tracks without artist or
album metadata are grouped in `Sanatçısı Bilinmeyenler` or `Albümü
Bilinmeyenler`. Existing filenames are preserved by adding `(2)`, `(3)`, and so
on. If automatic saving fails, the raw temporary PCM path is shown instead of
silently deleting the recording.
While an automatic save is queued or encoded, Recorder holds the raw PCM file
open to prevent premature deletion. Transient access failures are retried for
two seconds; a persistent error reports the exact path and expected/available
file sizes while leaving recoverable data untouched.
Media Foundation's intermediate MP3 is also created with a short unique name in
`_TMP`; it no longer repeats long artist, album, and title text that can exceed
Windows path limits. The completed tagged file is moved into the album folder
only after encoding succeeds.
Automatic save decisions use the duration actually captured by Recorder, not
Spotify's reported track duration. After leading and trailing silence is
trimmed, recordings at or below two minutes are discarded without creating an
output file. Manual recordings are not subject to this automatic-mode rule.
Automatic mode, source, output format, and destination folder are saved as soon
as they change and restored the next time Recorder opens.
Enable **OTOMATİK GEÇ** to check the selected artist/album folder before a
capture starts. The exact `Artist - Title.mp3` or `.wav` (including numbered
copies) must also have a readable duration within ten percent of the duration
reported by Spotify. A match inside that range is not recorded and Recorder
sends the source application's Windows **Next** command. If the difference is
greater than ten percent, the track is recorded again and safely replaces the
older same-name file instead of creating a `(2)` copy. The old file is preserved
if the new recording cannot be saved. The option is off initially and its last
setting is remembered.
The persistent **Son otomatik kayıt** line shows the complete path of the last
file written, even while the following track is already being captured. Manual
WAV/MP3 save buttons are hidden in automatic mode because no confirmation is
required at a track boundary.

Metadata availability depends on the selected app and service. Spotify and
Chrome normally provide a title and artist, while album may be blank if it is
not published to Windows. Windows media sessions do not publish album release
year, so the year field stays blank in automatic mode; it can be filled for a
manual recording. In automatic mode the fields are read-only and show exactly
what the selected source reported. Embedded artwork is detected locally by
Neon Jukebox, so it does not need to search online for that recording. Turn
automatic mode off to make a manual recording with editable fields.

## First run

1. Create a 4-8 digit administrator PIN.
2. Add one or more **MUSIC SOURCES** containing MP3, OGG, FLAC, or WAV files.
3. Optionally add separate **VIDEO SOURCES** containing MP4, M4V, MOV, AVI,
   WMV, MKV, WebM, MPEG, or MPG files, then press **CONTINUE**.
4. Press **INSERT COIN +1**, tap a music track or video, and choose
   **ADD TO QUEUE**.

Tracks appear and can be queued as soon as they are found, while the remaining
folders keep scanning in the background. Café shuffle starts with the first
available track; new arrivals do not interrupt playback or reset the current page.

Admin **MUSIC SOURCES** and **VIDEO SOURCES** remain available during scanning.
Adding folders saves the expanded source list and cancels the current scan in
the background. Once it stops, one scan starts with the latest combined music
and video folders; further additions update that pending request. The existing
catalogue, current playback and request queue remain available throughout.

After each startup/library scan, tracks without embedded or nearby cover files
are grouped by album and searched through a six-source fallback chain:
MusicBrainz/Cover Art Archive, Apple Search, Deezer, TheAudioDB, Wikimedia
Commons, and Internet Archive. A miss or service error at one source continues
to the next source. Candidate artist and album/track names are scored before an
image is accepted, with accent-insensitive matching and fallbacks for edition
labels such as `Deluxe`, `Remastered`, and `Disc 1`. Featured-artist suffixes
are removed for album grouping so one album is not searched repeatedly.

Matching covers are stored in `library/artwork` beside the EXE. Local artwork
always has priority and downloads never block browsing or playback. Only artist
and album/title search text is sent; audio files are never uploaded. A complete
lookup, including a miss, is cached for that file version in `metadata.json`.
Later sessions replay saved metadata without repeating the search. Provider limits are respected with per-service
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

- In **ADMIN CONTROL → TEMA**, choose **MODERN** for the neon
  appearance or **RETRO** for the chrome wallbox. The choice applies
  immediately and is remembered after restart. Existing installations keep Neon.
  Retro has two ten-record pages (A0–A9 and B0–B9), green credit/playlist displays,
  red selection keys and a gold selected record. The codes refer to the current
  page; touch a title card, then **ADD TO QUEUE**. Search, genres, video, spinning
  CD, the visualizer and credit rules work in both themes. Design research and
  implementation notes are in `docs/THEMES.md`.
  Retro groups each pair of songs in one white insert, separated by grey metal
  and fixing dots. Numbered arrow selectors (1–20) also select the title beside
  them. Catalogue rows always use square artwork, with the bold artist centered
  above the regular-weight song title. Green screens retain the original monospace text.
  The spinning option becomes **SPINNING LP** in Retro: a black vinyl record with
  the album cover on its centre label. It sits below Now Playing and above the
  VU meter, followed by credits/time left, playlist and request buttons.

- Café shuffle starts automatically and keeps playing whenever the manual
  request queue is empty. After selecting **MUSIC** or **VIDEO**, shuffle uses
  only that section, including after the last credit is spent. This preference
  survives search/filter resets, queued requests from the other section and
  restarts. An empty section never falls back to the other media type.
- The on-screen **INSERT COIN +1** button simulates the future coin mechanism.
  Every press grants exactly one track request; two presses grant two requests.
- Visitor search, the paged **ALL GENRES** dropdown, Music/Video filters,
  paging, media selection, and **ADD TO QUEUE** stay
  disabled until at least one credit is available. Duplicate requests remain
  allowed when the visitor has enough credits.
- Genres are read from each file's TagLib metadata. Choosing a genre filters
  the current section; **MUSIC** or **VIDEO** can change that section.
  Tracks without genre metadata remain available under **ALL GENRES**.
- Records are ordered by artist, then title, with natural numeric ordering.
  In Retro, the **A–Z** row below search filters artist names by their first
  letter. **ALL** clears that filter; **0–9** shows artists starting with digits.
  Turkish and accented letters match their base letter (Ş → S, Ç → C, İ → I).
  The index combines with search, genre and media filters, uses the normal credit
  rules, and stays selected when background playback advances to another song.
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
  Neon has twenty-eight live, audio-reactive styles: Aurora Spectrum, Reference
  VU, Neon Arc VU, Mirror Stage, Chromatic Waterfall, Orbit Vinyl, Stereo
  Vector, Signal Ribbon, Studio LED, Precision Levels, CAVA Gravity, Prism
  Reflection, Phosphor Oscilloscope, Lissajous Studio, Radial Inferno, Circular
  Wave, Magma Spectrogram, MilkDrop Motion Mesh, Particle Galaxy, and Mastering
  Dashboard, Vintage Flat VU, OW Level Meter, Rackmount Spectrum, Green dB Meter, Spectrum Skyline, Neon Mosaic, Triple Sound Meter, and Warm Twin VU. Use **PREVIOUS**/**NEXT** or swipe horizontally; the selected style
  is remembered after restart. Their open-source research provenance and DSP
  design notes are recorded in `docs/VISUALIZER_RESEARCH.md`.
  Retro has its own **Retro Phosphor** waveform, with luminous yellow-green
  traces and a dark green display background. The picker shows only that theme's
  styles, and each theme remembers its own selection independently.
- Administrators can pause/resume, seek, skip, set volume, reorder or clear the
  queue, manage favorites, rescan the library, add multiple independent music
  and video source folders, change the PIN, and safely exit. Both source groups
  are pooled for the **ALL** view; duplicate file paths are indexed once. Café
  shuffle and continuous playback remain enabled.
- The administrator can choose **ARTWORK** or **SPINNING CD** for the music
  **NOW PLAYING** panel. Spinning CD maps each track's own artwork onto a
  circular disc with a metallic rim and centre hub, rotates slowly only while
  playback is active, pauses with the track, and is remembered after restart.

The `library` directory beside the EXE stores `library.json` (song/video tags),
`artwork` (downloaded covers and lookup results), and `video-thumbnails` (saved
PNG video stills). Unchanged videos reuse these files even after a restart.
At startup an independent background sweep prepares covers for the entire
cached library, including videos that have never been browsed. Embedded and
sidecar music covers are saved in `library/music-covers`; video stills use the
existing `video-thumbnails` directory. Completed file versions are skipped on
later launches. Scan updates and newly downloaded artwork enqueue only new or
changed versions; completed images refresh visible cards automatically. Music
and video preparation have separate low-priority workers and do not show an
overlay or require opening the Video section. Interrupted/offline work remains
eligible on the next launch or scan; no-cover results are remembered per version.
Videos on USB, optical or network drives are prepared on background workers in
`library/video-media` before decoding. This avoids random decoder reads and
30-second open timeouts on slow sources. Complete files are reused by path,
size and modification time; interrupted copies are never played. The cache
keeps up to 2 GiB of recent videos (or one larger video). Preparation progress
appears in the existing Now Playing metadata line; Skip and Exit cancel the
copy without blocking the interface. Videos on fixed local disks play directly.
During video shuffle, the next candidate is prepared while the current clip
plays. Failed candidates are replaced in the background; a ready or still
running preparation is handed to playback without copying the same file again.
Queued requests retain priority. Copy limits apply per source volume so a
stalled USB driver cannot block preparation from a healthy network source.
Settings, the request queue and `jukebox.log` stay under the directory returned
by `SDL_GetPrefPath("NeonJukebox", "NeonJukebox")`. Large catalogue writes and
periodic playback saves run on a background writer; pending writes finish on exit.
