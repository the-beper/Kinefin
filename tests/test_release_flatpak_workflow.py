# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Contract tests for who holds which secret and permission in the release workflow, and for how it calls the release scripts."""

from __future__ import annotations

from pathlib import Path
import posixpath
import re
import shlex
import unittest


ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
CI = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
RELEASE_DIR = ROOT / "build/release"
JOBS = ["validate", "ci-gate", "build-flatpak", "prepare-flatpak", "check-flatpak", "sign-flatpak",
        "finish-flatpak", "check-bundle", "attest-flatpak", "verify", "publish"]

SECRETS = re.compile(r"\bsecrets\b")
# Every way the workflow token (or the one of the environment) can be named; GHCR_TOKEN is what build-flatpak calls it for podman
TOKENS = re.compile(r"\bGH_TOKEN\b|\bGITHUB_TOKEN\b|\bgithub\.token\b|\bgithub\[\s*['\"]token['\"]\s*\]")
GH_TOKEN_JOBS = ("ci-gate", "prepare-flatpak", "verify", "publish")

# The jobs and the output each EXPECT_* variable of the scripts comes from: a pin read from the wrong job is a pin that does not pin
EXPECT_SOURCES = {
    "EXPECT_COMMITS_SHA256": "needs.build-flatpak.outputs.commits_sha256",
    "EXPECT_REFS_SHA256": "needs.build-flatpak.outputs.refs_sha256",
    "EXPECT_PREPARED_SHA256": "needs.prepare-flatpak.outputs.prepared_sha256",
    "EXPECT_SIGNLIST_SHA256": "needs.prepare-flatpak.outputs.signlist_sha256",
    "EXPECT_HEADS_SHA256": "needs.prepare-flatpak.outputs.heads_sha256",
    "EXPECT_BASE_SHA": "needs.prepare-flatpak.outputs.base_sha",
    "EXPECT_CHECKED_SHA256": "needs.check-flatpak.outputs.checked_sha256",
    "EXPECT_SIGNED_SHA256": "needs.sign-flatpak.outputs.signed_sha256",
    "EXPECT_FLATPAK_SHA256": "needs.finish-flatpak.outputs.flatpak_sha256",
}

# Every call of a release script in the workflow: (job, script, mode). The variables and the arguments come from the usage() of the script
SCRIPT_CALLS = [
    ("build-flatpak", "export-flatpak-commits.sh", None),
    ("prepare-flatpak", "prepare-flatpak-repo.sh", None),
    ("check-flatpak", "sign-flatpak-repo.sh", "--check"),
    ("sign-flatpak", "sign-flatpak-repo.sh", "--unpack"),
    ("sign-flatpak", "sign-flatpak-repo.sh", "--sign"),
    ("sign-flatpak", "sign-flatpak-repo.sh", "--cleanup"),
    ("finish-flatpak", "finish-flatpak-repo.sh", None),
    ("check-bundle", "check-flatpak-bundle.sh", None),
    ("publish", "publish_release.py", None),
]
SHELL_SCRIPTS = {script for _, script, _ in SCRIPT_CALLS if script.endswith(".sh")}
# publish_release.py has no usage() text: its arguments are those of its argparse parser and its variables are read in main() and site_step(); GH_TOKEN is read by gh, RELEASE_TAG by the shell for the first argument
PUBLISH_ARGS = ["<tag>", "<release-assets>"]
PUBLISH_ENV = {"GH_TOKEN", "RELEASE_TAG", "RELEASE_COMMIT", "RELEASE_IMAGE_DIGEST", "SITE_TAR", "EXPECT_SITE_SHA256", "SITE_BASE_SHA", "SITE_URL"}

# What each job uploads and downloads, by artifact name; "id:" is a download by artifact id, whose expression follows
ARTIFACTS = {
    "validate": (["flathub-manifest"], []),
    "ci-gate": ([], []),
    "build-flatpak": (["flatpak-commits", "flatpak-refs"], []),
    "prepare-flatpak": (["flatpak-prepared"], ["flatpak-commits"]),
    "check-flatpak": (["flatpak-checked"], ["flatpak-prepared", "flatpak-refs"]),
    "sign-flatpak": (["flatpak-signed"], ["flatpak-refs", "id:needs.check-flatpak.outputs.artifact_id"]),
    "finish-flatpak": (["asset-flatpak", "flatpak-site"], ["flatpak-prepared", "flatpak-refs", "flatpak-signed"]),
    "check-bundle": ([], ["asset-flatpak", "flatpak-refs"]),
    "attest-flatpak": ([], ["asset-flatpak"]),
    "verify": (["release-assets"], ["asset-flatpak"]),
    "publish": ([], ["flatpak-site", "release-assets"]),
}


