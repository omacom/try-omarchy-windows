import copy
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


module = importlib.util.spec_from_file_location("follow_omarchy", Path(__file__).with_name("follow-omarchy.py"))
follow = importlib.util.module_from_spec(module)
module.loader.exec_module(follow)


class ReleaseTests(unittest.TestCase):
    def test_only_new_stable_releases(self):
        self.assertTrue(follow.newer_release({"tag_name": "v4.5.0"}, "4.0.5"))
        self.assertTrue(follow.newer_release({"tag_name": "v4.10.0"}, "4.9.0"))
        for tag in ("v4.0.5", "v4.0.4"):
            self.assertFalse(follow.newer_release({"tag_name": tag}, "4.0.5"))
        for flag in ("draft", "prerelease"):
            self.assertFalse(follow.newer_release({"tag_name": "v4.5.0", flag: True}, "4.0.5"))

    def test_rejects_untrusted_or_prerelease_tags(self):
        for tag in ("v4.5.0-rc1", "main", "v4.5.0;true", "../v4.5.0", "v4.5", "v4.5.0\n"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                follow.version(tag)

    def test_pin_keeps_backports_and_actual_version_file(self):
        spec = {"upstream": {"version": "4.0.4", "channel": "quattro", "packageRelease": 11},
                "authenticity": {"backports": [{"id": "reviewed", "beforeSha256": "old"}]}}
        before = copy.deepcopy(spec["authenticity"])
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            (source / "version").write_text("4.5.0.alpha\n")
            with patch.object(follow, "run", side_effect=["commit", "tree"]):
                follow.update_spec(spec, {"tag_name": "v4.5.0"}, source, "digest")
        self.assertEqual(spec["upstream"], {
            "repository": "https://github.com/omacom/omarchy", "version": "4.5.0",
            "versionFile": "4.5.0.alpha", "channel": "quattro", "packageRelease": 1,
            "commit": "commit", "tree": "tree", "treeSha256": "digest"})
        self.assertEqual(spec["authenticity"], before)


class MergeGateTests(unittest.TestCase):
    def setUp(self):
        self.ci = {"id": 12, "head_sha": "tested", "event": "workflow_dispatch", "conclusion": "success", "run_attempt": 1,
                   "created_at": "2099-01-01T00:00:00Z", "html_url": "https://github.com/example/actions/runs/12"}
        self.jobs = [{"name": "Launcher", "conclusion": "success"},
                     {"name": "Guest image candidate", "conclusion": "success"}]

    def test_requires_complete_successful_guest_candidate(self):
        self.assertTrue(follow.full_ci_passed(self.ci, self.jobs, "tested"))
        self.assertFalse(follow.full_ci_passed(self.ci, self.jobs, "new-head"))
        self.assertFalse(follow.full_ci_passed(self.ci, [], "tested"))
        self.assertFalse(follow.full_ci_passed(self.ci, self.jobs[:1], "tested"))
        for result in ("skipped", "cancelled", "failure", None):
            jobs = copy.deepcopy(self.jobs)
            jobs[1]["conclusion"] = result
            self.assertFalse(follow.full_ci_passed(self.ci, jobs, "tested"))
        for field, value in (("event", "pull_request"), ("conclusion", "failure")):
            ci = dict(self.ci, **{field: value})
            self.assertFalse(follow.full_ci_passed(ci, self.jobs, "tested"))

    def exercise_merge(self, live_sha="tested", lock_ok=True, jobs=None, watch_fails=False):
        responses = [{"workflow_runs": [self.ci]}, self.ci, [{"jobs": self.jobs if jobs is None else jobs}],
                     {"headRefOid": live_sha, "state": "OPEN", "baseRefName": "master"}]
        def command(*args, **kwargs):
            if watch_fails and args[:3] == ("gh", "run", "watch"):
                raise follow.subprocess.CalledProcessError(1, args)
            return ""
        with patch.object(follow, "gh", side_effect=responses), patch.object(follow, "run", side_effect=command) as run, patch.object(follow, "summary"):
            try:
                follow.validate_and_merge("owner/repo", 1, "omarchy-release/v4.5.0", "tested", lock_ok)
            except (RuntimeError, follow.subprocess.CalledProcessError):
                pass
        return [call.args for call in run.call_args_list]

    def test_merge_uses_tested_sha_and_repository_rules(self):
        commands = self.exercise_merge()
        self.assertIn(("gh", "pr", "ready", "1", "--repo", "owner/repo"), commands)
        self.assertIn(("gh", "pr", "merge", "1", "--repo", "owner/repo", "--squash", "--auto", "--match-head-commit", "tested"), commands)
        self.assertIn(("gh", "workflow", "run", "ci.yml", "--repo", "owner/repo", "--ref", "omarchy-release/v4.5.0", "-f", "build_guest=true"), commands)

    def test_never_merges_failed_or_changed_candidates(self):
        for options in ({"live_sha": "changed"}, {"lock_ok": False}, {"jobs": self.jobs[:1]}, {"watch_fails": True}):
            with self.subTest(options=options):
                commands = self.exercise_merge(**options)
                self.assertFalse(any(command[:3] in (("gh", "pr", "merge"), ("gh", "pr", "ready")) for command in commands))


class IdempotenceTests(unittest.TestCase):
    def test_existing_proposal_is_never_overwritten(self):
        with patch.dict(follow.os.environ, {"GITHUB_REPOSITORY": "omacom/try-omarchy-windows"}), patch.object(
            follow, "gh", side_effect=[{"tag_name": "v4.5.0"}, [{"url": "existing-pr", "state": "CLOSED"}]]
        ), patch.object(follow, "run") as run, patch.object(follow, "summary"):
            follow.main()
        run.assert_not_called()

    def test_forks_cannot_publish_or_merge(self):
        with patch.dict(follow.os.environ, {"GITHUB_REPOSITORY": "fork/try-omarchy-windows"}), patch.object(follow, "gh") as gh:
            with self.assertRaises(RuntimeError):
                follow.main()
        gh.assert_not_called()


if __name__ == "__main__":
    unittest.main()
