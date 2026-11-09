#!/bin/bash
# Start-up versus steady state: each test with 60 and with 1200 frames
# (best of RUNS). Steady fps = 1140 / (t1200 - t60).
FF=${FFMPEG:-ffmpeg}
RUNS=${RUNS:-3}
SRC=${SRC:-/tmp/vbench-src.mp4}
if [ -f /run/environment.d/90-virtio-dxg.conf ]; then
    while IFS= read -r l; do [ -n "$l" ] && export "$l"; done < /run/environment.d/90-virtio-dxg.conf
fi
best() {
    local b=999999999
    for i in $(seq 1 "$RUNS"); do
        local t0=$(date +%s%N)
        "$FF" -hide_banner -nostats -loglevel error "$@" >/dev/null 2>&1
        local t=$(( ($(date +%s%N) - t0) / 1000000 ))
        [ $t -lt $b ] && b=$t
    done
    echo $b
}
row() {
    local name=$1; shift
    local pre=() post=()
    while [ "$1" != -- ]; do pre+=("$1"); shift; done; shift
    post=("$@")
    local s=$(best "${pre[@]}" -i "$SRC" -frames:v 60 "${post[@]}")
    local f=$(best "${pre[@]}" -i "$SRC" -frames:v 1200 "${post[@]}")
    printf '%-20s start-up %5d ms   steady %6.0f fps\n' "$name" "$s" \
        "$(awk -v s=$s -v f=$f 'BEGIN{print 1140 * 1000 / (f - s)}')"
}
row "decode NVDEC" -hwaccel cuda -hwaccel_output_format cuda -- -f null -
row "encode H.264 NVENC" -- -c:v h264_nvenc -preset p4 -f null -
row "encode HEVC NVENC" -- -c:v hevc_nvenc -preset p4 -f null -
