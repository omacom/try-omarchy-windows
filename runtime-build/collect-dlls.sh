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
ucrt_bin=$(cygpath -m /ucrt64/bin)
copy_count=0

walk() {
    local target=$1
    [[ -z ${seen[$target]:-} ]] || return 0
    seen[$target]=1

    local path source_path filename
    while IFS= read -r path; do
        path=${path//\\//}
        if [[ $path == /ucrt64/bin/* ]]; then
            source_path=$path
        elif [[ $path == "$ucrt_bin"/* ]]; then
            source_path="/ucrt64/bin/$(basename "$path")"
        else
            continue
        fi
        filename=$(basename "$path")
        if [[ -z ${copied[$filename]:-} ]]; then
            cp "$source_path" "$bin_dir/$filename"
            copied[$filename]=$source_path
            ((copy_count += 1))
            walk "$source_path"
        elif [[ ${copied[$filename]} != "$source_path" ]]; then
            echo "dependency basename collision for $filename" >&2
            exit 1
        fi
    done < <(ldd "$target" 2>/dev/null | sed -nE 's/.* => ([^ ]+) \(.*/\1/p')
}

for target in "$bin_dir"/*.exe "$bin_dir"/*.dll; do
    [[ -f "$target" ]] && walk "$target"
done
if ((copy_count == 0)); then
    echo "ldd found no UCRT64 runtime dependencies" >&2
    exit 1
fi
printf '%s\n' "${copied[@]}" | sort -u >"$source_list"
echo "Collected $copy_count UCRT64 DLL dependencies"
