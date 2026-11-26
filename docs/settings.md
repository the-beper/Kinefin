# Settings reference

## Contents

- [General (`main`)](#general-main)
- [Audio (`audio`)](#audio-audio)
- [Video (`video`)](#video-video)
- [Subtitles (`subtitles`)](#subtitles-subtitles)
- [Library (`library`)](#library-library)
- [Downloads (`downloads`)](#downloads-downloads)
- [mpv Configuration (`other`)](#mpv-configuration-other)
- [Settings without a control](#settings-without-a-control)
- [Not carried over from Jellyfin Desktop](#not-carried-over-from-jellyfin-desktop)

Open the Settings page from the main menu, **Settings**, or press Ctrl+,. Its categories come in this order: General, Audio, Video, Subtitles, Library, Downloads and mpv Configuration, then User while you are signed in. The User category holds your preferences stored on the Jellyfin server and is not part of this reference. Each category groups its settings into cards; the card name is in parentheses in the Setting column below.

Settings are stored per profile in `kinefin.conf`, in the [profile data folder](user-guide.md#where-kinefin-keeps-its-files). The file has one group per category, named by the code in the heading of each section below, and holds only the values that differ from the default. A value that is not valid reads as the default. A file written by a newer version is renamed to `kinefin.conf.broken` and the settings start from the defaults.

In the tables, the Key column is the key as stored in the file and the Default column is the value as stored there (`""` is an empty value).

## General (`main`)

| Setting | Key | Default | Values | What it does |
| --- | --- | --- | --- | --- |
| Full Screen (Window) | `fullscreen` | `false` | On or off | Starts the window in full screen and switches it live when you change the setting. The command-line flags `--fullscreen` and `--windowed` override it for that run, until you change the setting. |
| Force Full Screen Display (Window) | `forceFSScreen` | `""` | Name of a screen output, such as `DP-1`; empty for the current screen | Full screen goes to the output with this name when it exists. |
| Interface Scale (Application) | `uiScale` | `100` | 100% (`100`), 110% (`110`), 125% (`125`), 150% (`150`), 175% (`175`), 200% (`200`) | Enlarges the whole interface (text, icons, cards and spacing) by this percentage, on top of the scale factor Qt takes from the desktop. It applies at the next start, because Qt reads its scale factor only while the application starts, and it does not scale the video. If the `QT_SCALE_FACTOR` environment variable is set, Kinefin leaves it as it is and ignores this setting. |
| Gamepad (Experimental) (Application) | `sdlEnabled` | `true` | On or off | Turns SDL3 gamepad input on or off. Off, Kinefin never initialises SDL and never opens a `/dev/input` device. |
| Enable Input Repeat (Application) | `enableInputRepeat` | `true` | On or off | Repeats an action while you hold a gamepad direction or a keyboard key. Text fields keep their own key repeat, and the toggles in the player overlay never repeat. |
| Ignore Certificate Errors (Application) | `ignoreSSLErrors` | `false` | On or off | Accepts invalid TLS certificates for all traffic to the Jellyfin server (API, WebSocket, images, mpv), never for the metadata providers' covers. Off, a certificate error stops the connection with a message naming this switch. A warning shows on the General page while it is on. The `--ignore-certificate-errors` flag forces it on for a run. |
| Auto-Detect Certificate Bundle (Application) | `autodetectCertBundle` | `true` | On or off | Points mpv at the first readable system certificate bundle. Off, mpv and FFmpeg use their own defaults. A `tls-ca-file` line in Manual MPV Configuration overrides it; the same line in `mpv.conf` does not, since the file is the lowest layer. |

## Audio (`audio`)

| Setting | Key | Default | Values | What it does |
| --- | --- | --- | --- | --- |
| Device Type (Output) | `devicetype` | `basic` | Basic (`basic`), S/PDIF (`spdif`), HDMI (`hdmi`) | Selects which codecs the Passthrough switches can send undecoded: none on Basic, AC3 and DTS on S/PDIF, all five on HDMI. |
| Channels (Output) | `channels` | `2.0` | Auto (`auto`), Stereo (2.0) (`2.0`), 5.1 surround (`5.1,2.0`), 7.1 surround (`7.1,5.1,2.0`) | Sets the channel layout mpv outputs. It also decides how many audio channels the app tells the server it can play, so it affects when the server transcodes audio. S/PDIF forces stereo. |
| Device (Output) | `device` | `auto` | `auto` or the name of an mpv audio device | Chooses the audio output device. If the chosen device is absent, playback falls back to `auto` and logs it. |
| Normalize (Output) | `normalize` | `true` | On or off | Keeps the volume level when mpv mixes surround audio down to fewer channels. |
| Night Mode (Output) | `night_mode` | `false` | On or off | Raises quiet dialogue and lowers loud scenes. The player menu has the same switch, and both change this one setting. A filter cannot act on a bitstream, so Night Mode turns passthrough off while it is on and shows a notification. The passthrough switches keep their values. |
| AC3 (Passthrough) | `passthrough.ac3` | `false` | On or off | Sends AC3 audio to the receiver undecoded. Needs Device Type S/PDIF or HDMI. |
| DTS (Passthrough) | `passthrough.dts` | `false` | On or off | Sends DTS audio undecoded. Needs Device Type S/PDIF or HDMI. Ignored while DTS-HD is on. |
| E-AC3 (Passthrough) | `passthrough.eac3` | `false` | On or off | Sends E-AC3 audio undecoded. Needs Device Type HDMI. |
| DTS-HD (Passthrough) | `passthrough.dts-hd` | `false` | On or off | Sends DTS-HD audio undecoded. Needs Device Type HDMI. With it on, the plain DTS entry is dropped from the passthrough list. |
| TrueHD (Passthrough) | `passthrough.truehd` | `false` | On or off | Sends TrueHD audio undecoded. Needs Device Type HDMI. |

## Video (`video`)

| Setting | Key | Default | Values | What it does |
| --- | --- | --- | --- | --- |
| Hardware Decoding (Decoding and Display) | `hardwareDecoding` | `copy` | Enabled (`enabled`), Copy (`copy`), Disabled (`disabled`) | Maps to the mpv `hwdec` option: `enabled` is `auto`, `copy` is `auto-copy` (decodes on the GPU and copies frames back), `disabled` is `no`. |
| Deinterlace (Decoding and Display) | `deinterlace` | `false` | On or off | Deinterlaces interlaced video. |
| HDR Output (Decoding and Display) | `hdr_output` | `auto` | Auto (`auto`), Off (`off`) | A Kinefin addition, not in Jellyfin Desktop. Auto shows HDR video as HDR in the full player when the desktop and the screen allow it, see [HDR video](user-guide.md#hdr-video). Off keeps mpv's tone mapping to SDR. While HDR output is on, Kinefin sets `target-trc`, `target-prim` and `target-peak` itself, over lines for those options in `mpv.conf` and Manual MPV Configuration. The row is disabled with a reason when the build or the desktop cannot offer HDR output. |
| AV Sync Mode (Playback) | `sync_mode` | `audio` | Audio (`audio`), Display resample (`display-resample`), Display, drop audio (`display-adrop`) | Sets the mpv `video-sync` mode: what keeps picture and sound together. |
| Cache Size (Playback) | `cache` | `75` | 10 MB (`10`), 75 MB (`75`), 150 MB (`150`), 500 MB (`500`) | Sets how much of the stream mpv buffers ahead (`demuxer-max-bytes`). |
| Default Playback Speed (Playback) | `default_playback_speed` | `1.0` | A number from 0.5 to 4 (times normal speed) | Speed at which every playback starts. |
| Skip Intros Automatically (Playback) | `skip_intros` | `false` | On or off | Skips an Intro segment once when playback enters it. It does not skip one you seeked into, one that starts before the point a resumed playback began, or one that runs to within 5 s of the end. Recap and Outro segments stay on the Skip button. |
| Prefer Transcode to H.265 (Transcoding) | `prefer_transcode_to_h265` | `false` | On or off | Puts H.265 first in the video codec list offered to the server for transcoding. Has an effect only with Allow Transcode to HEVC on. |
| Allow Transcode to HEVC (Transcoding) | `allow_transcode_to_hevc` | `false` | On or off | Lets the server transcode to HEVC. Off, it transcodes to H.264, MPEG-4 or MPEG-2. |
| Force Transcode HEVC (Transcoding) | `force_transcode_hevc` | `false` | On or off | Makes the server transcode every HEVC video instead of direct playing it. |
| Force Transcode 4K (Transcoding) | `force_transcode_4k` | `false` | On or off | Makes the server transcode video above 1920x1080. |
| Force Transcode AV1 (Transcoding) | `force_transcode_av1` | `false` | On or off | Makes the server transcode every AV1 video. |
| Force Transcode HDR (Transcoding) | `force_transcode_hdr` | `false` | On or off | Makes the server transcode every video that is not SDR, Dolby Vision variants included. |
| Force Transcode Hi10P (Transcoding) | `force_transcode_hi10p` | `false` | On or off | Makes the server transcode video with more than 8 bits per channel. |
| Force Transcode Dolby Vision (Transcoding) | `force_transcode_dovi` | `true` | On or off | Makes the server transcode video whose range type is exactly Dolby Vision (profile 5, no fallback layer). Other Dolby Vision variants still direct play. |
| Always Force Transcode (Transcoding) | `always_force_transcode` | `false` | On or off | Never direct plays video: the server transcodes every video. |
| Lower Quality on Slow Connections (Transcoding) | `adaptive_bitrate` | `true` | On or off | When the cache stalls for 10 s within a minute, asks the server for a stream at a lower bit rate, at the same position, with a notification naming the new rate. Takes at most three steps per item, 30 s apart, and never goes back up while the item plays. Does not apply to downloads, SyncPlay groups or a bit rate pinned in the Quality picker. |

## Subtitles (`subtitles`)

| Setting | Key | Default | Values | What it does |
| --- | --- | --- | --- | --- |
| Font (Text) | `font` | `sans-serif` | Default (`""`), Serif (`serif`), Sans-Serif (`sans-serif`), Script (`script`), Monospace (`monospace`), Display (`display`), Cursive (`cursive`), Fantasy (`fantasy`) | Font family for subtitles (mpv `sub-font`). |
| Size (Text) | `size` | `-1` | Default (`-1`), `18`, `24`, `32`, `42`, `60` | Subtitle size, scaled as the value divided by 32 (`sub-scale`). `-1` keeps the mpv default. |
| Color (Text) | `color` | `""` | Default (`""`), Black (`#000000`), White (`#FFFFFF`), Light Grey (`#EEEEEE`), Blended Light Grey (`#CCCCCC`), Yellow (`#FBF93E`), Light Yellow (`#FFFFCC`), Blended Light Yellow (`#EEEDB8`) | Text color. |
| Placement (Text) | `placement` | `""` | Default (`""`), Left, Bottom (`left,bottom`), Right, Bottom (`right,bottom`), Center, Bottom (`center,bottom`), Left, Top (`left,top`), Right, Top (`right,top`), Center, Top (`center,top`) | Horizontal alignment and bottom or top position of the subtitles. |
| Color (Border) | `border_color` | `""` | Default (`""`), Black (`#000000`), White (`#FFFFFF`), Light Grey (`#EEEEEE`), Blended Light Grey (`#CCCCCC`), Yellow (`#FBF93E`), Light Yellow (`#FFFFCC`), Blended Light Yellow (`#EEEDB8`) | Color of the outline around the text. |
| Size (Border) | `border_size` | `-1` | Default (`-1`), None (`0`), Tiny (`1`), Small (`2`), Normal (`3`), Large (`4`), Huge (`5`) | Thickness of the outline. |
| Color (Background) | `background_color` | `""` | Default (`""`), Black (`#000000`), White (`#FFFFFF`), Light Grey (`#EEEEEE`), Blended Light Grey (`#CCCCCC`), Yellow (`#FBF93E`), Light Yellow (`#FFFFCC`), Blended Light Yellow (`#EEEDB8`) | Color of the box behind the text. It applies only when Transparency is also set. |
| Transparency (Background) | `background_transparency` | `""` | Default (`""`), 0% (`FF`), 25% (`C0`), 50% (`80`), 75% (`40`), 100% (`00`) | Opacity of the background box, stored as the hex alpha value. It applies only when Color (Background) is also set. |
| Scale Border and Shadow (ASS Subtitles) | `ass_scale_border_and_shadow` | `true` | On or off | Scales the border and shadow of styled (ASS) subtitles with the subtitle size. |
| Style Override (ASS Subtitles) | `ass_style_override` | `""` | Default (`""`), Yes (`yes`), No (`no`), Force (`force`), Scale (`scale`), Strip (`strip`) | Sets how far your subtitle settings replace the styles embedded in ASS subtitles (mpv `sub-ass-override`). |

## Library (`library`)

| Setting | Key | Default | Values | What it does |
| --- | --- | --- | --- | --- |
| Libraries (Show on Home) | `home.libraries` | `true` | On or off | Shows the Libraries heading and tiles on Home. Off hides them at once. |
| Favorites (Show on Home) | `home.favorites` | `true` | On or off | Shows the Favorites row on Home, after Next Up. Off, the app does not request it. |
| Show New Badge on Recently Added Items (Cards) | `newBadge` | `true` | On or off | Shows a New badge on cards of unplayed items added within the last 7 days. |
| Suggestions (Show Tabs in Movies) | `tabs.movies.suggestions` | `false` | On or off | Shows the Suggestions tab in Movies, with server recommendations. |
| Favorites (Show Tabs in Movies) | `tabs.movies.favorites` | `true` | On or off | Shows the Favorites tab in Movies, with the favorite movies of the chosen library. |
| Collections (Show Tabs in Movies) | `tabs.movies.collections` | `true` | On or off | Shows the Collections tab in Movies, with the collections of all libraries. |
| Genres (Show Tabs in Movies) | `tabs.movies.genres` | `false` | On or off | Shows the Genres tab in Movies, with one row per genre. |
| Suggestions (Show Tabs in Shows) | `tabs.tv.suggestions` | `false` | On or off | Shows the Suggestions tab in Shows, with server suggestions from all libraries. |
| Favorites (Show Tabs in Shows) | `tabs.tv.favorites` | `true` | On or off | Shows the Favorites tab in Shows, with the favorite series of the chosen library. |
| Collections (Show Tabs in Shows) | `tabs.tv.collections` | `true` | On or off | Shows the Collections tab in Shows, with the same collections as in Movies. |
| Upcoming (Show Tabs in Shows) | `tabs.tv.upcoming` | `false` | On or off | Shows the Upcoming tab in Shows, with episodes yet to air. |
| Genres (Show Tabs in Shows) | `tabs.tv.genres` | `false` | On or off | Shows the Genres tab in Shows, with one row per genre. |
| Networks (Show Tabs in Shows) | `tabs.tv.networks` | `false` | On or off | Shows the Networks tab in Shows. |
| Episodes (Show Tabs in Shows) | `tabs.tv.episodes` | `false` | On or off | Shows the Episodes tab in Shows, with all episodes of the chosen library. |

Every tab switch applies at once. Turning a tab off returns you to the main tab, which is always shown.

## Downloads (`downloads`)

| Setting | Key | Default | Values | What it does |
| --- | --- | --- | --- | --- |
| Parallel Downloads | `parallel` | `2` | 1 download (`1`), 2 downloads (`2`), 3 downloads (`3`), 4 downloads (`4`) | Sets how many downloads run at the same time. |
| Write Kodi Metadata Files | `kodiSidecars` | `false` | On or off | Writes a Kodi-compatible `.nfo` file and `poster.jpg` next to an exported file. |
| Export When Download Completes | `exportOnCompletion` | `false` | On or off | Copies each finished download to the host folder you chose, through the document portal. The control stays disabled with an explanation while no durable portal grant exists. |

The Downloads category is hidden when the server does not allow you to download media. Under its cards, **Automatic Downloads** opens the page that lists the automatic download rules of your series, and says how many are tracked.

## mpv Configuration (`other`)

| Setting | Key | Default | Values | What it does |
| --- | --- | --- | --- | --- |
| Manual MPV Configuration | `other_conf` | `""` | `key=value` lines in `mpv.conf` syntax | Passes the lines to mpv after every managed setting, so they win over the settings above. Removing a line restores the default only after a restart. |

Besides that setting, the profile's own `mpv.conf` is read from the profile folder (`~/.local/share/kinefin/profiles/<profile>/mpv.conf`, under `~/.var/app/io.github.the_beper.Kinefin/` in the Flatpak). The category's **mpv.conf file** card says whether the file is there, how many of its lines apply and what is wrong with the others, with a **Check Again** button.

- Kinefin first sets `video-timing-offset` to `0.01` (mpv's default is `0.05`). The file is applied next, then the settings above, then Manual MPV Configuration: a line for an option that a setting manages is overridden by it, and a `video-timing-offset` line in the file or in Manual MPV Configuration replaces the default. Taking the line away restores `0.01` the next time the settings are applied.
- A video that drops frames with heavy shaders (a render of more than 10 ms) may need `video-timing-offset=0.05`.
- A line is `option=value`, `option` alone (a flag, set to `yes`), `no-option` (set to `no`), a `#` comment, a blank line, and a value may be in single or double quotes.
- A `[profile]` section is reported, and the lines under it are left out.
- `script`, `scripts`, `load-scripts`, `include`, `input-conf`, `config`, `config-dir`, `profile`, `input-ipc-server`, `input-ipc-client`, `ytdl`, `ytdl-format`, `ytdl-raw-options` and `http-*` are refused, as are the options and values that mpv itself does not accept. That list guards against the obvious and is not a sandbox: the file is yours, and mpv runs the other lines as written. A refused line is reported with its line number and skipped, and the others apply.
- The limits are 64 KiB, 1000 lines, 64 characters for an option name and 4096 for a value.
- The check cannot judge every value: mpv takes any text for `vo`, `ao` and `hwdec` until it opens the output, so a wrong one shows only in mpv's own log (set `log-file` to read it); `log-file`, `dump-stats` and `stream-record` write a file, so they are not tried and apply as written.

The same rules apply to Manual MPV Configuration. Added or changed lines take effect at the next playback; removing a line restores the default only after a restart.

## Settings without a control

These settings have no control on the Settings page. Some are kept for compatibility and some are read only from `kinefin.conf`. Edit the file while Kinefin is closed.

| Key | Default | Values | What it does |
| --- | --- | --- | --- |
| `main.disablemouse` | `false` | On or off | Ignores mouse input (clicks, movement, wheel) in the full player and the picture-in-picture window. The mini player and browsing are unaffected. |
| `main.logLevel` | `debug` | `debug`, `info`, `warn`, `error`, `fatal`, `disable` | Sets the log verbosity. The `--log-level` flag wins for that run. |
| `main.forceAlwaysFS` | `false` | On or off | Kept so that a configuration copied from Jellyfin Desktop stays valid; Kinefin does not use it. |
| `main.useSystemVideoCodecs` | `false` | On or off | Kept so that a configuration copied from Jellyfin Desktop stays valid; Kinefin does not use it. |
| `video.aspect` | `normal` | `normal`, `custom`, `zoom`, `force_4_3`, `force_16_9`, `force_16_9_if_4_3`, `stretch`, `noscaling` | Default picture aspect handling. `custom` leaves mpv untouched, `force_16_9_if_4_3` stretches only 4:3 video, `noscaling` plays at the original size. The Aspect Ratio menu in the player overrides it for the current item only. |
| `video.audio_delay.normal` | `0` | A number, in milliseconds | Audio delay used when the screen refresh rate is not near 24, 25 or 50 Hz. |
| `video.audio_delay.24hz` | `0` | A number, in milliseconds | Audio delay used when the screen refresh rate is within 0.5 Hz of 24 Hz. |
| `video.audio_delay.25hz` | `0` | A number, in milliseconds | Audio delay used when the screen refresh rate is within 0.5 Hz of 25 Hz. |
| `video.audio_delay.50hz` | `0` | A number, in milliseconds | Audio delay used when the screen refresh rate is within 0.5 Hz of 50 Hz. |
| `video.debug.force_vo` | `""` | An mpv video output name, or empty | Overrides the mpv `vo` option, for debugging. |
| `state.geometry` | `""` | `x,y,width,height` | Window geometry, saved when a windowed window closes and restored at the next start. On Wayland only the size returns, because a client cannot place its window. |

## Not carried over from Jellyfin Desktop

Jellyfin Desktop has these settings and Kinefin does not. A configuration copied from Jellyfin Desktop can still contain them, and they have no effect.

| Setting | Why |
| --- | --- |
| `alwaysOnTop` | Kinefin's main window never asks the compositor to stay on top. The picture-in-picture window does. |
| `enableMPV` | In Jellyfin Desktop it falls back to the HTML5 player. mpv is Kinefin's only player. |
| `useOpenGL` | A Windows switch between GL and X11 handling. It means nothing in a Qt 6 app that picks Wayland or X11 itself. |
| `refreshrate.auto_switch`, `refreshrate.delay`, `refreshrate.avoid_25hz_30hz` | Kinefin does not switch the display mode ([why](architecture.md#decisions)), so there is no mode to choose or wait for. |
| `exclusive` | Audio exclusive mode. Jellyfin Desktop does not offer it on Linux either. |
| `checkForUpdates` | Kinefin makes no update check. The Flatpak updates through its repository. |
| `hdmi_poweron`, `system` section | OpenELEC only: HDMI power-on and service management (LIRC, Samba, SSH). |
| `appleremote` section | Input from a macOS remote. |
| `userWebClient`, `path.startupurl_desktop`, `path.startupurl_extension` | Which web client page to load, the bundled one or your own build. Kinefin loads no web client. |
| Allow Browser Zoom (`allowBrowserZoom`) | Zooms the embedded web page. Kinefin is native and has no page to zoom. |
