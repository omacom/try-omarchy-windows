#!/usr/bin/env python3
"""Merge a fetched upstream ref into a clean checkout, without pushing/resetting."""
import argparse
import subprocess


def git(*args, check=True):
    return subprocess.run(["git", *args], text=True, capture_output=True, check=check)


def prepare(ref):
    if git("status", "--porcelain").stdout.strip():
        raise RuntimeError("Upstream sync requires a clean checkout")
    git("rev-parse", "--verify", f"{ref}^{{commit}}")
    relation = git("merge-base", "--is-ancestor", ref, "HEAD", check=False)
    if relation.returncode == 0:
        return False
    if relation.returncode != 1:
        raise RuntimeError(relation.stderr)
    # No rebase/force/reset: the existing fork commits are parents of the result.
    merge = git("merge", "--no-ff", "--no-edit", ref, check=False)
    if merge.returncode:
        git("merge", "--abort", check=False)
        raise RuntimeError("Upstream merge needs manual resolution; fork head retained:\n" + merge.stdout + merge.stderr)
    return True


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ref", default="refs/remotes/upstream/master")
    options = parser.parse_args()
    print("changed=" + str(prepare(options.ref)).lower())
