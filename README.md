# Kinefin

A native KDE Plasma 6 client for Jellyfin servers: Kirigami interface, its own REST client and a libmpv playback engine, without WebEngine. It browses movies and TV series (Home, library tabs with filters, Favorites, search, details, a play queue), plays them with mpv and can list the server's remote sessions; music and downloads are not built yet. It builds and runs as a Flatpak, but acceptance against a real Jellyfin server on a Plasma desktop is still pending for most v1 features; this is **not yet a release-ready client**.

## Building

Everything that needs Qt, KF6 or ECM runs in a Podman container; see [CONTRIBUTING.md](CONTRIBUTING.md).

```sh
build/build-env.sh build/check.sh
```

## Project status and release preparation

The [implementation plan](docs/plan.md) and [feature delivery table](docs/feature-status.md) distinguish implemented code from features verified on a Jellyfin server and KDE Plasma desktop. The first-release tooling and remaining acceptance requirements are documented in [P9 first-release checklist](build/release/FIRST_RELEASE.md); see also [release tooling](build/release/README.md). A passing offline preflight is **not** permission to publish a release.

```sh
python3 -I build/release/preflight.py
```

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). Jellyfin is a trademark of the Jellyfin project; Kinefin is an independent, clean-room client.
