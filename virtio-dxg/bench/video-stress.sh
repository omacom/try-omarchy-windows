#!/bin/bash
# Mixed video stress: NVDEC decode, NVENC H.264/HEVC/AV1 and a transcode, back
# to back, ROUNDS times. Prints failures with their first error line.
while IFS= read -r l; do [ -n "$l" ] && export "$l"; done < /run/environment.d/90-virtio-dxg.conf
SRC=/tmp/vbench-src.mp4
[ -f "$SRC" ] || ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=1920x1080:rate=60 \
    -t 20 -c:v libx264 -preset veryfast -pix_fmt yuv420p "$SRC"
runs=0 fails=0
t() {
    runs=$((runs + 1))
    sleep "${PAUSE:-0}"
    if ! ffmpeg -hide_banner -nostats -loglevel error "$@" >/dev/null 2>/tmp/vs-err.txt; then
        fails=$((fails + 1))
        echo "round $r $(echo "$*" | grep -oE '(h264|hevc|av1)_nvenc|hwaccel cuda' | head -1): $(head -1 /tmp/vs-err.txt | cut -c1-90)"
    fi
}
for r in $(seq 1 "${ROUNDS:-6}"); do
    t -hwaccel cuda -hwaccel_output_format cuda -i "$SRC" -f null -
    t -i "$SRC" -c:v h264_nvenc -preset p4 -f null -
    t -i "$SRC" -c:v hevc_nvenc -preset p4 -f null -
    t -i "$SRC" -c:v av1_nvenc -preset p4 -f null -
    t -hwaccel cuda -hwaccel_output_format cuda -i "$SRC" -c:v h264_nvenc -preset p4 -f null -
done
echo "video stress: $fails of $runs runs failed"
