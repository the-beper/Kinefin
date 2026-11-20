#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only API preflight: fixture fake-server by default, optional live server.

Does not launch mpv or claim end-to-end playback. Deliberately does not write
Sessions/Playing, Progress, Stopped or watched-state to a live Jellyfin server.
Use credentials in environment variables, never command-line arguments.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import threading
from urllib.parse import urlencode, urlsplit
from urllib.error import HTTPError, URLError
from urllib.request import HTTPRedirectHandler, Request, build_opener

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests" / "fixtures"


def read_fixture(name: str) -> dict:
    return json.loads((FIXTURES / f"{name}.json").read_text(encoding="utf-8"))


class StrictOriginRedirectHandler(HTTPRedirectHandler):
    """No silent credential leakage across hosts or downgrade to HTTP."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        old, new = urlsplit(req.full_url), urlsplit(newurl)
        if (old.scheme, old.netloc) != (new.scheme, new.netloc):
            raise ValueError("Refusing cross-origin redirect from Jellyfin server")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


class Probe:
    def __init__(self, base_url: str):
        base = urlsplit(base_url)
        if base.scheme not in ("http", "https") or not base.hostname:
            raise ValueError("A full HTTP(S) server URL is required")
        if base.username or base.password or base.query or base.fragment:
            raise ValueError("No credentials, query or fragment in the server URL")
        self.base = base_url.rstrip("/")
        self.opener = build_opener(StrictOriginRedirectHandler())
        self.token = ""

    def json(self, method: str, path: str, payload: dict | None = None) -> dict:
        if not path.startswith("/") or path.startswith("//"):
            raise ValueError("Only relative API paths are accepted")
        url = self.base + path
        auth = 'MediaBrowser Client="Kinefin-DesignProbe", Device="Preflight", DeviceId="kinefin-probe", Version="0.0.1"'
        if self.token:
            auth += ', Token="' + self.token + '"'
        headers = {"Authorization": auth, "Accept": "application/json"}
        data = None
        if payload is not None:
            data = json.dumps(payload).encode("utf-8")
            headers["Content-Type"] = "application/json"
        request = Request(url, data=data, headers=headers, method=method)
        try:
            with self.opener.open(request, timeout=15) as response:
                result = json.loads(response.read(1_048_577))
        except HTTPError as exc:
            # No untrusted response body or URL is printed: may contain secrets.
            raise ValueError(f"Jellyfin API returned HTTP {exc.code} for {method} endpoint") from exc
        except URLError as exc:
            raise ValueError("Jellyfin server connection failed") from exc
        if not isinstance(result, dict):
            raise ValueError("Expected JSON object from Jellyfin API")
        return result

    def negotiate(self, username: str, password: str) -> dict:
        public = self.json("GET", "/System/Info/Public")
        if not public.get("Id"):
            raise ValueError("Invalid System/Info/Public server identity")
        login = self.json("POST", "/Users/AuthenticateByName",
                          {"Username": username, "Pw": password})
        self.token = login.get("AccessToken", "")
        user_id = login.get("User", {}).get("Id", "")
        if not self.token or not user_id:
            raise ValueError("Missing access token or user id after authentication")
        views = self.json("GET", "/UserViews?" + urlencode({"userId": user_id}))
        if not isinstance(views.get("Items"), list):
            raise ValueError("UserViews is not BaseItemDtoQueryResult")
        items = self.json("GET", "/Items?" + urlencode({"userId": user_id,
                                                           "recursive": "true",
                                                           "includeItemTypes": "Movie",
                                                           "limit": "10"}))
        movies = [x for x in items.get("Items", []) if x.get("Type") == "Movie" and x.get("Id")]
        result = {"server_identity_present": True,
                  "library_count": len(views["Items"]),
                  "movie_count_in_page": len(movies),
                  "negotiation_checked": False,
                  "direct_play_available": False,
                  "transcoding_available": False,
                  "session_reporting_checked": False,
                  "mpv_playback_checked": False}
        if movies:
            item_id = movies[0]["Id"]
            # A minimal media-type probe only. Full device-profile parity is out of scope
            info = self.json("POST", f"/Items/{item_id}/PlaybackInfo?" + urlencode({"userId": user_id}),
                             {"DeviceProfile": {"Name": "Kinefin shape probe",
                                                "DirectPlayProfiles": [{"Container": "mkv,mp4,webm", "Type": "Video"}],
                                                "TranscodingProfiles": [],
                                                "SubtitleProfiles": []}})
            sources = info.get("MediaSources")
            if not isinstance(sources, list):
                raise ValueError("PlaybackInfo missing MediaSources array")
            result["negotiation_checked"] = True
            result["direct_play_available"] = any(s.get("SupportsDirectPlay") for s in sources)
            result["transcoding_available"] = any(s.get("SupportsTranscoding") for s in sources)
        return result


class FakeJellyfinHandler(BaseHTTPRequestHandler):
    def log_message(self, *args):  # avoid logging request URLs or auth-bearing headers
        pass

    def _respond(self, fixture: str, status: int = 200):
        data = (FIXTURES / f"{fixture}.json").read_bytes()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _is_authorized(self):
        return 'Token="' in self.headers.get("Authorization", "")

    def do_GET(self):
        if self.path == "/System/Info/Public":
            self._respond("system_info_public")
        elif not self._is_authorized():
            self.send_error(401)
        elif self.path.startswith("/UserViews?"):
            self._respond("user_views")
        elif self.path.startswith("/Items?"):
            self._respond("movie_item")
        else:
            self.send_error(404)

    def do_POST(self):
        content_length = int(self.headers.get("Content-Length", "0"))
        if content_length > 65536:
            self.send_error(413)
            return
        # Read and parse the body to detect broken JSON request encoding.
        try:
            body = json.loads(self.rfile.read(content_length))
        except (json.JSONDecodeError, UnicodeDecodeError):
            self.send_error(400)
            return
        if self.path == "/Users/AuthenticateByName" and "Username" in body and "Pw" in body:
            self._respond("authenticate_success")
        elif self.path.startswith("/Items/") and "/PlaybackInfo?" in self.path and self._is_authorized():
            if "DeviceProfile" in body:
                self._respond("playbackinfo_direct")
            else:
                self.send_error(400)
        else:
            self.send_error(404)


@contextmanager
def fake_server():
    server = ThreadingHTTPServer(("127.0.0.1", 0), FakeJellyfinHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}"
    finally:
        server.shutdown()
        thread.join()
        server.server_close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--fixture", action="store_true", help="in-process fake server")
    mode.add_argument("--live", action="store_true", help="real server: read-only API preflight")
    args = parser.parse_args()
    if args.fixture:
        with fake_server() as server:
            result = Probe(server).negotiate("fixture-user", "fixture-password")
    else:
        server = os.getenv("KINEFIN_PROBE_URL", "")
        parsed = urlsplit(server)
        if parsed.scheme != "https" and parsed.hostname not in ("127.0.0.1", "localhost", "::1"):
            parser.error("live remote credentials require HTTPS (localhost HTTP is permitted)")
        username = os.getenv("KINEFIN_PROBE_USERNAME", "")
        password = os.getenv("KINEFIN_PROBE_PASSWORD", "")
        if not username or not password:
            parser.error("set KINEFIN_PROBE_USERNAME and KINEFIN_PROBE_PASSWORD in the environment")
        result = Probe(server).negotiate(username, password)
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
