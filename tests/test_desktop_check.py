# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""build/desktop-check.sh against fake busctl and systemd-inhibit and a keyring file in a temporary home.

The script reads a real Plasma session; here the commands it calls are stubs that answer as a good
session would, and as a bad one would, so its parsing, its verdicts and its exit status are tested
without a desktop.
"""

import os
import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "build" / "desktop-check.sh"
SH = shutil.which("sh") or "/bin/sh"
APP_ID = "io.github.the_beper.Kinefin"
# Looks like an encrypted file, and must never show up in a report: the check reads the mode, not the content
KEYRING_MARKER = "KEYRING-CONTENT-MARKER"

# The state file holds what the fake player says; Pause, Play and Seek change it
BUSCTL = r"""#!/bin/sh
state="$FAKE_STATE"
[ -f "$state/status" ] || echo Playing > "$state/status"
[ -f "$state/position" ] || echo 1000000 > "$state/position"
for arg in "$@"; do
    case $arg in
    list)
        [ -f "$state/not-running" ] && exit 0
        printf 'org.mpris.MediaPlayer2.Kinefin.profile_default 11 kinefin\nio.github.the_beper.Kinefin.Runner.profile_default 11 kinefin\n'
        exit 0 ;;
    esac
done
case " $* " in
*" get-property "*)
    case " $* " in
    *" DesktopEntry "*) echo 's "io.github.the_beper.Kinefin"' ;;
    *" Identity "*) echo 's "Kinefin"' ;;
    *" CanControl "*) echo 'b true' ;;
    *" PlaybackStatus "*) printf 's "%s"\n' "$(cat "$state/status")" ;;
    *" Position "*) printf 'x %s\n' "$(cat "$state/position")" ;;
    *" Metadata "*) cat "$state/metadata" ;;
    esac
    ;;
*" Pause "*) echo Paused > "$state/status" ;;
*" Play "*) echo Playing > "$state/status" ;;
*" Seek "*) echo 6000000 > "$state/position" ;;
*" ListInhibitions "*)
    current=$(cat "$state/status")
    [ -f "$state/policy-down-when-paused" ] && [ "$current" = Paused ] && exit 1
    if [ ! -f "$state/no-inhibition" ] && { [ "$current" = Playing ] || [ -f "$state/stale-inhibition" ]; }; then
        echo 'a(ss) 2 "Firefox" "PRIVATE-REASON" "Kinefin" "Playing a video"'
    else
        echo 'a(ss) 1 "Firefox" "PRIVATE-REASON"'
    fi
    ;;
