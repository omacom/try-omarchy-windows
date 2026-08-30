#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 BIN_DIRECTORY SOURCE_LIST" >&2
    exit 2
fi

bin_dir=$(cd "$1" && pwd)
source_list=$2
declare -A seen
declare -A copied

walk() {
    local target=$1
    [[ -z ${seen[$target]:-} ]] || return 0
    seen[$target]=1

    local path filename
    while IFS= read -r path; do
        [[ $path == /ucrt64/bin/* ]] || continue
        filename=$(basename "$path")
        if [[ -z ${copied[$filename]:-} ]]; then
            cp "$path" "$bin_dir/$filename"
            copied[$filename]=$path
            walk "$path"
        elif [[ ${copied[$filename]} != "$path" ]]; then
            echo "dependency basename collision for $filename" >&2
            exit 1
        fi
    done < <(ldd "$target" 2>/dev/null | sed -nE 's/.* => ([^ ]+) \(.*/\1/p')
}

for target in "$bin_dir"/*.exe "$bin_dir"/*.dll; do
    [[ -f "$target" ]] && walk "$target"
done
printf '%s\n' "${copied[@]}" | sort -u >"$source_list"
