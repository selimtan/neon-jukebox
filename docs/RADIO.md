# Türkiye web radios

Choose **RADYO** beside Music and Video to load Turkish web radio stations in
the existing catalogue. As with music, insert a coin to enable browsing and
selection. The bottom A–Z buttons filter by the **station name**; Turkish letters
use their existing Latin equivalents (for example, Ş under S and İ under I).
Search, page navigation, and both Neon and Retro themes work with radio stations.

Choose a station and press **DINLE** (Retro) or **PLAY RADIO** (Neon) to start its live stream. A radio selection
uses one credit and plays immediately. Live broadcasts show **CANLI** instead
of a track duration. A later music or video request interrupts the broadcast so
the request does not wait forever for the station to finish. Radio stations do
not join the music/video shuffle pool. Administrator volume and pause controls
also apply to radio; seeking is unavailable for live broadcasts.

The catalogue is provided by [Radio Browser](https://www.radio-browser.info/),
using its [documented API](https://docs.radio-browser.info/) and country code
`TR`. The app requests stations reported as working, prefers resolved stream
addresses, and removes duplicate station names and streams. Directory checks do
not guarantee that every station remains available or supports playback on the
current Windows installation. MP3, AAC and HLS streams are opened through the
Windows MediaPlayer backend; a failed stream displays a message and another
station can be selected.

Fetching runs in the background. Stations are saved with the local catalogue
and remain visible if a refresh fails. Selecting RADYO retries after a failure
and refreshes successful lists after an hour; administrator **RESCAN** also
refreshes the active radio category. Internet access is needed for broadcasts.
Local music/video rescans preserve the radio catalogue and active station.
For a radio-only setup, continue past source-folder selection without adding a
folder.