def without_comments(text: str) -> str:
    return "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))


def split_jobs(workflow: str) -> dict[str, str]:
    """The text of each job of the jobs: section, comments removed, keyed by job id."""
    section = without_comments(workflow.split("\njobs:\n", 1)[1])
    parts = re.split(r"^  ([a-z][a-z-]*):\n", section, flags=re.MULTILINE)
    return dict(zip(parts[1::2], parts[2::2]))


def permissions_of(job: str) -> dict[str, str]:
    block = re.search(r"^    permissions:\n((?:      .*\n)+)", job, flags=re.MULTILINE)
    if block is None:
        raise AssertionError("a job without permissions")
    return dict(re.findall(r"^      ([a-z-]+): (\w+)$", block.group(1), flags=re.MULTILINE))


def needs_of(job: str) -> list[str]:
    match = re.search(r"^    needs: \[?([^\]\n]*)\]?$", job, flags=re.MULTILINE)
    return [name.strip() for name in match.group(1).split(",")] if match else []


def outputs_of(job: str) -> set[str]:
    block = re.search(r"^    outputs:\n((?:      .*\n)+)", job, flags=re.MULTILINE)
    return set(re.findall(r"^      (\w+):", block.group(1), flags=re.MULTILINE)) if block else set()


def steps_of(job: str) -> list[str]:
    return re.split(r"^      - ", job, flags=re.MULTILINE)[1:]


def normalised(step: str) -> str:
    """A step as its keys are indented below the first one, so that `- run:` and `- name:` as the first key read alike."""
    return "        " + step.rstrip("\n") + "\n"


def run_of(step: str) -> str:
    """The script of a step (a `run: |` block or a one-line `run:`), whichever key comes first in the step; empty when it has none."""
    match = re.search(r"^        run: (?:\|\n((?:          .*\n|\n)+)|(.*)\n)", normalised(step), flags=re.MULTILINE)
    return (match.group(1) or match.group(2) or "") if match else ""


def env_of(step: str) -> dict[str, str]:
    block = re.search(r"^        env:\n((?:          .*\n)+)", normalised(step), flags=re.MULTILINE)
    return dict(re.findall(r"^          ([A-Za-z_][A-Za-z0-9_]*): (.*)$", block.group(1), flags=re.MULTILINE)) if block else {}


def with_of(step: str) -> dict[str, str]:
    block = re.search(r"^        with:\n((?:          .*\n)+)", normalised(step), flags=re.MULTILINE)
    return dict(re.findall(r"^          ([a-z-]+): (.*)$", block.group(1), flags=re.MULTILINE)) if block else {}


def artifact_steps(job: str, kind: str) -> list[dict[str, str]]:
    """The `with:` of every actions/<kind>-artifact step of a job."""
    return [with_of(step) for step in steps_of(job) if re.search(rf"uses: actions/{kind}-artifact@", step)]


def artifact_names(job: str, kind: str) -> list[str]:
    names = []
    for options in artifact_steps(job, kind):
        if "artifact-ids" in options:
            names.append("id:" + options["artifact-ids"].removeprefix("${{ ").removesuffix(" }}"))
        else:
            names.append(options["name"])
    return sorted(names)


