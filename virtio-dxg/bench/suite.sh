#!/bin/bash
# virtio-dxg test and benchmark suite. Runs as the desktop user inside the
# VM, with the session's GPU environment, and prints one line per result.
#   suite.sh [quick]     quick: shorter benchmark runs
S=/mnt/vdxgshare
Q=${1:-full}
# The session environment (virtio-dxg Mesa, NVIDIA libraries).
while IFS= read -r line; do
    [ -n "$line" ] && export "$line"
done < /run/environment.d/90-virtio-dxg.conf
export XDG_RUNTIME_DIR=/run/user/$(id -u)
export WAYLAND_DISPLAY=$(basename "$(ls $XDG_RUNTIME_DIR/wayland-? 2>/dev/null | head -1)")
export HYPRLAND_INSTANCE_SIGNATURE=$(ls -t $XDG_RUNTIME_DIR/hypr 2>/dev/null | head -1)
W=$(mktemp -d)
pass=0 fail=0
# An idle session turns the display off, and the compositor then throttles
# every client: keep it on.
bash "$S/awake.sh"
result() { printf '%-34s %s\n' "$1" "$2"; }
run() {     # run NAME SECONDS CMD... -> output in $W/NAME.log
    local name=$1 secs=$2; shift 2
    timeout -k 5 "$secs" "$@" >"$W/$name.log" 2>&1
    local rc=$?
    [ $rc -eq 0 ] && pass=$((pass + 1)) || fail=$((fail + 1))
    [ $rc -eq 124 ] && result "$name" "TIMEOUT after ${secs}s"
    [ $rc -ne 0 ] && [ $rc -ne 124 ] && result "$name" "FAILED rc=$rc: $(tail -2 "$W/$name.log" | tr '\n' ' ' | cut -c1-120)"
    return $rc
}

echo "== inventory"
run eglinfo 30 eglinfo -B -p surfaceless &&
    result "OpenGL (surfaceless)" "$(grep -m1 'OpenGL core profile renderer' "$W/eglinfo.log" | cut -d: -f2-) / $(grep -m1 'OpenGL core profile version' "$W/eglinfo.log" | cut -d: -f2- | cut -c1-30)"
run vulkaninfo 30 vulkaninfo --summary &&
    result "Vulkan" "$(grep -m1 deviceName "$W/vulkaninfo.log" | cut -d= -f2-) / API $(grep -m1 apiVersion "$W/vulkaninfo.log" | awk '{print $3}') / $(grep -m1 driverName "$W/vulkaninfo.log" | cut -d= -f2-)"
run vainfo 30 vainfo --display drm --device /dev/dri/renderD128 &&
    result "VA-API profiles" "$(grep -c 'VAProfile' "$W/vainfo.log") ($(grep -oE 'VAProfile(H264High|HEVCMain10|AV1Profile0|VP9Profile0)[^ ]*\s*:\s*VAEntrypoint(VLD|EncSlice)' "$W/vainfo.log" | sed 's/VAProfile//; s/VAEntrypoint//; s/ //g' | tr '\n' ' '))"
run nvidia-smi 30 /usr/lib/ld-linux-x86-64.so.2 /usr/lib/wsl/lib/nvidia-smi --query-gpu=name,driver_version,memory.total,temperature.gpu,power.draw --format=csv,noheader &&
    result "nvidia-smi" "$(cat "$W/nvidia-smi.log")"

echo "== OpenGL (Mesa d3d12 on NVIDIA)"
if [ "$Q" = quick ]; then GB="-b build:use-vbo=true -b texture -b shading:shading=phong -b refract -b terrain"; else GB=""; fi
run glmark2-offscreen 600 glmark2-es2-wayland --off-screen -s 1920x1080 $GB &&
    result "glmark2 1080p off-screen" "score $(grep -oE 'glmark2 Score: [0-9]+' "$W/glmark2-offscreen.log" | awk '{print $3}')"
run glmark2-window 600 glmark2-es2-wayland -s 1280x720 $GB &&
    result "glmark2 720p windowed" "score $(grep -oE 'glmark2 Score: [0-9]+' "$W/glmark2-window.log" | awk '{print $3}')"
cp "$W"/glmark2-*.log "$S/out/" 2>/dev/null

echo "== Vulkan (Dozen on NVIDIA D3D12)"
run vkmark 600 vkmark --winsys wayland -s 1280x720 &&
    result "vkmark 720p" "score $(grep -oE 'vkmark Score: [0-9]+' "$W/vkmark.log" | awk '{print $3}')"
cp "$W"/vkmark.log "$S/out/" 2>/dev/null

run vkcube 60 vkcube --wsi wayland --c 600 &&
    result "vkcube 600 frames (Dozen, Wayland)" "ok"
# The desktop's own frame rate: vkcube with vsync runs at the compositor's.
run fifo 120 bash "$S/fifo.sh" 600 &&
    result "desktop vsync (vkcube FIFO)" "$(grep -oE '= [0-9]+ fps' "$W/fifo.log" | tr -d '= ') at $(hyprctl monitors 2>/dev/null | grep -oE '@[0-9]+' | head -1 | tr -d @) Hz"
