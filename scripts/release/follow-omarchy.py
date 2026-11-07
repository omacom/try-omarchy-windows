#!/usr/bin/env python3
"""Propose pinned stable releases; merge only a fully validated candidate."""

import datetime
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
UPSTREAM = "omacom/omarchy"


def run(*args, cwd=ROOT):
    return subprocess.check_output(args, cwd=cwd, text=True).strip()


def gh(*args):
    return json.loads(run("gh", *args))


def version(value):
    match = re.fullmatch(r"v?(\d+)\.(\d+)\.(\d+)", value)
    if not match:
        raise ValueError(f"Not a stable release version: {value!r}")
    return tuple(map(int, match.groups()))


def newer_release(release, current):
    if release.get("draft") or release.get("prerelease"):
        return False
    return version(release["tag_name"]) > version(current)


def update_spec(spec, release, source, digest):
    upstream = spec["upstream"]
    upstream.update(
        repository=f"https://github.com/{UPSTREAM}",
        version=release["tag_name"].removeprefix("v"),
        versionFile=(source / "version").read_text().strip(),
        commit=run("git", "rev-parse", "HEAD", cwd=source),
        tree=run("git", "rev-parse", "HEAD^{tree}", cwd=source),
        treeSha256=digest,
        packageRelease=1,
    )


def full_ci_passed(workflow_run, jobs, sha):
    return (
        workflow_run.get("head_sha") == sha
        and workflow_run.get("event") == "workflow_dispatch"
        and workflow_run.get("conclusion") == "success"
        and bool(jobs)
        and all(job.get("conclusion") == "success" for job in jobs)
        and any(job.get("name") == "Guest image candidate" for job in jobs)
    )


def summary(message):
    print(message, flush=True)
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if path:
        with open(path, "a", encoding="utf-8") as stream:
            stream.write(message + "\n\n")


def validate_and_merge(repo, pr, branch, sha, lock_ok=True):
    started = datetime.datetime.now(datetime.timezone.utc)
    run("gh", "workflow", "run", "ci.yml", "--repo", repo, "--ref", branch,
        "-f", "build_guest=true")
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline:
        candidates = gh("api", f"repos/{repo}/actions/workflows/ci.yml/runs?branch={branch}&event=workflow_dispatch&per_page=30")["workflow_runs"]
        candidates = [item for item in candidates if item["head_sha"] == sha
                      and datetime.datetime.fromisoformat(item["created_at"].replace("Z", "+00:00")) >= started.replace(microsecond=0)]
        if candidates:
            ci = max(candidates, key=lambda item: item["id"])
            break
        time.sleep(5)
    else:
        raise RuntimeError("Dispatched CI was not found; leaving the PR as a draft")
    summary(f"Candidate CI: {ci['html_url']}")
    run("gh", "run", "watch", str(ci["id"]), "--repo", repo, "--exit-status", "--interval", "30")
    ci = gh("api", f"repos/{repo}/actions/runs/{ci['id']}")
    jobs = gh("api", "--paginate", "--slurp", f"repos/{repo}/actions/runs/{ci['id']}/attempts/{ci['run_attempt']}/jobs?per_page=100")
    jobs = [job for page in jobs for job in page["jobs"]]
    if not full_ci_passed(ci, jobs, sha):
        raise RuntimeError("Full CI including guest build and boot did not pass")
    if not lock_ok:
        raise RuntimeError("Package lock refresh failed; leaving the PR as a draft")
    live = gh("pr", "view", str(pr), "--repo", repo, "--json", "headRefOid,state,baseRefName")
    if live["headRefOid"] != sha or live["state"] != "OPEN" or live["baseRefName"] != "master":
        raise RuntimeError("PR changed during validation; refusing to merge")
    run("gh", "pr", "ready", str(pr), "--repo", repo)
    run("gh", "pr", "merge", str(pr), "--repo", repo, "--squash", "--auto", "--match-head-commit", sha)
    summary("Full CI passed. Requested merge of the tested commit under repository rules.")


