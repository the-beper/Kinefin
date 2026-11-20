# Kinefin

A native KDE Plasma 6 client for Jellyfin servers: Kirigami interface, its own REST client and a libmpv playback engine, without WebEngine. It browses movies and TV series (Home, library tabs with filters, Favorites, search, details, a play queue), plays them with mpv or on a DLNA TV or player, downloads them for offline playback, can control the server's remote sessions and searches from KRunner; music and audiobooks are not built yet. It builds and runs as a Flatpak, but acceptance against a real Jellyfin server on a Plasma desktop is still pending for most v1 features; this is **not yet a release-ready client**.

## Building

Everything that needs Qt, KF6 or ECM runs in a Podman container; see [CONTRIBUTING.md](CONTRIBUTING.md).

```sh
build/build-env.sh build/check.sh
```

## KRunner

While Kinefin is open and signed in, KRunner can find movies, series, episodes and collections on the server; the plugin is off by default, and the [user guide](docs/user-guide.md#search-from-krunner) explains how to turn it on and what it sends.

## Documentation

The [user guide](docs/user-guide.md) explains how to use Kinefin, the [settings reference](docs/settings.md) lists every setting with its default, and the [architecture overview](docs/architecture.md) is where contributors start.

## Release preparation

The first-release tooling and remaining acceptance requirements are documented in the [first-release checklist](build/release/FIRST_RELEASE.md); see also [release tooling](build/release/README.md). A passing offline preflight is not permission to publish a release.

```sh
python3 -I build/release/preflight.py
```

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). Jellyfin is a trademark of the Jellyfin project; Kinefin is an independent, clean-room client.
