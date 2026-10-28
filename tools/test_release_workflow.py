"""Tests for the release-and-Pages GitHub Actions workflow.

These tests parse ``.github/workflows/release-pages.yml`` as plain text
(no PyYAML dependency) and assert the gating rules required by the release
plan: pull requests only run verification, ``publish`` requires a
successful, ancestry-checked ``verify`` run on a tag, and ``deploy``
requires ``publish`` and is granted only the minimal Pages permissions.
"""
import os
import re
import unittest

WORKFLOW_PATH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    ".github",
    "workflows",
    "release-pages.yml",
)


def _read_workflow():
    with open(WORKFLOW_PATH, "r", encoding="utf-8") as f:
        return f.read()


def _indented_block(text, key):
    """Return the text strictly nested under a ``key:`` line, by indentation.

    Finds a line matching ``<indent>key:`` (optionally with a trailing
    value, which is ignored), then returns every subsequent line that is
    indented *more* than that key line, stopping at the first line with
    indentation less than or equal to the key's own indentation (or EOF).
    This correctly separates a block from sibling keys at the same
    indentation level, regardless of the indent width used.
    """
    pattern = re.compile(rf"^([ \t]*){re.escape(key)}:[ \t]*(?:#.*)?$", re.MULTILINE)
    match = pattern.search(text)
    if match is None:
        return None
    key_indent = len(match.group(1))
    start = match.end() + 1  # skip the newline after the key line
    lines = text[start:].splitlines(keepends=True)
    collected = []
    for line in lines:
        stripped = line.strip()
        if not stripped:
            collected.append(line)
            continue
        indent = len(line) - len(line.lstrip(" \t"))
        if indent <= key_indent:
            break
        collected.append(line)
    return "".join(collected)


def _top_level_block(text, key):
    """Return the text of a top-level ``key:`` block (e.g. 'on', 'jobs')."""
    return _indented_block(text, key)


def _job_block(jobs_text, job_name):
    """Return the text of a single job block (e.g. 'verify') within 'jobs:'."""
    return _indented_block(jobs_text, job_name)