def main():
    repo = os.environ["GITHUB_REPOSITORY"]
    if repo != "omacom/try-omarchy-windows":
        raise RuntimeError("Run this workflow in the upstream repository")
    release = gh("api", f"repos/{UPSTREAM}/releases/latest")
    tag = release["tag_name"]
    version(tag)
    branch = f"omarchy-release/{tag}"
    existing = gh("pr", "list", "--repo", repo, "--head", branch, "--state", "all", "--json", "number,state,url")
    if existing:
        summary(f"Release already proposed: {existing[0]['url']}. Repair or validate that PR instead of overwriting it.")
        return
    run("git", "config", "user.name", "github-actions[bot]")
    run("git", "config", "user.email", "41898282+github-actions[bot]@users.noreply.github.com")
    with tempfile.TemporaryDirectory(prefix="omarchy-release-") as work:
        work = Path(work)
        builder = work / "builder"
        run("bash", "scripts/release/build-guest.sh", "--source-only", "--output", str(builder))
        spec_path = builder / "guest/spec.json"
        spec = json.loads(spec_path.read_text())
        if not newer_release(release, spec["upstream"]["version"]):
            summary("No newer stable Omarchy release.")
            return
        source = work / "upstream"
        run("git", "-c", "core.autocrlf=false", "clone", "--depth", "1", "--branch", tag,
            f"https://github.com/{UPSTREAM}.git", str(source))
        module = importlib.util.spec_from_file_location("source_digest", builder / "guest/scripts/source-digest.py")
        digest_module = importlib.util.module_from_spec(module)
        module.loader.exec_module(digest_module)
        digest, _ = digest_module.digest_source(source)
        update_spec(spec, release, source, digest)
        spec_path.write_text(json.dumps(spec, indent=2) + "\n")
        lock_ok = True
        try:
            subprocess.run(["sudo", "bash", str(builder / "guest/build-container.sh"),
                            "--refresh-package-lock", str(builder / "guest/packages.lock.json")], check=True)
        except subprocess.CalledProcessError:
            lock_ok = False
            run("git", "restore", "guest/packages.lock.json", cwd=builder)
        run("git", "add", "guest/spec.json", "guest/packages.lock.json", cwd=builder)
        run("git", "commit", "-m", f"Upgrade Omarchy to {tag}", cwd=builder)
        last = max(int(path.name.split("-", 1)[0]) for path in (ROOT / "guest-build").glob("*.patch"))
        run("git", "format-patch", "-1", "--start-number", str(last + 1), "-o", str(ROOT / "guest-build"), cwd=builder)
        run("git", "checkout", "-b", branch)
        run("git", "add", "guest-build")
        run("git", "commit", "-m", f"Upgrade Omarchy to {tag}")
        sha = run("git", "rev-parse", "HEAD")
        run("git", "push", "origin", branch)
        body = work / "body.md"
        body.write_text(
            f"## Change\n\nUpgrade the guest to [{tag}](https://github.com/{UPSTREAM}/releases/tag/{tag}). "
            "Pin its commit, tree and normalized source digest. Keep reviewed backports in place so incompatibilities fail validation.\n\n"
            "## User check\n\nCreate a fresh VM and confirm its version and desktop. Upgrade a disposable older VM and verify files and Windows integrations.\n\n"
            f"## Validation\n\nPackage lock refresh: {'passed' if lock_ok else 'FAILED; manual compatibility work required'}. "
            "Full CI is explicitly dispatched with the guest image build and boot enabled. "
            "Automatic merge requires that entire run to pass for this exact commit. Windows hardware acceptance and signed release publication remain separate.\n",
            encoding="utf-8")
        try:
            url = run("gh", "pr", "create", "--repo", repo, "--draft", "--title", f"Upgrade Omarchy to {tag}",
                      "--body-file", str(body), "--head", branch, "--base", "master")
        except subprocess.CalledProcessError:
            summary(f"Branch retained. Bot PR creation was denied: https://github.com/{repo}/compare/master...{branch}?expand=1")
            raise
        summary(f"Upgrade draft: {url}")
        pr = gh("pr", "view", url, "--repo", repo, "--json", "number")["number"]
        validate_and_merge(repo, pr, branch, sha, lock_ok)


if __name__ == "__main__":
    main()
