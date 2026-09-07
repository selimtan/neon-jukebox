# Jukebox theme plan and research

## Scope and design decisions

The theme choices are named **Modern** and **Retro** without numbers. Modern
uses the existing neon appearance and retains its stored `neon` ID. Select either
in the PIN-protected administrator panel; apply immediately
and save in `settings.json`. Existing settings default to Neon. Unknown or
non-string theme values fall back to Neon without resetting unrelated settings.

The supplied screenshot is a visual reference: two hinged catalogue pages,
red A0–A9/B0–B9 rails, pale title cards, a metal cabinet, and a green display
console. Its text is reference content, not application requirements.

## Research (5 September 2026)

- [Seeburg wallboxes, illustrated model archive](https://jukeboxhistory.info/seeburg/wallboxes.html):
  the 3W1 examples show ivory or chrome finishes, red pushbuttons and printed
  selection strips. Use a chrome frame, physical-looking keys and a two-page
  catalogue, with full-row touch targets.
- [Seeburg 3W1 restoration photographs](https://www.goodstuffnowllc.com/3w1-5736.html):
  polished borders, title windows and separate alphabet/numeric controls informed
  the recessed cards and selection-code rails.
- [Original Rock-Ola 1544/1546 service manual](https://www.jukeboxhistory.info/rock-ola/wallboxes/rock-ola_1544_1546_service-manual.pdf):
  search-indexed parts entries identify the chrome front, wall-box window and
  coin-insert parts. The archive challenged direct access, so the design relies
  on the accessible Seeburg photographs and supplied screenshot for visual detail.
- [Retro Jukebox software reference](https://www.arcadepunks.com/touchscreen-pc-cab-friendly-jukebox-software-retro-jukebox/):
  cabinet/touchscreen software context for the user's example. The green digital
  displays come from that reference style; this is a hybrid retro skin, not a
  claim that original 1950s wallboxes had digital screens.

No third-party screenshots, logos or fonts are redistributed. Cabinet materials,
screws, hinges, highlights and display scanlines are drawn natively with SDL.
Actual library covers supply the album images. Retro uses Windows' Arial Narrow
Bold when available, with Bahnschrift and the existing font as fallbacks;
the digital display uses the existing monospace font.

## Reference video effects

### Retro video catalogue (6 September 2026)

The Video filter shows two large VHS items per page, one on each hinged leaf.
The printed sleeves put the artist in the top label and the clip title below
the real video thumbnail, with duration inside the picture. Untagged "Artist -
Clip" titles use the same artist/title labels for display, search, sorting and
the A–Z index in both themes. Saved source tags are unchanged; artist metadata takes
precedence, and official-video/HD suffixes are omitted while mix/live names stay.
Exposed black cassettes have ribs, screws, two spool windows and
cream selection labels. Gold edges mark the selected video. Music remains the
20-title catalogue; Modern retains its nine-card layout. Paging, prefetching,
automatic track focus and theme changes all use the active catalogue capacity.
Genre and text searches continue to filter the active video collection.
An empty filtered result shows a no-matches notice even during a background scan;
the initial collection-building notice is reserved for an empty, unfiltered library.

References: [VHS tape/sleeve scans](https://www.fallinsideahole.com/collections/audio-video/tapes/vhs/vhstapes.html)
and [TDK TV shell photographs](https://goughlui.com/the-vhs-corner/vhs-cassette-library/tdk-tv/).
The design is native SDL geometry; no manufacturer artwork is bundled.

Video stills use Windows' `IShellItemImageFactory` thumbnail provider on dedicated
background workers, sharing the OS thumbnail cache without opening a player.
If the thumbnail is missing or nearly blank, a separate Media Foundation source
reader seeks into the clip to obtain a more useful still without playing audio.
Stills are persisted as PNG files in `library/video-thumbnails` beside the EXE,
keyed by source path, file size and modification time. Restarts reuse them without
opening the source video. Video page turns never wait for extraction; uncached or
unsupported videos use an analogue test card while the background work finishes.
Extraction follows the [Windows image-factory guidance](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishellitemimagefactory-getimage).
`neon_ui_tests --preview-vhs <state-directory> <output-directory>` validates real
video extraction, two-item selection, an odd last page and three output sizes.

The user-supplied **Retro-Jukebox beta 1** video provides two sound excerpts:
`assets/sounds/retro/coin.wav` (36.35–37.10 s) and `page-turn.wav`
(47.77–48.27 s). Both precede music playback. They retain 44.1 kHz stereo PCM
with 3 ms fades; `scripts/extract_retro_effects.py` reproduces the cuts.
The source video is not packaged. See the accompanying `SOURCE.md` for attribution.

Effects are predecoded on audio initialization and use separate SDL_mixer voices
at 35% gain, following the master volume. Three coin voices support repeated
inserts, and a dedicated page voice leaves music playback and completion events
untouched. A coin sound only plays when credit actually increases (up to 99).
The page sound fires once at 65% of the displayed 680 ms turn, never on rejected
clicks or after a cancelled transition. Leaving Retro stops effect voices.
Missing/corrupt effects remain nonfatal and are reported in `jukebox.log`.

Retro controls use red keys with a full-face hover highlight, gold focus
borders and a depressed bevel on press. A short release response remains visible
for quick taps. The status display moves changed messages vertically over 180 ms;
waiting prompts blink with a 1,600 ms period. The search keyboard uses a metal
panel, green input display and the same red keys over a dimmed cabinet. Its
existing actions and the administrator-only controls retain their access rules.

## Required components

1. Theme catalogue: stable setting IDs, names, page capacity; default `neon`.
2. Admin theme selector: two visual choice cards, explicit selected state.
3. Retro cabinet: brushed silver surround, recessed paper windows and central
   hinge; red code rails with a gold selected-card highlight.
4. Catalogue: two columns of ten real tracks with square artwork and two centered
   lines: bold artist above regular-weight song title. Codes identify slots on the
   current page. Empty slots are visibly inactive.
   Each metal leaf contains five white inserts, each holding two tracks with a
   fine red dividing line. Grey gaps and three small fixing dots separate inserts.
   Outlined arrow selectors numbered 1–20 select the corresponding record, alongside
   the A0–B9 rails and square artwork. Printed titles and artists use condensed capitals.
   Selector borders use a closed stroke mesh with joined corners and alpha-softened
   edges. Their visible stroke width follows physical pixels so small screens retain
   every edge without the gaps of single-pixel line primitives.
5. Navigation: search, genre popup, music/video filters, an artist A–Z index, previous/next pages and
   page/result count. Long and Turkish metadata must remain legible.
6. Console: green status display, credit count, current-track remaining time, real FIFO
   queue with overflow count, current artwork/spinning CD/video, progress/time,
   live visualizer, coin and add-to-queue controls.
   The TIME LEFT display follows actual playback (music or video), stays fixed
   when paused, clamps at zero and shows `--:--` when the duration is unavailable.
7. Shared dialogs: PIN, admin, search keyboard, genre and play-now confirmation
   retain their actions with theme-appropriate colours and buttons.
8. Behaviour: visitor credit gating, modal hit blocking, admin hot corner,
   persisted artwork/visualizer options, queue and playback remain functional.
9. Validation: build, storage backward compatibility, theme page capacities,
   hit targets, disabled controls, modal blocking, last-page/empty states,
   video geometry and rendered screenshots at multiple output sizes.

Library scanning and artwork lookup continue in the background without a bottom
status strip. The Retro cabinet uses the freed height for taller catalogue rows
and the console meter, with the bottom controls aligned to a 20 px outer margin.
The search, genre and media controls start at the top of the left cabinet, with
the artist index directly below. The former wordmark/tagline area is removed;
its 72 logical pixels extend the catalogue, with artwork and labels centered
within the taller rows.
The console starts with the larger green Now Playing display, followed by
credit/time-left, the spinning vinyl/artwork/video, the responsive visualizer,
the playlist and request buttons. The display includes the title, artist, album and year, elapsed
and total time, and a phosphor-green progress bar inside its bottom edge. Playback
information cycles for 20 seconds, followed by a 5-second PLEASE / INSERT COIN
reminder whose main text blinks on/off every 500 ms. Then the information returns
for another 20 seconds. A new track or entering Retro starts a fresh cycle;
the progress bar and playback keep running throughout. The 134 px visualizer
adapts its density to the available on-screen area.

Visualizer catalogues and selections are separate for each theme. Neon owns the
original 28 styles and retains the existing `visualizerMode` preference. Retro
offers one style, **Retro Phosphor**, stored as `retroVisualizerMode`. It uses the
reference oscilloscope waveform with fading trails on the same dark green gradient
as the status displays; the trace is yellow-green phosphor. The picker shows only
the active theme's styles and hides cycling controls when there is one choice.
Legacy settings retain their selection for Neon and default Retro to its own meter;
invalid or cross-theme selections fall back within the appropriate catalogue.
Changing themes does not overwrite either preference.

The saved spinning-disc option renders as a vinyl LP in Retro (labelled SPINNING
LP in its admin panel). The main record has a
black grooved surface, subtle fixed reflections, a small central album-art label
and a spindle hole. Only the large label rotates during playback; pausing stops
it. No rectangular frame surrounds the vinyl. Retro catalogue thumbnails always
use normal square artwork, independent of this setting. Neon retains its original CD.

Both themes place the artist index directly below search/genre/media controls.
Neon's navigation spans the full width above its three panels, without a wordmark
or separate media-count header. ALL clears the artist filter; 0–9 selects artists starting with a digit.
A–Z matches the start of the artist name, ignoring case and accents (including
Ç/C, Ş/S, Ö/O, Ü/U and İ/ı/I). It combines with the existing search, genre and
media filters and resets to page one on selection. Empty results retain the
index so visitors can choose another letter. The buttons use the same credit
and modal rules as search and consume no credits. Background playback changes
leave artist results in place while credit remains. With zero credits, newly
playing queued tracks follow the same catalogue focus as shuffle: their page
opens and their row is selected. Matching filters remain; filters hiding the
track are cleared. Switching themes preserves the selected artist filter.
Both themes sort by artist, then title, then album using natural numeric order
(2 before 10), including cached libraries and incremental scan arrivals.

## Implementation notes

`Theme.hpp` holds the catalogue. `UIRetro.cpp` owns the cabinet renderer and
material primitives; the original browse renderer remains in `UI.cpp`.
`UI::nowPlayingMediaRect` supplies both the UI and native video window with the
same geometry. Theme changes remap the current page using the visible selection,
or the old page's first item, without changing the search, queue or playback.
The Retro video frame spans the console's full 436-unit width, aligned with the
Now Playing display and meter. Its inset native video surface preserves the
picture's aspect ratio; music artwork and the vinyl keep their existing size.
The coin button reads INSERT COIN; the credit display supplies its feedback.
Neither theme draws floating notification banners over the controls. Setup and
administrator feedback stays inline in free panel space, and PIN errors use the
existing PIN prompt.
Adding a different layout requires a catalogue entry and renderer; skins are
compiled into this native application, not loaded from external theme packages.

The administrator's artwork setting controls the main playback display. Neon
also applies Spinning CD to its music catalogue thumbnails. Retro catalogue rows
always use square covers and centered artist/title labels; duration and year stay
in the playback console instead of occupying a corner of the printed title strip.

Retro catalogue navigation turns a rigid metal leaf around the center hinge in
680 ms: Next rotates the right leaf to the left, and Previous reverses it. The
old face and the new reverse face use separate rendered spreads, a subdivided
perspective projection, moving shadow, directional shading and a chrome edge.
Both directions lift toward the viewer, using a 4800-unit camera distance and
positive depth for a subtle forward perspective within the cabinet. The moving
leaf is drawn above its shadow, the stationary pages and the fixed hinge. The
hinge stays visible only where the moving leaf does not cover it, and is fully
visible again when the turn settles. The cached page faces leave the pivot gap
transparent so it does not turn into a grey bar beside the hinge; the fixed
hinge is drawn above the cast shadow and below the actual page frame.
The reference-style hinge is a continuous narrow chrome cylinder with rounded
ends, recessed side channels and nine fine curved knuckle joints. Its reflections
and seam highlights are drawn with native geometry, replacing the broad crossbars.
Raised upper corners can pass in front of the artist index; clipping uses the
outer cabinet instead of the resting page opening so the full top edge stays visible.
Playback, the console and the hinge remain live. Track selection and additional
page presses are blocked until the leaf settles; delayed mouse events from that
interval are discarded instead of replayed on the new page. Animation advances
by at most 50 ms per rendered frame, so a slow frame cannot skip the entire turn.
Search/filter changes, library replacement, dialogs, resizing or leaving Retro
cancel the transition; incremental scan arrivals can update its destination. Only
explicit Previous/Next actions animate, so automatic playback focus and search
result changes stay immediate. Renderer targets follow the output resolution
and are released on exit; unsupported targets fall back to immediate paging.

Both themes prefetch two pages in each direction plus a third in the direction
of travel, using up to four background decoders and a 128-entry LRU cache.
The requested destination takes priority over nearby
pages; obsolete queued work is cancelled on filter changes. Previous/Next keeps
the current music page until every destination cover is ready, then displays the page
as a whole (and starts the Retro leaf animation). Repeated clicks during this
preparation are ignored; playback and other controls stay responsive. Missing
artwork finishes with a generated cover so it cannot hold up navigation.
Retro video pages bypass this gate so slow video codecs cannot block Next/Previous.
Large artwork is resized before uploading at most eight textures or four
milliseconds of uploads per frame. Old results cannot overwrite a newly requested
cover. Static catalogue spreads are reused
until their labels, selection, display mode or artwork change, avoiding repeated
CD geometry and text drawing during the turn. Offscreen cover uploads do not
invalidate the visible Retro spread.

The UI tests use a supplied frame clock to verify both directions, old/new face
visibility, burst/stale input blocking, long-frame recovery, interruption, Neon behavior and the partial last
page at 1920x1080, 1280x720 and 1024x768. Passing an output directory to
`neon_ui_tests` also writes transition stills and a 50 fps preview frame sequence.
`neon_ui_tests --profile-paging <state-directory>` measures first and subsequent
frames against an existing library using a hidden hardware renderer; it only
reads that library. Regression checks also cover blocked artwork decoding,
eviction during decoding, cleanup, and Unicode filenames in the saved library.
