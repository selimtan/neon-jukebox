# Third-Party Notices

Neon Jukebox is built from the following pinned open-source components. Their
license texts are included in the `licenses` directory of the portable package.

- SDL 3.4.14, SDL_mixer 3.2.4, SDL_image 3.4.4, SDL_ttf 3.2.2 — zlib license
- TagLib 2.3.1 — Mozilla Public License 1.1 option
- nlohmann/json 3.12.0 — MIT license
- FreeType (vendored by SDL_ttf) — FreeType Project License
- HarfBuzz (vendored by SDL_ttf) — Old MIT license
- dr_mp3 and dr_flac (vendored by SDL_mixer) — public domain or MIT-0 option
- stb_vorbis and stb_image (vendored by the SDL libraries) — public domain or MIT option

The exact corresponding source revisions are declared in `CMakeLists.txt` and
can be obtained from their linked upstream Git repositories. No third-party
component was modified by this project.
