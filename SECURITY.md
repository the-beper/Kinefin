# Security policy

## Reporting

Use GitHub's private security advisory (`https://github.com/the-beper/Kinefin/security/advisories/new`), not a public issue. A fix and a release usually land within days; there is no security team, just one maintainer. For anything time-critical, say so in the advisory text.

Please include: the Kinefin version (`kinefin --version`), how it was installed (Flatpak vs local build), the context you observed (profile path, desktop session, server version), and a minimal reproduction. Do not attach files that contain your server address or access token.

## Supported versions

Only the most recent release (and `main` between releases). Older tags DO NOT get a backport.

## Scope and threat model

Kinefin is a native desktop client: its attack surface against the network is an ordinary Qt desktop app. Kinefin never loads, executes or injects web content.

User-facing guarantees that are part of the security posture, with the numbers/marks in the docs:

- **Server credentials.** The access token lives only in the user's keyring, through QtKeychain (service `io.github.the_beper.Kinefin`, one entry per profile, server and user, key `<profile>/<serverId>/<userId>`) and in memory while signed in; it is never written to a file in plain text, never logged and never lands in crash reports. Several users can be saved per server. The profile's `servers.json` (mode 0600, inside the app's own data dir) holds the non-secret record only, one per saved user: server id, address, user id, user name, avatar tag, device id, device name, `askPassword` and `remembered` (whether a token is stored). Passwords are never stored. A user with "Ask for Password" on has no token stored at all: the session lives in memory and that user always signs in again. Removing a user or signing out deletes the local token and record only; the session on the server is not revoked. Deleting a profile (`--delete-profile`) removes its keyring entries; if that fails, the command says to revoke the profile's devices in the server's dashboard.
- **Token keyring in the Flatpak.** The backend is pinned to libsecret (`QTKEYCHAIN_BACKEND=libsecret`). The entries are in an encrypted keyring file, `~/.var/app/io.github.the_beper.Kinefin/data/keyrings/default.keyring`, whose key the Secret portal holds. Without a Secret portal, or if it refuses access, the session stays in memory and the user signs in again at the next start; there is no plaintext fallback. The Flatpak does not talk to `kwalletd6`. `flatpak uninstall --delete-data` removes the keyring file.
- **Tokens from earlier KWallet versions.** They are neither read nor migrated. The old entries stay in the KWallet folder `Kinefin` until deleted in KWalletManager.
- **Switching users is not a security boundary.** Anyone with access to your desktop session and an unlocked keyring can switch to any saved user of a server without that user's password. Use "Ask for Password" for accounts that must not be reachable this way.
- **Remote metadata is data, not code.** Item names, overview text, cast biographies and collection titles from the server are rendered as plain text, never as HTML/rich text. Images are fetched only over the server's own base URL. A future markup-rendering regression is a vulnerability, not a styling bug.
- **TLS.** Certificate verification is on by default, for the API client and for mpv (`tls-verify`, with the first readable system bundle while `autodetectCertBundle` is on). "Ignore Certificate Errors" (`ignoreSSLErrors`, in the General settings) is off by default, shows a warning while it is on and covers all traffic to the Jellyfin server.
- **Plain HTTP.** Kinefin sends credentials to a plain-HTTP address that is not on this computer only after the user confirms the "Connect Without Encryption" dialog. The dialog is not shown again for a saved server (same address, same public server id, as long as a saved user of it exists), except when plain HTTP answered after an automatic fallback from HTTPS, which always asks. The server id is public (`/System/Info/Public` needs no authentication), so a host that takes over a saved plain-HTTP address and repeats the id receives the stored token at an automatic restore without a dialog. This residual risk is accepted: over plain HTTP the token already travels in clear with every request after the first confirmation, so a repeated dialog would not stop an active attacker. HTTPS has no such gap: the certificate ties the server to its address.
- **Sandboxing.** The Flatpak is the only supported packaging: it talks to the portals for opening links in the browser and holds the narrow D-Bus names it needs (single-instance, MPRIS, screensaver) and the Secret portal for the token keyring. No `--filesystem=home`, no WebEngine BaseApp. Host builds exist for development only and have no such promise.
- **Downloads.** Downloaded media lives inside the app's own data dir (`downloads/` under the profile), so a stray file cannot leave the sandbox. A host export ("move to `~/…`") happens only through a Document-portal **folder grant the user confirmed**, never a broad `--filesystem=home`; server-controlled titles are sanitised down to a basename (no separators, no control characters, length-capped) and never used raw as path components. The download request authenticates with an `Authorization` header, not an `api_key` query parameter, so the token cannot leak into server logs or proxies.
- **Local D-Bus surface.** MPRIS and the single-instance service expose only window/player controls; no token and no raw file path crosses them.

Out of scope: the security of your Jellyfin server (report that to the Jellyfin project), the web UI reached through the "open server web interface" button (it is your server's page, opened in your own browser), macOS/Windows (Kinefin is Linux-only).