echo "== CUDA (NVIDIA libcuda)"
if [ -x "$S/cudatest" ] || [ -f "$S/cudatest" ]; then
    run cuda 120 /usr/lib/ld-linux-x86-64.so.2 "$S/cudatest" &&
        result "CUDA" "$(grep -iE 'GB/s|launch|correct|PASS' "$W/cuda.log" | tr '\n' ';' | cut -c1-160)"
fi

echo "== Video"
SRC=$W/src.mp4
run make-clip 300 ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=1920x1080:rate=60 -t 10 -c:v libx264 -preset veryfast -pix_fmt yuv420p "$SRC"
if [ -f "$SRC" ]; then
    # Frames over ffmpeg's own wall time (-benchmark's rtime): its fps
    # field reads 0.0 for runs that end within its first update.
    fps() {
        local f r
        f=$(tr "\r" "\n" <"$W/$1.log" | grep -oE "frame= *[0-9]+" | tail -1 | tr -dc 0-9)
        r=$(grep -oE "rtime=[0-9.]+" "$W/$1.log" | tail -1 | cut -d= -f2)
        awk -v f="${f:-0}" -v r="${r:-0}" 'BEGIN { if (r > 0) printf "%.0f", f / r; else print 0 }'
    }
    run decode-sw 120 ffmpeg -hide_banner -benchmark -i "$SRC" -f null - &&
        result "H.264 1080p decode, CPU" "$(fps decode-sw) fps"
    run decode-vaapi 120 ffmpeg -hide_banner -benchmark -hwaccel vaapi -hwaccel_output_format vaapi -i "$SRC" -f null - &&
        result "H.264 1080p decode, VA-API d3d12" "$(fps decode-vaapi) fps"
    run decode-nvdec 120 ffmpeg -hide_banner -benchmark -hwaccel cuda -hwaccel_output_format cuda -i "$SRC" -f null - &&
        result "H.264 1080p decode, NVDEC" "$(fps decode-nvdec) fps"
    run encode-vaapi 120 ffmpeg -hide_banner -benchmark -vaapi_device /dev/dri/renderD128 -i "$SRC" -vf 'format=nv12,hwupload' -c:v h264_vaapi -f null - &&
        result "H.264 1080p encode, VA-API d3d12" "$(fps encode-vaapi) fps"
    run encode-nvenc 120 ffmpeg -hide_banner -benchmark -i "$SRC" -c:v h264_nvenc -preset p4 -f null - &&
        result "H.264 1080p encode, NVENC" "$(fps encode-nvenc) fps"
    run encode-hevc-nvenc 120 ffmpeg -hide_banner -benchmark -i "$SRC" -c:v hevc_nvenc -preset p4 -f null - &&
        result "HEVC 1080p encode, NVENC" "$(fps encode-hevc-nvenc) fps"
    run encode-av1-nvenc 120 ffmpeg -hide_banner -benchmark -i "$SRC" -c:v av1_nvenc -preset p4 -f null - &&
        result "AV1 1080p encode, NVENC" "$(fps encode-av1-nvenc) fps"
fi
cp "$W"/decode-*.log "$W"/encode-*.log "$S/out/" 2>/dev/null
echo "== Image"
MJ=$W/src-mjpeg.mkv
run make-mjpeg 120 ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=1920x1080:rate=30 -t 10 -c:v mjpeg -q:v 3 "$MJ"
if [ -f "$MJ" ]; then
    run jpeg-cpu 120 ffmpeg -hide_banner -benchmark -i "$MJ" -f null - &&
        result "JPEG 1080p decode, CPU" "$(fps jpeg-cpu) fps"
    run jpeg-nvdec 120 ffmpeg -hide_banner -benchmark -c:v mjpeg_cuvid -i "$MJ" -f null - &&
        result "JPEG 1080p decode, NVDEC" "$(fps jpeg-nvdec) fps"
fi
run scale-cuda 120 ffmpeg -hide_banner -benchmark -hwaccel cuda -hwaccel_output_format cuda -i "$SRC" -vf scale_cuda=3840:2160 -f null - &&
    result "1080p to 4K scaling, CUDA" "$(fps scale-cuda) fps"

echo "== NVIDIA libraries the guest can load"
for lib in libnvoptix.so.1 libnvidia-ngx.so.1 libnvidia-opticalflow.so.1 libnvidia-ml.so.1 libnvcuvid.so.1 libnvidia-encode.so.1; do
    if python3 -c "import ctypes, sys; ctypes.CDLL(sys.argv[1])" "$lib" 2>/dev/null; then
        result "load $lib" "ok"; pass=$((pass + 1))
    else
        result "load $lib" "FAILED"; fail=$((fail + 1))
    fi
done


echo "== D3D12"
if [ -f "$S/d3d12test" ]; then
    run d3d12 60 /usr/lib/ld-linux-x86-64.so.2 "$S/d3d12test" &&
        result "D3D12 device" "$(tail -3 "$W/d3d12.log" | tr '\n' ' ' | cut -c1-140)"
fi

echo "== guest kernel"
result "dxgkrnl errors since boot" "$(journalctl -k -b --no-pager 2>/dev/null | grep -ciE 'dxg.*(fail|error)' ) lines"
echo "== summary: $pass passed, $fail failed"
rm -rf "$W"