class ReleaseWorkflowGatingTests(unittest.TestCase):
    """Tests that the workflow gates verification, publish, and deploy correctly."""

    def setUp(self):
        if not os.path.isfile(WORKFLOW_PATH):
            self.fail(f"Workflow file not found: {WORKFLOW_PATH}")
        self.text = _read_workflow()
        self.jobs_block = _top_level_block(self.text, "jobs")
        self.assertIsNotNone(self.jobs_block, "workflow must have a top-level 'jobs:' key")
        self.verify_job = _job_block(self.jobs_block, "verify")
        self.publish_job = _job_block(self.jobs_block, "publish")
        self.deploy_job = _job_block(self.jobs_block, "deploy")
        self.assertIsNotNone(self.verify_job, "workflow must define a 'verify' job")
        self.assertIsNotNone(self.publish_job, "workflow must define a 'publish' job")
        self.assertIsNotNone(self.deploy_job, "workflow must define a 'deploy' job")

    def test_pull_requests_run_verification_only(self):
        """A PR to main triggers verification; publish/deploy are tag-only."""
        # Triggers: pull_request targeting main, and tag pushes for v*
        self.assertIn("pull_request", self.text)
        self.assertRegex(self.text, r"pull_request:\s*\n(?:.*\n)*?\s*branches:\s*\n\s*-\s*main")
        self.assertRegex(self.text, r"push:\s*\n(?:.*\n)*?\s*tags:\s*\n\s*-\s*['\"]?v\*")

        # publish and deploy must be conditioned on a tag ref, not on pull_request
        for job_name, job_text in (("publish", self.publish_job), ("deploy", self.deploy_job)):
            if_match = re.search(r"^\s*if:\s*(.+)$", job_text, re.MULTILINE)
            needs_match = re.search(r"needs:", job_text)
            # A tag-only condition must exist directly on the job, or be
            # inherited transitively via 'needs' from a job that is itself
            # tag-gated (publish -> verify's tag condition via needs+if).
            has_tag_guard = bool(if_match and (
                "refs/tags" in if_match.group(1) or "startsWith" in if_match.group(1)
            ))
            self.assertTrue(
                has_tag_guard or needs_match,
                f"job '{job_name}' must be gated to tag pushes, directly or via needs",
            )
        # publish job itself must carry an explicit tag-ref guard somewhere
        self.assertRegex(
            self.publish_job,
            r"refs/tags/v",
            "publish job must guard on a 'refs/tags/v' ref condition",
        )

    def test_publish_requires_successful_verification(self):
        """publish needs verify, and verify checks tag ancestry against origin/main."""
        self.assertRegex(
            self.publish_job,
            r"needs:\s*\n?\s*verify\b|needs:\s*\[?\s*verify\s*\]?",
            "publish job must declare 'needs: verify'",
        )
        # The verify job must check that the tag commit is an ancestor of
        # origin/main before proceeding.
        self.assertIn("origin/main", self.verify_job)
        self.assertRegex(
            self.verify_job,
            r"merge-base --is-ancestor|git merge-base",
            "verify job must check tag ancestry against origin/main",
        )

    def test_deploy_requires_publish_and_minimal_permissions(self):
        """deploy needs publish and is granted only pages/id-token write permissions."""
        self.assertRegex(
            self.deploy_job,
            r"needs:\s*\n?\s*publish\b|needs:\s*\[?\s*publish\s*\]?",
            "deploy job must declare 'needs: publish'",
        )

        perm_text = _indented_block(self.deploy_job, "permissions")
        self.assertIsNotNone(perm_text, "deploy job must declare a 'permissions' block")
        perm_keys = dict(
            re.findall(r"^\s*([\w-]+):\s*(\S+)\s*$", perm_text, re.MULTILINE)
        )
        self.assertEqual(
            perm_keys,
            {"pages": "write", "id-token": "write"},
            f"deploy permissions must be exactly pages:write and id-token:write, got {perm_keys}",
        )

        # deploy must use the github-pages environment
        self.assertIn("github-pages", self.deploy_job)

    def test_checksum_is_generated_from_inside_firmware_dist(self):
        """SHA256SUMS must record the plain basename 'merged-firmware.bin'.

        The download step places both release assets in a single
        'firmware-dist' folder, so a later 'sha256sum -c SHA256SUMS' run
        must find a checksum entry for the bare basename
        'merged-firmware.bin' with the working directory already changed
        into 'firmware-dist' (or an equivalent 'cd firmware-dist &&' style
        invocation), not the path-qualified
        'firmware-dist/merged-firmware.bin', which does not resolve when
        verifying from inside that directory.
        """
        checksum_step = re.search(
            r"Generate checksum\s*\n\s*run:\s*\|?\s*(.*?)(?=\n\s*- name:|\Z)",
            self.publish_job,
            re.DOTALL,
        )
        self.assertIsNotNone(checksum_step, "expected a 'Generate checksum' step in publish job")
        run_text = checksum_step.group(1)

        self.assertRegex(
            run_text,
            r"cd\s+firmware-dist",
            "checksum step must change into firmware-dist before hashing, so "
            "SHA256SUMS records the bare basename 'merged-firmware.bin'",
        )
        self.assertRegex(
            run_text,
            r"sha256sum\s+merged-firmware\.bin\s*>\s*SHA256SUMS",
            "expected 'sha256sum merged-firmware.bin > SHA256SUMS' run from "
            "inside firmware-dist (not a firmware-dist/-qualified path)",
        )
        self.assertNotIn(
            "firmware-dist/merged-firmware.bin > firmware-dist/SHA256SUMS",
            run_text,
            "checksum must not be generated with a firmware-dist/-qualified "
            "path, which breaks 'sha256sum -c SHA256SUMS' after downloading "
            "both assets into one folder",
        )

    def test_merge_bin_output_is_relative_to_build_directory(self):
        """idf.py merge-bin must write its output relative to its own cwd.

        espressif/esp-idf-ci-action runs 'idf.py merge-bin' with its
        working directory already set to the project's 'build' folder, so
        passing '--output build/merged-firmware.bin' makes esptool look for
        the nonexistent nested 'build/build/merged-firmware.bin' and raise
        FileNotFoundError. The command argument must instead be
        '--output merged-firmware.bin' (no 'build/' prefix), while the
        later check/upload steps must keep referring to the
        repository-relative 'build/merged-firmware.bin' path.
        """
        build_step = re.search(
            r"Build and merge firmware.*?\n\s*command:\s*([^\n]+)",
            self.verify_job,
            re.DOTALL,
        )
        self.assertIsNotNone(
            build_step, "expected a 'Build and merge firmware' step with a 'command:' line"
        )
        command_text = build_step.group(1)

        self.assertIn("merge-bin", command_text)
        self.assertRegex(
            command_text,
            r"--output\s+merged-firmware\.bin(?:\s|$)",
            "merge-bin command must use '--output merged-firmware.bin' "
            "(relative to the build directory it runs in), not a "
            "'build/'-prefixed path",
        )
        self.assertNotRegex(
            command_text,
            r"--output\s+build/merged-firmware\.bin",
            "merge-bin command must not pass 'build/merged-firmware.bin' as "
            "--output, since idf.py runs it from inside the build "
            "directory already and that would resolve to the nonexistent "
            "nested 'build/build/merged-firmware.bin'",
        )

        # The later check and upload steps must still use the
        # repository-relative path 'build/merged-firmware.bin'.
        self.assertIn("test -f build/merged-firmware.bin", self.verify_job)
        self.assertRegex(
            self.verify_job,
            r"path:\s*build/merged-firmware\.bin",
            "upload-artifact step must still reference 'build/merged-firmware.bin'",
        )


if __name__ == "__main__":
    unittest.main()
