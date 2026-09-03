# Online artwork sources

Neon Jukebox uses these providers in order and stops as soon as a validated
image is downloaded:

1. **MusicBrainz + Cover Art Archive** — release-group metadata and canonical
   front artwork. API: https://musicbrainz.org/doc/MusicBrainz_API and
   https://musicbrainz.org/doc/Cover_Art_Archive/API
2. **Apple Search API** — album or song search with Store artwork. API:
   https://developer.apple.com/library/archive/documentation/AudioVideo/Conceptual/iTuneSearchAPI/
3. **Deezer Search API** — album and track results with album cover variants.
   Developer site: https://developers.deezer.com/api
4. **TheAudioDB** — exact artist/album or artist/track lookup. API:
   https://www.theaudiodb.com/free_music_api
5. **Wikimedia Commons** — file search through the MediaWiki API. API:
   https://commons.wikimedia.org/wiki/Commons:API/MediaWiki
6. **Internet Archive** — metadata search followed by the item's image service.
   Developer resources: https://archive.org/developers/

## Matching and failure behavior

- The local priority remains embedded image, nearby `cover`/`folder`/`front`
  file, downloaded image, then the generated neon record.
- Online candidates must match both artist and album/track. Matching is Unicode
  case-insensitive, accent-insensitive, and tolerant of common edition suffixes.
- The background worker continues after a provider miss, malformed response,
  rejected candidate, invalid image, HTTP failure, or download failure.
- A temporary failure is retried up to three times and is not written to the
  negative cache. Only a completed pass in which every provider misses is
  cached for 14 days.
- The negative-cache namespace is versioned. Changing the provider strategy can
  invalidate old misses without deleting successful artwork.
- Provider-specific pacing stays below documented public request limits where a
  numerical limit is published.
- Requests contain artist and album/title text only. Music and video contents,
  paths, queue data, PIN data, and user settings are never sent.

## Metadata enrichment

- A validated result may fill an otherwise missing artist, album, genre, or
  album year in the local library cache. Existing embedded tags are not replaced.
- Apple supplies `artistName`, `collectionName`, `primaryGenreName`, and the year
  portion of `releaseDate`; MusicBrainz supplies artist credit, first release date,
  and genre data; TheAudioDB supplies `strGenre` and `intYearReleased`.
- Compilation albums fall back to a conservatively matched representative track
  search when the album title cannot provide a genre.
- Enrichment updates the genre dropdown as results arrive. It does not modify the
  user's MP3, FLAC, OGG, WAV, or video files.

Artwork can be copyrighted or subject to provider-specific terms. Operators are
responsible for ensuring that displaying downloaded images is permitted in
their venue and jurisdiction.
