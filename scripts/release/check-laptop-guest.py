#!/usr/bin/env python3
"""Check a running physical-laptop guest and compare seeded data across updates.

This does not launch, update, stop or repair the VM. Seed only a disposable copy.
Results describe guest telemetry; listening, input feel and signed updater
acceptance remain separate checks.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time


REMOTE = r'''
import hashlib, json, os, pathlib, pwd, subprocess

def run(*args):
    p = subprocess.run(args, capture_output=True, text=True, timeout=15)
    if p.returncode:
        raise RuntimeError(args[0] + " failed: " + p.stderr.strip()[:400])
    return p.stdout.strip()

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

home = pathlib.Path.home()
root = home / ".local/state/try-omarchy-release-test" / config["run_id"]
prefs = home / ".config/try-omarchy-release-test" / config["run_id"]
if config["seed"]:
    root.mkdir(parents=True, exist_ok=False)
    prefs.mkdir(parents=True, exist_ok=False)
    (root / "nested empty").mkdir()
    (root / "Unicode-中文-é.txt").write_text("release acceptance\n中文 é\n\n", encoding="utf-8")
    (root / "binary.bin").write_bytes(os.urandom(65536))
    (prefs / "preferences.json").write_text('{"theme":"preserve-me","enabled":true}\n')
if not root.is_dir() or not prefs.is_dir():
    raise RuntimeError("seed data absent; use seed before upgrading the copy")
files = {str(p.relative_to(home)): digest(p) for base in (root, prefs)
         for p in sorted(base.rglob("*")) if p.is_file()}
for relative in config["preserve"]:
    path = home / relative
    if not path.is_file() or not path.resolve().is_relative_to(home.resolve()):
        raise RuntimeError("preservation file missing or outside home: " + relative)
    files[relative] = digest(path)
directories = sorted(str(p.relative_to(home)) for p in root.rglob("*") if p.is_dir())
account = pwd.getpwuid(os.getuid())
identity = {"uid": account.pw_uid, "gid": account.pw_gid,
            "home": account.pw_dir, "machine_id_sha256": digest(pathlib.Path("/etc/machine-id"))}
checks, facts = [], {}
def check(name, action):
    try:
        value = action()
        checks.append({"name": name, "passed": True})
        facts[name] = value
    except Exception as error:
        checks.append({"name": name, "passed": False, "error": str(error)[:600]})

def active_services():
    services = ["pipewire", "pipewire-pulse", "wireplumber", "clipboard-bridge",
                "omarchy-windows-audio-bridge", "omarchy-windows-camera-bridge"]
    for service in services:
        if run("systemctl", "--user", "is-active", service) != "active":
            raise RuntimeError(service + " not active")
    return services
check("bridge_services", active_services)

def desktop():
    monitors = json.loads(run("systemd-run", "--user", "--wait", "--pipe", "--collect",
                              "hyprctl", "-j", "monitors"))
    usable = [m for m in monitors if m.get("width", 0) >= 320 and m.get("height", 0) >= 200
              and m.get("dpmsStatus") and not m.get("disabled")]
    if not usable:
        raise RuntimeError("no active usable compositor output")
    return [{k: m.get(k) for k in ("name", "width", "height", "scale")} for m in usable]
check("desktop_output", desktop)

def audio():
    sinks = json.loads(run("pactl", "--format=json", "list", "sinks"))
    transport = [s for s in sinks if s.get("properties", {}).get("alsa.card_name") == "VirtIO SoundCard"]
    virtual = [s for s in sinks if s.get("name", "").startswith("omarchy_windows_output_")]
    if len(transport) != 1 or not virtual:
        raise RuntimeError("missing unique VirtIO transport or playable Windows routes")
    volumes = [c["value"] for c in transport[0]["volume"].values()]
    if config["expect_unity"] and (not volumes or any(v != 65536 for v in volumes)):
        raise RuntimeError("hidden transport gain is not unity: " + repr(volumes))
    default = run("pactl", "get-default-sink")
    if default not in [s["name"] for s in virtual]:
        raise RuntimeError("default sink is not a Windows route")
    return {"transport_gain": volumes, "transport_muted": transport[0].get("mute"),
            "route_count": len(virtual), "default": default}
check("audio_routes", audio)

def network():
    # Real HTTPS, no credentials or remote writes. Bound the request.
    return run("curl", "--fail", "--silent", "--show-error", "--max-time", "10",
               "--output", "/dev/null", "--write-out", "%{http_code}", "https://tryomarchy.com")
check("guest_https", network)
check("compat_version", lambda: pathlib.Path("/usr/share/try-omarchy/compat-version").read_text().strip())
check("battery_module_version", lambda: pathlib.Path("/sys/module/try_omarchy_battery/version").read_text().strip())
check("audio_bridge_sha256", lambda: digest(pathlib.Path("/usr/local/bin/omarchy-windows-audio-bridge")))
facts["kernel"] = run("uname", "-r")
facts["failed_user_units"] = run("systemctl", "--user", "--failed", "--no-legend", "--no-pager").splitlines()
facts["root_free_bytes"] = os.statvfs("/").f_bavail * os.statvfs("/").f_frsize
print(json.dumps({"identity": identity, "files": files, "directories": directories,
                  "checks": checks, "facts": facts}, ensure_ascii=False))
'''


def evaluate(report, baseline=None, expected=None):
    failures = [c["name"] + ": " + c.get("error", "failed")
                for c in report["checks"] if not c["passed"]]
    if report["facts"].get("failed_user_units"):
        failures.append("failed user units: " + "; ".join(report["facts"]["failed_user_units"]))
    if report["facts"].get("root_free_bytes", 0) < 256 * 1024**2:
        failures.append("less than 256 MiB guest root free space")
    if baseline:
        for field in ("identity", "files", "directories"):
            if report[field] != baseline[field]:
                failures.append(field + " changed across update/restart")
    for key, value in (expected or {}).items():
        if value and report["facts"].get(key) != value:
            failures.append(key + " differs from the pinned candidate")
    compat = (expected or {}).get("compat_version")
    if compat and ":" in compat and report["facts"].get("kernel") != compat.split(":", 1)[1]:
        failures.append("running kernel differs from the pinned candidate")
    return failures


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("phase", choices=("seed", "verify"))
    p.add_argument("--host", required=True, help="SSH destination, e.g. omarchy@127.0.0.1")
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--key", type=Path, required=True)
    p.add_argument("--known-hosts", type=Path, required=True)
    p.add_argument("--run-id", required=True)
    p.add_argument("--output", type=Path, required=True, help="new private JSON evidence file")
    p.add_argument("--baseline", type=Path, help="seed report for verify")
    p.add_argument("--preserve", action="append", default=[], help="existing file relative to guest home to hash before/after")
    p.add_argument("--expect-compat", help="full marker, e.g. 48:7.2.8-arch1-2")
    p.add_argument("--expect-module")
    p.add_argument("--expect-audio-sha256")
    p.add_argument("--expect-unity", action="store_true", help="only for a controlled test with no manual transport gain")
    a = p.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}", a.run_id):
        p.error("run ID must contain only letters, digits, underscores and hyphens")
    if not 1 <= a.port <= 65535 or a.host.startswith("-"):
        p.error("invalid SSH host or port")
    if a.phase == "verify" and not a.baseline:
        p.error("verify requires --baseline")
    if a.output.exists():
        p.error("output already exists; choose a new phase report")
    for path in (a.key, a.known_hosts):
        if not path.is_file():
            p.error("missing SSH key or pinned known-host file")
    baseline = json.loads(a.baseline.read_text()) if a.baseline else None
    if baseline is not None:
        if not isinstance(baseline, dict) or baseline.get("run_id") != a.run_id:
            p.error("baseline belongs to a different run")
        if baseline.get("phase") != "seed" or baseline.get("passed") is not True:
            p.error("baseline must be a passing seed report")
        if (not isinstance(baseline.get("identity"), dict) or not baseline["identity"]
                or not isinstance(baseline.get("files"), dict) or not baseline["files"]
                or not isinstance(baseline.get("directories"), list)
                or not isinstance(baseline.get("facts"), dict)
                or not isinstance(baseline.get("checks"), list) or not baseline["checks"]):
            p.error("baseline preservation evidence is incomplete")
    preserve = baseline.get("preserve_paths", []) if baseline else a.preserve
    if baseline and a.preserve and a.preserve != preserve:
        p.error("preservation paths differ from baseline")
    if any(Path(name).is_absolute() or ".." in Path(name).parts or not name for name in preserve):
        p.error("preservation paths must stay within guest home")
    config = {"seed": a.phase == "seed", "run_id": a.run_id, "expect_unity": a.expect_unity,
              "preserve": preserve}
    command = ["ssh", "-i", str(a.key), "-p", str(a.port), "-o", "BatchMode=yes",
               "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=10",
               "-o", "UserKnownHostsFile=" + str(a.known_hosts), a.host, "python3 -"]
    started = time.monotonic()
    result = subprocess.run(command, input="config=" + repr(config) + "\n" + REMOTE,
                            capture_output=True, text=True, timeout=150)
    if result.returncode:
        raise RuntimeError("guest check failed: " + result.stderr.strip()[-1000:])
    report = json.loads(result.stdout)
    report.update(run_id=a.run_id, phase=a.phase, preserve_paths=preserve,
                  elapsed_seconds=round(time.monotonic()-started, 2))
    report["failures"] = evaluate(report, baseline, {
        "compat_version": a.expect_compat, "battery_module_version": a.expect_module,
        "audio_bridge_sha256": a.expect_audio_sha256})
    report["passed"] = not report["failures"]
    report["scope"] = "guest telemetry and seeded preservation; not signed update, UI feel or sound acceptance"
    a.output.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(a.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print(("PASS" if report["passed"] else "FAIL") + " " + str(a.output))
    for failure in report["failures"]:
        print("  " + failure)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print("FAIL " + str(error), file=sys.stderr)
        sys.exit(1)
