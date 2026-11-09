# Kinefin

A native KDE Plasma 6 client for Jellyfin servers: Kirigami interface, its own REST client and native libmpv playback, no WebEngine.

## Building

Everything that needs Qt, KF6 or ECM runs in a Podman container; see [CONTRIBUTING.md](CONTRIBUTING.md).

```sh
build/build-env.sh build/check.sh
```

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). Jellyfin is a trademark of the Jellyfin project; Kinefin is an independent, clean-room client.
