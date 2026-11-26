# Third-party components

Kinefin is GPL-3.0-or-later. This page lists the code and data from other projects that it links, bundles, vendors or shows in its documentation. The per-file licenses are in [REUSE.toml](REUSE.toml) and the license texts in [LICENSES/](LICENSES/).

## Contents

- [From the KDE runtime](#from-the-kde-runtime)
- [Built into the Flatpak](#built-into-the-flatpak)
- [Vendored in this repository](#vendored-in-this-repository)
- [Documentation artwork](#documentation-artwork)

## From the KDE runtime

The Flatpak runs on `org.kde.Platform` 6.11 from Flathub. These libraries come with it and are not part of the bundle:

| Component | Used for | License |
| --- | --- | --- |
| [Qt 6](https://www.qt.io) (Core, Gui, Widgets, Quick, Qml, Quick Controls 2, D-Bus, Network) | The application framework | LGPL-3.0-only |
| [KDE Frameworks 6](https://develop.kde.org/products/frameworks/) (Kirigami, KConfig, KCoreAddons, KI18n, KDBusAddons, KWallet, KWindowSystem) | The interface, settings, translations, single instance, the token store, the shadow of the picture-in-picture window | LGPL, per file |
| [OpenSSL 3](https://www.openssl.org) | TLS (as Qt Network's backend), and the signature and chain checks of Google Cast device authentication | Apache-2.0 |
| [SDL 3](https://www.libsdl.org) | Gamepad input | Zlib |

FFmpeg and the other system libraries that mpv and Qt load also come from the runtime.

## Built into the Flatpak

The manifest, `build/flatpak/io.github.the_beper.Kinefin.yml`, builds these from pinned sources into the bundle:

| Component | Version | Used for | License |
| --- | --- | --- | --- |
| [mpv](https://mpv.io) (libmpv) | 0.42.0 | Playback | GPL-2.0-or-later (mpv's default build, with its `gpl` option on) |
| [libplacebo](https://code.videolan.org/videolan/libplacebo) | 7.387.0 | mpv's video renderer | LGPL-2.1-or-later |
| [libass](https://github.com/libass/libass) | 0.17.5 | Styled subtitles in mpv | ISC |
| [uchardet](https://www.freedesktop.org/wiki/Software/uchardet/) | 0.0.8 | Character set detection of subtitle files in mpv | MPL-1.1 OR GPL-2.0-or-later OR LGPL-2.1-or-later |
| [Qt WebSockets](https://github.com/qt/qtwebsockets) | 6.11.3 | The server's live events (the runtime does not ship it) | LGPL-3.0-only |
| [Kirigami Addons](https://invent.kde.org/libraries/kirigami-addons) | 1.13.1 | The forms of the Settings page | LGPL, per file |
| [mpvqt](https://invent.kde.org/libraries/mpvqt) | 1.2.1 | The Qt Quick item that shows mpv's video; compiled into Kinefin, not installed on its own | LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL |

A native build takes the same libraries from the system, except mpvqt, which CMake always fetches at the pinned version.

## Vendored in this repository

| Files | From | Changed | License |
| --- | --- | --- | --- |
| `third_party/mpvqt/mpvrenderer.{h,cpp}` | mpvqt 1.2.1 | Yes: the renderer replaces mpvqt's own at configure time | LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL |
| `third_party/openscreen/*.der` | The Cast and Eureka root certificates of Chromium's [Open Screen](https://chromium.googlesource.com/openscreen/), where the certificate chain of a Google Cast device must end ([details](third_party/openscreen/README.md)) | No | BSD-3-Clause, The Chromium Authors |
| `third_party/wayland/xx-pip-v1.xml` | KWin's picture-in-picture Wayland protocol, which the KDE runtime does not ship | No | MIT, Vlad Zahorodnii |

## Documentation artwork

The screenshots in `docs/images` show a sample library of the open films of the Blender Foundation and Blender Studio. The posters and frames in `tests/assets/doc` are scaled or cropped copies of the files below, and the screenshots derived from them carry the same licenses. The film in the player is *Tears of Steel* (CC BY 3.0, Blender Foundation | mango.blender.org).

| Film | File on Wikimedia Commons | License |
| --- | --- | --- |
| Elephants Dream | [ElephantsDreamPoster.jpg](https://commons.wikimedia.org/wiki/File:ElephantsDreamPoster.jpg) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| Big Buck Bunny | [Big buck bunny poster big.jpg](https://commons.wikimedia.org/wiki/File:Big_buck_bunny_poster_big.jpg) | [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/) |
| Sintel | [Sintel poster.jpg](https://commons.wikimedia.org/wiki/File:Sintel_poster.jpg) | [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/) |
| Tears of Steel (poster) | [Tos-poster.png](https://commons.wikimedia.org/wiki/File:Tos-poster.png) | [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/) |
| Tears of Steel (frame) | [Tears of Steel frame 08 4a.jpg](https://commons.wikimedia.org/wiki/File:Tears_of_Steel_frame_08_4a.jpg) | [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/) |
| Tears of Steel (frame) | [Tears of Steel frame 01 2a.jpg](https://commons.wikimedia.org/wiki/File:Tears_of_Steel_frame_01_2a.jpg) | [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/) |
| Tears of Steel (frame of the player) | [Tears of Steel frame 09 1a.jpg](https://commons.wikimedia.org/wiki/File:Tears_of_Steel_frame_09_1a.jpg) | [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/) |
| Cosmos Laundromat | [CosmosLaundromatPoster.jpg](https://commons.wikimedia.org/wiki/File:CosmosLaundromatPoster.jpg) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| Spring | [Spring2019AlphaPosterBlender.jpg](https://commons.wikimedia.org/wiki/File:Spring2019AlphaPosterBlender.jpg) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| Coffee Run | [Coffee Run-movie poster.png](https://commons.wikimedia.org/wiki/File:Coffee_Run-movie_poster.png) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| Sprite Fright | [Sprite Fright-movie poster.jpg](https://commons.wikimedia.org/wiki/File:Sprite_Fright-movie_poster.jpg) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| Charge | [Charge-movie poster.jpg](https://commons.wikimedia.org/wiki/File:Charge-movie_poster.jpg) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