def usage_of(script: str) -> tuple[dict[str | None, list[str]], dict[str | None, set[str]]]:
    """The arguments of each mode and the variables of each mode (None: of every mode) that the usage() text of a script names. Variables GitHub sets in every job are left out."""
    text = (RELEASE_DIR / script).read_text(encoding="utf-8")
    heredoc = re.search(r"usage\(\) \{\n    cat >&2 <<'EOF'\n(.*?)\nEOF\n", text, flags=re.DOTALL)
    if heredoc is None:
        raise AssertionError(f"{script} has no usage() text")
    arguments: dict[str | None, list[str]] = {}
    variables: dict[str | None, set[str]] = {}
    for line in heredoc.group(1).splitlines():
        call = re.match(rf"^(?:usage: |       ){re.escape(script)}(?: (--[a-z]+))?((?: \[?<[^>]+>\]?)*)$", line)
        if call:
            arguments[call.group(1)] = call.group(2).split()
        names = re.match(r"^environment(?: of (--[a-z]+))?: (.*)$", line)
        if names:
            listed = re.sub(r"\([^)]*\)", "", names.group(2))
            variables[names.group(1)] = {n for n in re.findall(r"\b[A-Z][A-Z0-9_]{2,}\b", listed) if not n.startswith(("GITHUB_", "RUNNER_"))}
    return arguments, variables


def calls_in(job: str) -> list[tuple[str, str | None, list[str], dict[str, str], str]]:
    """Every call of a release script in the run blocks of a job: script, mode, the arguments after the mode, the env of its step and the step."""
    found = []
    for step in steps_of(job):
        script_text = run_of(step).replace("\\\n", " ")
        for line in script_text.splitlines():
            for script in sorted(SHELL_SCRIPTS | {"publish_release.py"}):
                if not re.search(rf"(?:^|\s)(?:\S*/)?build/release/{re.escape(script)}(?:\s|$)", line):
                    continue
                tokens = shlex.split(line)
                after = tokens[next(i for i, t in enumerate(tokens) if t.endswith("build/release/" + script)) + 1:]
                mode = after.pop(0) if script == "sign-flatpak-repo.sh" else None
                found.append((script, mode, after, env_of(step), step))
    return found


def usage_for(script: str, mode: str | None) -> tuple[list[str], set[str]]:
    if script == "publish_release.py":
        return PUBLISH_ARGS, PUBLISH_ENV
    arguments, variables = usage_of(script)
    return arguments[mode], variables.get(None, set()) | variables.get(mode, set())


