#!/usr/bin/env bash

set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../.." && pwd)
artifacts=${1:-}
[[ -n $artifacts && -d $artifacts ]] || { echo "Usage: $0 ARTIFACT_DIR" >&2; exit 2; }

runtime_lock=${TRYOMARCHY_RUNTIME_LOCK:-"$repo_root/guest-build/runtime.lock.json"}
readarray -t runtime_archives < <(python3 - "$runtime_lock" <<'PY'
import json
import pathlib
import sys

lock = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
if set(lock) != {"runtime", "source"}:
    raise SystemExit("runtime lock must contain runtime and source entries")
for role in ("runtime", "source"):
    entry = lock[role]
    if set(entry) != {"url", "filename", "sha256"}:
        raise SystemExit(f"invalid {role} entry in runtime lock")
    print("\t".join((role, entry["url"], entry["filename"], entry["sha256"])))
PY
)
[[ ${#runtime_archives[@]} -eq 2 ]] || { echo "Invalid runtime lock" >&2; exit 1; }

declare -A seen_names=()
for archive in "${runtime_archives[@]}"; do
  IFS=$'\t' read -r role url name digest <<<"$archive"
  [[ $role == runtime || $role == source ]]
  [[ -n $url && $name != */* && $digest =~ ^[0-9a-f]{64}$ ]] || {
    echo "Invalid $role archive in runtime lock" >&2
    exit 1
  }
  [[ -z ${seen_names[$name]:-} ]] || { echo "Duplicate runtime archive: $name" >&2; exit 1; }
  seen_names[$name]=1
  if grep -qE "[[:space:]]${name}$" "$artifacts/SHA256SUMS"; then
    echo "$name is already present in SHA256SUMS" >&2
    exit 1
  fi
  curl --fail --location --retry 3 --output "$artifacts/$name" "$url"
  printf '%s  %s\n' "$digest" "$artifacts/$name" | sha256sum --check
  printf '%s  %s\n' "$digest" "$name" >>"$artifacts/SHA256SUMS"
done

(
  cd "$artifacts"
  sha256sum --check SHA256SUMS
)

sha256sum "$artifacts/SHA256SUMS"
