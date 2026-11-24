#!/bin/bash
# Video benchmark, identical on the Windows host (Git Bash, FFMPEG=ffmpeg.exe)
# and in the VM: 1080p60 H.264 source, NVDEC decode and NVENC encodes.
# Prints "<test> best_ms median_ms fps" over RUNS runs (wall time, start-up
# included).
FF=${FFMPEG:-ffmpeg}
RUNS=${RUNS:-3}
SRC=${SRC:-/tmp/vbench-src.mp4}
if [ -f /run/environment.d/90-virtio-dxg.conf ]; then
    while IFS= read -r l; do [ -n "$l" ] && export "$l"; done < /run/environment.d/90-virtio-dxg.conf
fi
[ -f "$SRC" ] || "$FF" -hide_banner -loglevel error -f lavfi -i testsrc2=size=1920x1080:rate=60 \
    -t 20 -c:v libx264 -preset veryfast -pix_fmt yuv420p "$SRC"
FRAMES=1200
ms() { echo $(( ($(date +%s%N) - $1) / 1000000 )); }
bench() {
    local name=$1; shift
    local times=() ok=1
    for i in $(seq 1 "$RUNS"); do
        local t0=$(date +%s%N)
        if ! "$FF" -hide_banner -nostats -loglevel error "$@" >/dev/null 2>/tmp/vbench-err.txt; then
            ok=0
            echo "  $name run $i: $(head -c 300 /tmp/vbench-err.txt | tr '
' ' ')" >>"${ERRLOG:-/tmp/vbench-errors.txt}"
        fi
        times+=("$(ms "$t0")")
    done
    local sorted=($(printf '%s\n' "${times[@]}" | sort -n))
    local best=${sorted[0]} med=${sorted[$((RUNS / 2))]}
    printf '%-22s best %6d ms  median %6d ms  %6.0f fps %s\n' "$name" "$best" "$med" \
        "$(echo "$FRAMES $best" | awk '{print $1 * 1000 / $2}')" "$([ $ok = 1 ] || echo FAILED)"
}
bench "decode CPU"            -i "$SRC" -f null -
bench "decode NVDEC"          -hwaccel cuda -hwaccel_output_format cuda -i "$SRC" -f null -
bench "encode H.264 NVENC"    -i "$SRC" -c:v h264_nvenc -preset p4 -f null -
bench "encode HEVC NVENC"     -i "$SRC" -c:v hevc_nvenc -preset p4 -f null -
bench "encode AV1 NVENC"      -i "$SRC" -c:v av1_nvenc -preset p4 -f null -
bench "transcode NVDEC>NVENC" -hwaccel cuda -hwaccel_output_format cuda -i "$SRC" -c:v h264_nvenc -preset p4 -f null -