class ReleaseFlatpakWorkflowTest(unittest.TestCase):
    def setUp(self) -> None:
        self.jobs = split_jobs(WORKFLOW)

    def test_jobs_and_default_permissions(self) -> None:
        self.assertEqual(list(self.jobs), JOBS)
        self.assertRegex(WORKFLOW, r"(?m)^permissions: \{\}$")
        for name, job in self.jobs.items():
            with self.subTest(job=name):
                self.assertIn("timeout-minutes:", job)
                self.assertEqual(job.count("actions/checkout@"), job.count("persist-credentials: false"))
                self.assertNotIn("continue-on-error", job)
                self.assertNotIn("|| true", job)

    def test_the_header_describes_every_job_in_one_line(self) -> None:
        header = WORKFLOW.split("\non:\n", 1)[0]
        for name in JOBS:
            with self.subTest(job=name):
                self.assertRegex(header, rf"(?m)^# {name} +\S")
        self.assertRegex(header, r"(?m)^# verify +the attestation, PROVENANCE\.json and the exact SHA256SUMS$")
        self.assertRegex(header, r"(?m)^# check-bundle +.*passes nothing on")

    def test_sign_flatpak_is_the_only_job_with_an_environment(self) -> None:
        for name, job in self.jobs.items():
            with self.subTest(job=name):
                if name == "sign-flatpak":
                    self.assertRegex(job, r"(?m)^    environment: release-signing$")
                else:
                    self.assertNotIn("environment:", job)

    def test_the_secret_is_on_the_sign_step_only(self) -> None:
        # \bsecrets\b, not "secrets.": toJSON(secrets) and secrets['NAME'] reach the secret too
        self.assertEqual(len(SECRETS.findall(without_comments(WORKFLOW))), 1)
        for name, job in self.jobs.items():
            with self.subTest(job=name):
                if name != "sign-flatpak":
                    self.assertEqual(SECRETS.findall(job), [])
                    continue
                self.assertEqual(len(SECRETS.findall(job)), 1)
                holders = [step for step in steps_of(job) if SECRETS.search(step)]
                self.assertEqual(len(holders), 1)
                self.assertEqual(env_of(holders[0]), {"FLATPAK_GPG_PRIVATE_KEY": "${{ secrets.FLATPAK_GPG_PRIVATE_KEY }}"})
                self.assertIn("build/release/sign-flatpak-repo.sh --sign ", holders[0])
                self.assertNotIn("--unpack", holders[0])

    def test_the_token_goes_only_where_it_is_needed(self) -> None:
        before_jobs = without_comments(WORKFLOW.split("\njobs:\n", 1)[0])
        self.assertIsNone(TOKENS.search(before_jobs))
        self.assertIsNone(re.search(r"\bGHCR_TOKEN\b", before_jobs))
        for name, job in self.jobs.items():
            with self.subTest(job=name):
                if name == "build-flatpak":
                    # the token reaches podman as GHCR_TOKEN and nothing else of the job
                    self.assertEqual(job.count("GHCR_TOKEN: ${{ github.token }}"), 1)
                    self.assertIsNone(TOKENS.search(job.replace("GHCR_TOKEN: ${{ github.token }}", "")))
                    continue
                self.assertIsNone(re.search(r"\bGHCR_TOKEN\b", job))
                if name in GH_TOKEN_JOBS:
                    self.assertEqual(job.count("github.token"), job.count("GH_TOKEN: ${{ github.token }}"))
                    self.assertIsNone(re.search(r"\bGITHUB_TOKEN\b|\bgithub\[", job))
                else:
                    self.assertIsNone(TOKENS.search(job))

    def test_sign_flatpak_waits_for_every_check(self) -> None:
        needs = needs_of(self.jobs["sign-flatpak"])
        for name in ("validate", "ci-gate", "build-flatpak", "prepare-flatpak", "check-flatpak"):
            self.assertIn(name, needs)

    def test_sign_flatpak_takes_the_checked_repository_by_id_and_cleans_up_always(self) -> None:
        job = self.jobs["sign-flatpak"]
        downloads = artifact_steps(job, "download")
        by_id = [options for options in downloads if "artifact-ids" in options]
        self.assertEqual(len(by_id), 1)
        self.assertEqual(by_id[0]["artifact-ids"], "${{ needs.check-flatpak.outputs.artifact_id }}")
        self.assertEqual(by_id[0]["path"], "flatpak-checked")
        self.assertNotIn("name: flatpak-checked", job)
        self.assertNotIn("name", by_id[0])
        check = self.jobs["check-flatpak"]
        self.assertIn("artifact_id: ${{ steps.upload.outputs.artifact-id }}", check)
        self.assertEqual([with_of(step)["name"] for step in steps_of(check) if re.search(r"id: upload$", step, flags=re.MULTILINE)], ["flatpak-checked"])
        steps = steps_of(job)
        cleanup = [step for step in steps if "sign-flatpak-repo.sh --cleanup" in run_of(step)]
        self.assertEqual(len(cleanup), 1)
        self.assertRegex(normalised(cleanup[0]), r"(?m)^        if: always\(\)$")
        sign = next(i for i, step in enumerate(steps) if "sign-flatpak-repo.sh --sign " in run_of(step))
        self.assertGreater(steps.index(cleanup[0]), sign)

    def test_artifact_names_are_exactly_the_designed_ones(self) -> None:
        for name, job in self.jobs.items():
            with self.subTest(job=name):
                uploads, downloads = ARTIFACTS[name]
                self.assertEqual(artifact_names(job, "upload"), sorted(uploads))
                self.assertEqual(artifact_names(job, "download"), sorted(downloads))

    def test_only_attest_flatpak_may_attest(self) -> None:
        for name, job in self.jobs.items():
            with self.subTest(job=name):
                permissions = permissions_of(job)
                if name == "attest-flatpak":
                    self.assertEqual(permissions, {"contents": "read", "id-token": "write",
                                                   "attestations": "write", "artifact-metadata": "write"})
                else:
                    self.assertNotIn("id-token", permissions)
                    self.assertNotEqual(permissions.get("attestations"), "write")
                    self.assertNotIn("artifact-metadata", permissions)
                    self.assertNotIn("actions/attest@", job)

    def test_only_publish_may_write_contents(self) -> None:
        for name, job in self.jobs.items():
            with self.subTest(job=name):
                permissions = permissions_of(job)
                if name == "publish":
                    self.assertEqual(permissions, {"contents": "write", "attestations": "read"})
                else:
                    self.assertNotEqual(permissions.get("contents"), "write")

    def test_job_permissions_are_as_small_as_designed(self) -> None:
        self.assertEqual(permissions_of(self.jobs["build-flatpak"]), {"contents": "read", "packages": "read"})
        for name in ("prepare-flatpak", "check-flatpak", "sign-flatpak", "finish-flatpak", "check-bundle"):
            with self.subTest(job=name):
                self.assertEqual(permissions_of(self.jobs[name]), {"contents": "read"})

    def test_finish_flatpak_holds_no_write_permission_and_does_not_install_the_bundle(self) -> None:
        job = self.jobs["finish-flatpak"]
        for permission, value in permissions_of(job).items():
            self.assertEqual(value, "read", permission)
        self.assertNotIn("environment:", job)
        self.assertIsNone(TOKENS.search(job))
        self.assertIn("flatpak_sha256: ${{ steps.finish.outputs.flatpak_sha256 }}", job)
        script = (RELEASE_DIR / "finish-flatpak-repo.sh").read_text(encoding="utf-8")
        self.assertIn("flatpak build-bundle", script)
        self.assertNotRegex(script, r"flatpak install[^\n]*--bundle")
        self.assertNotIn("bundle-client", script)

    def test_check_bundle_deploys_the_bundle_and_passes_nothing_on(self) -> None:
        job = self.jobs["check-bundle"]
        self.assertEqual(permissions_of(job), {"contents": "read"})
        self.assertNotIn("outputs:", job)
        self.assertNotIn("upload-artifact", job)
        self.assertNotIn("environment:", job)
        self.assertIsNone(TOKENS.search(job))
        self.assertIsNone(SECRETS.search(job))
        self.assertNotIn("GITHUB_OUTPUT", job)
        self.assertNotIn("GITHUB_ENV", job)
        self.assertNotIn("GITHUB_PATH", job)
        self.assertNotIn("actions/attest@", job)
        self.assertEqual(set(needs_of(job)), {"build-flatpak", "finish-flatpak"})
        self.assertEqual(job.count("check-flatpak-bundle.sh"), 1)
        # nothing reads what it produces
        for name, other in self.jobs.items():
            self.assertNotIn("needs.check-bundle.", other, name)
        for name in ("attest-flatpak", "publish"):
            with self.subTest(job=name):
                self.assertIn("check-bundle", needs_of(self.jobs[name]))
        script = (RELEASE_DIR / "check-flatpak-bundle.sh").read_text(encoding="utf-8")
        self.assertRegex(script, r"flatpak install[^\n]*--bundle")
        self.assertNotRegex(script, r">>?\s*\"?\$\{?GITHUB_(OUTPUT|ENV|PATH)")

    def test_site_url_is_defined_once_at_workflow_level(self) -> None:
        self.assertEqual(len(re.findall(r"(?m)^\s*SITE_URL:", without_comments(WORKFLOW))), 1)
        self.assertRegex(WORKFLOW, r"(?m)^env:\n  SITE_URL: https://the-beper\.github\.io/Kinefin$")
        self.assertLess(WORKFLOW.index("\nenv:\n"), WORKFLOW.index("\njobs:\n"))

    def test_every_script_call_has_its_variables_and_arguments_in_usage_order(self) -> None:
        workflow_env = set(re.findall(r"^  ([A-Z_]+):", without_comments(WORKFLOW).split("\njobs:\n", 1)[0].split("\nenv:\n", 1)[1], flags=re.MULTILINE))
        found = []
        for name, job in self.jobs.items():
            download_paths = {options["path"] for options in artifact_steps(job, "download")}
            for script, mode, arguments, env, step in calls_in(job):
                found.append((name, script, mode))
                with self.subTest(job=name, script=script, mode=mode):
                    placeholders, variables = usage_for(script, mode)
                    # the variables
                    for variable in sorted(variables):
                        self.assertTrue(variable in env or variable in workflow_env, f"{variable} is read by {script} {mode or ''} and not in the env of its step")
                    for variable, value in env.items():
                        if variable in EXPECT_SOURCES:
                            self.assertEqual(value, "${{ " + EXPECT_SOURCES[variable] + " }}", variable)
                    # the arguments, in the order of the usage text: a placeholder with a dot is a file, and the argument names it
                    required = [p for p in placeholders if not p.startswith("[")]
                    self.assertGreaterEqual(len(arguments), len(required))
                    self.assertLessEqual(len(arguments), len(placeholders))
                    for placeholder, argument in zip(placeholders, arguments):
                        wanted = placeholder.strip("[]<>")
                        if "." in wanted:
                            self.assertEqual(posixpath.basename(argument), wanted, f"{script}: {argument} is in the place of {placeholder}")
                            # a file comes from a download of the job
                            self.assertIn(posixpath.dirname(argument), download_paths, f"{script}: nothing puts {argument} there")
        self.assertEqual(set(found), set(SCRIPT_CALLS))
        self.assertEqual(len(found), len(SCRIPT_CALLS))

    def test_the_usage_of_the_sign_script_names_the_secret_of_its_signing_mode_only(self) -> None:
        for mode, expected in (("--check", set()), ("--unpack", set()), ("--sign", {"FLATPAK_GPG_PRIVATE_KEY"})):
            with self.subTest(mode=mode):
                variables = usage_for("sign-flatpak-repo.sh", mode)[1]
                self.assertEqual({v for v in variables if not v.startswith("EXPECT_")}, expected)

    def test_the_sign_steps_share_one_work_folder(self) -> None:
        folders = {mode: arguments[0] if mode == "--sign" else arguments[-1] for script, mode, arguments, _, _ in calls_in(self.jobs["sign-flatpak"]) if script == "sign-flatpak-repo.sh"}
        self.assertEqual(set(folders), {"--unpack", "--sign", "--cleanup"})
        self.assertEqual(len(set(folders.values())), 1)
        self.assertTrue(folders["--sign"].endswith("flatpak-sign-work"))
        self.assertEqual(calls_in(self.jobs["sign-flatpak"])[0][2][0], "flatpak-checked/checked-repo.tar")

    def test_every_output_that_is_read_is_declared_by_a_job_the_reader_needs(self) -> None:
        for name, job in self.jobs.items():
            with self.subTest(job=name):
                for source in set(re.findall(r"\bneeds\.([a-z-]+)\.", job)):
                    self.assertIn(source, needs_of(job))
                for source, output in set(re.findall(r"\bneeds\.([a-z-]+)\.outputs\.(\w+)", job)):
                    self.assertIn(output, outputs_of(self.jobs[source]))

    def test_attest_flatpak_checks_the_hash_before_attesting_the_downloaded_bundle(self) -> None:
        job = self.jobs["attest-flatpak"]
        self.assertEqual(set(needs_of(job)), {"finish-flatpak", "check-bundle"})
        self.assertNotIn("actions/checkout@", job)
        self.assertNotIn("build/", job)
        self.assertEqual(artifact_names(job, "download"), ["asset-flatpak"])
        self.assertIn("EXPECT_SHA256_FLATPAK: ${{ needs.finish-flatpak.outputs.flatpak_sha256 }}", job)
        check = job.index('sha256sum -c -')
        self.assertLess(job.index("actions/download-artifact@"), check)
        self.assertLess(check, job.index("actions/attest@"))
        self.assertIn("subject-path: asset-flatpak/Kinefin-x86_64.flatpak", job)
        self.assertIn("asset-flatpak/Kinefin-x86_64.flatpak", job[check - 100:check + 100])

    def test_finish_flatpak_uploads_the_signed_bundle_and_the_site(self) -> None:
        job = self.jobs["finish-flatpak"]
        self.assertIn("name: asset-flatpak\n          path: flatpak-finished/Kinefin-x86_64.flatpak", job)
        self.assertIn("name: flatpak-site\n          path: flatpak-finished/site.tar", job)
        self.assertNotIn("asset-flatpak", self.jobs["build-flatpak"])

    def test_verify_takes_the_bundle_hash_from_finish_and_needs_the_attestation(self) -> None:
        job = self.jobs["verify"]
        self.assertEqual(set(needs_of(job)), {"build-flatpak", "finish-flatpak", "attest-flatpak"})
        self.assertIn("EXPECT_SHA256_FLATPAK: ${{ needs.finish-flatpak.outputs.flatpak_sha256 }}", job)
        self.assertIn("RELEASE_IMAGE_DIGEST: ${{ needs.build-flatpak.outputs.image_digest }}", job)

    def test_publish_passes_the_site_inputs_of_finish_flatpak(self) -> None:
        job = self.jobs["publish"]
        self.assertEqual(set(needs_of(job)), {"validate", "ci-gate", "build-flatpak", "finish-flatpak", "check-bundle", "verify"})
        self.assertIn("name: flatpak-site\n          path: flatpak-site", job)
        self.assertIn("SITE_TAR: flatpak-site/site.tar", job)
        self.assertIn("EXPECT_SITE_SHA256: ${{ needs.finish-flatpak.outputs.site_sha256 }}", job)
        self.assertIn("SITE_BASE_SHA: ${{ needs.finish-flatpak.outputs.base_sha }}", job)
        self.assertIn("GH_TOKEN: ${{ github.token }}", job)
        self.assertIn('python3 -I build/release/publish_release.py "$RELEASE_TAG" release-assets', job)
        self.assertGreaterEqual(int(re.search(r"timeout-minutes: (\d+)", job).group(1)), 25)

    def test_publish_variable_table_matches_the_script(self) -> None:
        source = (RELEASE_DIR / "publish_release.py").read_text(encoding="utf-8")
        for variable in sorted(PUBLISH_ENV - {"GH_TOKEN", "RELEASE_TAG"}):
            self.assertRegex(source, rf"[\"']{variable}[\"']", variable)

    def test_validate_refuses_a_tag_older_than_the_newest(self) -> None:
        job = self.jobs["validate"]
        self.assertIn('python3 -I build/release/verify_release.py "$RELEASE_TAG" --require-newest', job)
        self.assertIn("fetch-depth: 0", job)
        self.assertIn("fetch-tags: true", job)

    def test_build_flatpak_builds_on_the_published_repository_and_exports_the_commits(self) -> None:
        job = self.jobs["build-flatpak"]
        self.assertIn('git ls-remote --heads "$url" gh-pages', job)
        self.assertIn('git clone --quiet --depth 1 --branch gh-pages "$url" dist/pages', job)
        self.assertIn('KINEFIN_FLATPAK_REPO="$PWD/dist/pages/repo" build/flatpak/build-flatpak.sh', job)
        self.assertLess(job.index("test-flatpak.sh"), job.index("export-flatpak-commits.sh dist/pages dist/flatpak-commits"))
        for output in ("image_digest", "commits_sha256", "refs_sha256"):
            self.assertIn(f"      {output}: ${{{{ steps.", job)
        self.assertNotIn("sign-flatpak-repo.sh", job)

    def test_untrusted_context_never_reaches_a_shell(self) -> None:
        for name, job in self.jobs.items():
            for step in steps_of(job):
                with self.subTest(job=name):
                    self.assertNotIn("${{", run_of(step), f"{name}: an expression inside a run script")

    def test_run_blocks_are_seen_when_run_is_the_first_key_of_a_step(self) -> None:
        self.assertEqual(run_of("run: echo ${{ github.ref_name }}\n"), "echo ${{ github.ref_name }}")
        self.assertEqual(run_of("run: |\n          echo ${{ github.ref_name }}\n          echo two\n"), "          echo ${{ github.ref_name }}\n          echo two\n")
        self.assertEqual(run_of("name: x\n        run: echo ${{ github.ref_name }}\n"), "echo ${{ github.ref_name }}")
        self.assertEqual(run_of("uses: actions/checkout@abc\n"), "")
        self.assertEqual(env_of("env:\n          A: b\n        run: echo\n"), {"A": "b"})

    def test_the_ci_repo_update_job_runs_the_release_simulation_with_sh(self) -> None:
        job = split_jobs(CI)["repo-update"]
        runs = [line.strip() for step in steps_of(job) for line in run_of(step).splitlines()]
        self.assertIn("/bin/sh build/release/simulate.sh", runs)
        self.assertNotIn("build/release/simulate.sh", runs)


if __name__ == "__main__":
    unittest.main()
