# Security policy

## Reporting

Use GitHub's private security advisory (`https://github.com/the-beper/Kinefin/security/advisories/new`), not a public issue. A fix and a release usually land within days; there is no security team, just one maintainer — for anything time-critical say so in the advisory text.

Please include: the Kinefin version (`kinefin --version`), how it was installed (Flatpak vs local build), the context you observed (profile path, desktop session, server version), and a minimal reproduction. Do not attach files that contain your server address or access token.

## Supported versions

Only the most recent release (and `main` between releases). Older tags DO NOT get a backport.

## Scope and threat model

Kinefin is a native desktop client: its attack surface against the network is an ordinary Qt desktop app. Kinefin never loads, executes or injects web content.

User-facing guarantees that are part of the security posture, with the numbers/marks in the docs:

- **Server credentials.** The access token lives only in the user's KWallet (default wallet, folder `Kinefin`, one entry per profile and server) and in memory while signed in; it is never written to a file, never logged and never lands in crash reports. The profile's `servers.json` (mode 0600, inside the app's own data dir) holds the non-secret record only: server id, address, user id, user name, device id and device name. If KWallet is disabled, missing or refuses access, the session stays in memory and the user signs in again at the next start; there is no plaintext fallback. When a profile is reset (`ResetSettings` equivalent) its wallet entries must go with it (not implemented yet: no reset path exists).
- **Remote metadata is data, not code.** Item names, overview text, cast biographies and collection titles from the server are rendered as plain text, never as HTML/rich text. Images are fetched only over the server's own base URL. A future markup-rendering regression is a vulnerability, not a styling bug.
- **TLS.** Certificate verification is on by default (`autodetectCertBundle`); the `ignoreSSLErrors` compatibility switch is hidden and off by default, and the player is meant to share the same trust store (not implemented yet).
- **Sandboxing.** The Flatpak is the only supported packaging: it talks to the portals for opening links in the browser and holds the narrow D-Bus names it needs (single-instance, MPRIS, screensaver, KWallet, NetworkManager read access, logind). No `--filesystem=home`, no WebEngine BaseApp. Host builds exist for development only and have no such promise.
- **Downloads (planned, not implemented yet).** Downloaded media lives inside the app's own data dir (`downloads/` under the profile), so a stray file cannot leave the sandbox. A host export ("move to `~/…`") happens only through a Document-portal **folder grant the user confirmed**, never a broad `--filesystem=home`; server-controlled titles are sanitised down to a basename (no separators, no control characters, length-capped) and never used raw as path components. The download request authenticates with an `Authorization` header, not an `api_key` query parameter, so the token cannot leak into server logs or proxies.
- **Local D-Bus surface (MPRIS is not implemented yet).** MPRIS and the single-instance service expose only window/player controls; no token and no raw file path crosses them.

Out of scope: the security of your Jellyfin server (report that to the Jellyfin project), the web UI reached through the "open server web interface" button (it is your server's page, opened in your own browser), macOS/Windows (Kinefin is Linux-only).