*" Match "*) cat "$state/matches" ;;
*" Run "*) exit 0 ;;
*" Actions "*) echo 'a(sss) 0' ;;
esac
"""

INHIBIT = """#!/bin/sh
cat "$FAKE_STATE/inhibitors"
"""

GOOD_METADATA = (
    'a{sv} 3 "xesam:title" s "Arcturus" "mpris:length" x 7200000000 '
    '"mpris:artUrl" s "file:///home/u/.cache/kinefin/cover.png"'
)


class DesktopCheck(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        base = Path(self.temp.name)
        self.state = base / "state"
        self.state.mkdir()
        self.bin = base / "bin"
        self.bin.mkdir()
        for name, text in (("busctl", BUSCTL), ("systemd-inhibit", INHIBIT)):
            path = self.bin / name
            path.write_text(text)
            path.chmod(path.stat().st_mode | stat.S_IXUSR)
        (self.state / "metadata").write_text(GOOD_METADATA)
        (self.state / "matches").write_text('a(sssida{sv}) 2 "m1" "Arcturus (2012)" "video" 50 1 0 "m2" "Boreal" "video" 50 1 0\n')
        self.keyring = base / ".var" / "app" / APP_ID / "data" / "keyrings" / "default.keyring"
        self.keyring.parent.mkdir(parents=True)
        self.keyring.write_text(KEYRING_MARKER)
        self.keyring.chmod(0o600)
        (self.state / "inhibitors").write_text("WHO UID USER PID COMM WHAT WHY MODE\nKinefin 1000 u 11 kinefin sleep Cast playing block\n")
        self.profiles = base / "profiles"
        (self.profiles / "default").mkdir(parents=True)
        (self.profiles / "default" / "servers.json").write_text('{"servers": [{"id": "x"}]}')
        self.report = base / "report.txt"

    def run_script(self, *args: str, path: str | None = None) -> subprocess.CompletedProcess[str]:
        env = {
            "PATH": path if path is not None else f"{self.bin}:{os.environ['PATH']}",
            "FAKE_STATE": str(self.state),
            "KINEFIN_DESKTOP_CHECK_INNER": "1",
            "KINEFIN_DESKTOP_CHECK_OUT": str(self.report),
            "HOME": self.temp.name,
        }
        return subprocess.run(
            [SH, str(SCRIPT), "--profile-dir", str(self.profiles), *args],
            env=env,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=60,
        )

    def test_a_good_session_passes_every_check(self) -> None:
        result = self.run_script("--act", "--cast")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for line in (
            "PASS  MPRIS name: org.mpris.MediaPlayer2.Kinefin.profile_default",
            "PASS  MPRIS DesktopEntry: io.github.the_beper.Kinefin",
            "PASS  MPRIS CanControl: true",
            "PASS  MPRIS PlaybackStatus: Playing",
            "PASS  MPRIS Metadata secrets: none",
            "PASS  MPRIS artUrl: a local file",
            "PASS  MPRIS Pause: Paused",
            "PASS  MPRIS Seek: 1000000 to 6000000",
            "PASS  MPRIS Play: Playing",
            "PASS  Screen inhibition while playing: Kinefin holds 1",
            "PASS  Suspend inhibition during a cast: a sleep inhibitor of Kinefin",
            "PASS  Token keyring: default.keyring in the Flatpak data folder, private (mode 600)",
            "PASS  servers.json holds no token: 1 file(s) checked",
            "PASS  KRunner Match 'the': 2 match(es)",
            "PASS  KRunner Run: the first match was run",
            "PASS  KRunner Actions: answers",
        ):
            self.assertIn(line, result.stdout)
        self.assertIn("LOOK  ", result.stdout)
        self.assertEqual(self.report.read_text().count("FAIL"), 0)
        self.assertEqual(result.stdout.strip(), self.report.read_text().strip())
        self.assertNotIn("PRIVATE-REASON", result.stdout)
        self.assertNotIn(KEYRING_MARKER, result.stdout + self.report.read_text())

    def test_without_the_act_flag_nothing_is_controlled(self) -> None:
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn("MPRIS Pause", result.stdout)
        self.assertNotIn("KRunner Run", result.stdout)
        self.assertEqual((self.state / "status").read_text().strip(), "Playing")
        self.assertIn("SKIP  Suspend inhibition: pass --cast while a cast plays", result.stdout)

    def test_a_token_in_the_metadata_or_an_artwork_from_the_server_fails(self) -> None:
        (self.state / "metadata").write_text(
            'a{sv} 2 "xesam:title" s "Arcturus" "mpris:artUrl" s "https://jf.example/Items/1/Images/Primary?api_key=SECRET"'
        )
        result = self.run_script()
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAIL  MPRIS Metadata secrets", result.stdout)
        self.assertIn("FAIL  MPRIS artUrl", result.stdout)
        self.assertIn("FAIL  MPRIS artUrl: not a local file, scheme https", result.stdout)
        # Neither the token nor any part of the address is printed, on the screen or in the report
        for text in (result.stdout, self.report.read_text()):
            self.assertNotIn("SECRET", text)
            self.assertNotIn("jf.example", text)

    def test_a_playing_video_without_an_inhibition_fails(self) -> None:
        (self.state / "no-inhibition").write_text("")
        result = self.run_script()
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAIL  Screen inhibition while playing: no inhibition of Kinefin among the 1 held", result.stdout)
        # What another program gave as its reason stays out of the report
        self.assertNotIn("PRIVATE-REASON", result.stdout)
        self.assertNotIn("PRIVATE-REASON", self.report.read_text())

    def test_nothing_playing_skips_what_needs_a_video(self) -> None:
        (self.state / "status").write_text("Stopped\n")
        result = self.run_script("--act")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("SKIP  MPRIS Metadata: nothing plays: start a video", result.stdout)
        self.assertIn("SKIP  MPRIS controls: nothing plays", result.stdout)
        self.assertIn("SKIP  Screen inhibition: nothing plays: start a video", result.stdout)

    def test_a_keyring_readable_by_others_and_a_token_on_disk_fail(self) -> None:
        self.keyring.chmod(0o644)
        (self.profiles / "default" / "servers.json").write_text('{"servers": [{"id": "x", "accessToken": "t"}]}')
        result = self.run_script()
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAIL  Token keyring: default.keyring is readable by others (mode 644)", result.stdout)
        self.assertNotIn(KEYRING_MARKER, result.stdout + self.report.read_text())
        self.assertIn("FAIL  servers.json holds no token", result.stdout)

    def test_markup_in_a_match_fails(self) -> None:
        (self.state / "matches").write_text('a(sssida{sv}) 1 "m1" "<b>Arcturus</b>" "video" 50 1 0\n')
        result = self.run_script()
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAIL  KRunner Match text", result.stdout)

    def test_a_paused_player_holds_no_screen_inhibition(self) -> None:
        (self.state / "status").write_text("Paused\n")
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("PASS  MPRIS PlaybackStatus: Paused", result.stdout)
        self.assertIn("PASS  MPRIS Metadata title: present", result.stdout)
        self.assertIn("PASS  Screen inhibition while paused: none held", result.stdout)

    def test_a_paused_player_that_still_holds_an_inhibition_fails(self) -> None:
        (self.state / "status").write_text("Paused\n")
        (self.state / "stale-inhibition").write_text("")
        result = self.run_script()
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAIL  Screen inhibition while paused: Kinefin still holds 1", result.stdout)

    def test_a_power_policy_agent_that_stops_answering_is_not_a_release(self) -> None:
        (self.state / "policy-down-when-paused").write_text("")
        result = self.run_script("--act")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("SKIP  Screen inhibition after Pause: the Plasma power policy agent stopped answering", result.stdout)
        self.assertNotIn("PASS  Screen inhibition after Pause", result.stdout)

    def test_a_cast_without_a_sleep_inhibitor_fails(self) -> None:
        (self.state / "inhibitors").write_text("WHO UID USER PID COMM WHAT WHY MODE\nFirefox 1000 u 5 firefox idle Video block\n")
        result = self.run_script("--cast")
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAIL  Suspend inhibition during a cast: no sleep inhibitor of Kinefin", result.stdout)

    def test_a_cast_that_also_holds_an_idle_inhibitor_fails(self) -> None:
        (self.state / "inhibitors").write_text(
            "WHO UID USER PID COMM WHAT WHY MODE\nKinefin 1000 u 11 kinefin sleep Cast playing block\nKinefin 1000 u 11 kinefin idle Cast playing block\n"
        )
        result = self.run_script("--cast")
        self.assertEqual(result.returncode, 1)
        self.assertIn("PASS  Suspend inhibition during a cast", result.stdout)
        self.assertIn("FAIL  Suspend inhibition during a cast: Kinefin also holds an idle inhibitor", result.stdout)

    def test_kinefin_not_running_fails_the_names_and_skips_what_needs_a_player(self) -> None:
        (self.state / "not-running").write_text("")
        result = self.run_script("--act")
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAIL  MPRIS name", result.stdout)
        self.assertIn("FAIL  KRunner name", result.stdout)
        self.assertIn("SKIP  Screen inhibition: nothing plays: start a video", result.stdout)
        self.assertNotIn("MPRIS Pause", result.stdout)

    def test_a_missing_busctl_is_reported_as_such(self) -> None:
        tools = Path(self.temp.name) / "tools"
        tools.mkdir()
        for name in ("dirname", "mkdir"):
            (tools / name).symlink_to(shutil.which(name) or name)
        result = self.run_script(path=str(tools))
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAIL  busctl: busctl is missing", result.stdout)
        self.assertNotIn("MPRIS name", result.stdout)
        self.assertIn("1 check(s) failed", self.report.read_text())

    def test_a_missing_keyring_is_a_skip(self) -> None:
        self.keyring.unlink()
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("SKIP  Token keyring: no ", result.stdout)
        self.assertNotIn("PASS  Token keyring", result.stdout)

    def test_a_keyring_that_is_not_a_regular_file_fails(self) -> None:
        self.keyring.unlink()
        self.keyring.symlink_to(Path(self.temp.name) / "elsewhere")
        (Path(self.temp.name) / "elsewhere").write_text(KEYRING_MARKER)
        result = self.run_script()
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAIL  Token keyring: ", result.stdout)
        self.assertIn("is not a regular file", result.stdout)

    def test_no_servers_json_is_a_skip_and_does_not_read_stdin(self) -> None:
        (self.profiles / "default" / "servers.json").unlink()
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("SKIP  servers.json holds no token: no servers.json found in", result.stdout)
        self.assertNotIn("PASS  servers.json", result.stdout)

    def test_a_profile_folder_with_spaces_is_checked(self) -> None:
        spaced = Path(self.temp.name) / "my profiles" / "default"
        spaced.mkdir(parents=True)
        (spaced / "servers.json").write_text('{"token": "t"}')
        result = self.run_script("--profile-dir", str(spaced.parent))
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAIL  servers.json holds no token: a token key is in 1 servers.json file(s)", result.stdout)

    @unittest.skipIf(os.geteuid() == 0, "root reads everything")
    def test_an_unreadable_servers_json_or_folder_fails_and_the_summary_is_still_written(self) -> None:
        blocked = self.profiles / "blocked"
        blocked.mkdir()
        blocked.chmod(0)
        self.addCleanup(blocked.chmod, 0o700)
        unreadable = self.profiles / "default" / "servers.json"
        unreadable.chmod(0)
        self.addCleanup(unreadable.chmod, 0o600)
        result = self.run_script()
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAIL  servers.json holds no token: 2 file(s) or folder(s) could not be read", result.stdout)
        self.assertNotIn("PASS  servers.json", result.stdout)
        self.assertIn("1 check(s) failed", result.stdout)
        self.assertIn("1 check(s) failed", self.report.read_text())

    def test_the_script_parses_and_refuses_an_unknown_option(self) -> None:
        self.assertEqual(subprocess.run(["sh", "-n", str(SCRIPT)]).returncode, 0)
        result = self.run_script("--nonsense")
        self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main()
