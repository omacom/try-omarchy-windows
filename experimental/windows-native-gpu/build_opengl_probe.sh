#!/bin/sh
# Compile the real EGL/OpenGL guest probe against an explicitly supplied Mesa
# prefix. Building does not install drivers or run against the host WSL GPU.
set -eu
if [ "$#" -ne 2 ]; then
    echo 'Usage: build_opengl_probe.sh non-GLVND-Mesa-prefix output-directory' >&2
    exit 2
fi
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mesa=$(CDPATH= cd -- "$1" && pwd)
mkdir -p "$2"
output=$(CDPATH= cd -- "$2" && pwd)
exit_define=
if [ "${WDDM_BRIDGE_TEST_EXIT_LIFETIME:-0}" = 1 ]; then
    exit_define=-DWDDM_BRIDGE_EXIT_LIFETIME_TEST=1
    dxg=${WDDM_BRIDGE_DXG_HEADERS:?Supply libdxg source for the exit diagnostic}
    directx=${WDDM_BRIDGE_DIRECTX_HEADERS:?Supply DirectX-Headers source}
    "${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -shared -fPIC \
        -isystem "$dxg/include" -isystem "$directx/include" -isystem "$directx/include/wsl/stubs" \
        "$base/exit_lifetime_probe.cpp" -o "$output/exit-lifetime-probe.so"
fi
if [ "${WDDM_BRIDGE_TEST_GBM:-0}" = 1 ]; then
    for option in "${WDDM_BRIDGE_TEST_NATIVE_IMAGES:-0}" "${WDDM_BRIDGE_TEST_NATIVE_SYNC_FILES:-0}" "${WDDM_BRIDGE_TEST_NATIVE_SYNC_LINEAGE:-0}" "${WDDM_BRIDGE_TEST_EGL_NATIVE_SYNC:-0}" "${WDDM_BRIDGE_TEST_RESOURCE_FDS:-0}" "${WDDM_BRIDGE_TEST_MESA_FENCE_FDS:-0}" "${WDDM_BRIDGE_TEST_SYNC_FDS:-0}"; do
        if [ "$option" = 1 ]; then echo 'Select one explicit GBM/image diagnostic' >&2; exit 2; fi
    done
    dependencies=${WDDM_BRIDGE_DRM_DEPENDENCIES:?Supply pinned libdrm dependency prefix}
    "${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -isystem "$mesa/include" \
        -isystem "$dependencies/usr/include" -isystem "$dependencies/usr/include/libdrm" \
        -c "$base/gbm_native_probe.cpp" -o "$output/gbm-native-probe.o"
    "${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror $exit_define -DWDDM_BRIDGE_GBM_TEST=1 \
        -isystem "$mesa/include" -c "$base/egl_gl_probe.c" -o "$output/egl-gl-probe.o"
    "${CXX:-c++}" "$output/egl-gl-probe.o" "$output/gbm-native-probe.o" \
        -L"$mesa/lib" -L"$dependencies/usr/lib/x86_64-linux-gnu" -Wl,-rpath-link,"$mesa/lib" \
        -l:libEGL.so.1 -l:libgbm.so.1 -l:libdrm.so.2 -ldl -o "$output/egl-gl-probe"
elif [ "${WDDM_BRIDGE_TEST_NATIVE_IMAGES:-0}" = 1 ]; then
    for option in "${WDDM_BRIDGE_TEST_NATIVE_SYNC_FILES:-0}" "${WDDM_BRIDGE_TEST_NATIVE_SYNC_LINEAGE:-0}" "${WDDM_BRIDGE_TEST_EGL_NATIVE_SYNC:-0}" "${WDDM_BRIDGE_TEST_RESOURCE_FDS:-0}" "${WDDM_BRIDGE_TEST_MESA_FENCE_FDS:-0}" "${WDDM_BRIDGE_TEST_SYNC_FDS:-0}"; do
        if [ "$option" = 1 ]; then echo 'Select one explicit image/FD diagnostic' >&2; exit 2; fi
    done
    source=${WDDM_BRIDGE_MESA_SOURCE:?Supply pinned Mesa source}
    directx=${WDDM_BRIDGE_DIRECTX_HEADERS:?Supply DirectX-Headers source}
    image_define=-DWDDM_BRIDGE_NATIVE_IMAGE_TEST=1
    if [ "${WDDM_BRIDGE_TEST_NATIVE_IMAGE_DISABLED:-0}" = 1 ]; then image_define="$image_define -DWDDM_BRIDGE_NATIVE_IMAGE_DISABLED=1"; fi
    if [ "${WDDM_BRIDGE_TEST_NATIVE_IMAGE_BGRA:-0}" = 1 ]; then image_define="$image_define -DWDDM_BRIDGE_NATIVE_IMAGE_BGRA=1"; fi
    if [ "${WDDM_BRIDGE_TEST_NATIVE_IMAGE_BGRX:-0}" = 1 ]; then image_define="$image_define -DWDDM_BRIDGE_NATIVE_IMAGE_BGRX=1"; fi
    if [ "${WDDM_BRIDGE_TEST_NATIVE_IMAGE_RGBX:-0}" = 1 ]; then image_define="$image_define -DWDDM_BRIDGE_NATIVE_IMAGE_RGBX=1"; fi
    if [ "${WDDM_BRIDGE_TEST_NATIVE_IMAGE_SMALL:-0}" = 1 ]; then image_define="$image_define -DWDDM_BRIDGE_NATIVE_IMAGE_WIDTH=1281 -DWDDM_BRIDGE_NATIVE_IMAGE_HEIGHT=719"; fi
    "${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror $image_define \
        -isystem "$directx/include" -isystem "$directx/include/wsl/stubs" \
        -isystem "$source/include" -isystem "$source/src/gallium/drivers/d3d12" \
        -c "$base/egl_native_image_probe.cpp" -o "$output/egl-native-image-probe.o"
    "${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror $exit_define $image_define \
        -isystem "$mesa/include" -c "$base/egl_gl_probe.c" -o "$output/egl-gl-probe.o"
    "${CXX:-c++}" -std=c++17 -isystem "$directx/include" -isystem "$directx/include/wsl/stubs" \
        "$output/egl-gl-probe.o" "$output/egl-native-image-probe.o" "$directx/src/dxguids.cpp" \
        -L"$mesa/lib" -Wl,-rpath-link,"$mesa/lib" -l:libEGL.so.1 -ldl -o "$output/egl-gl-probe"
elif [ "${WDDM_BRIDGE_TEST_NATIVE_SYNC_FILES:-0}" = 1 ] || [ "${WDDM_BRIDGE_TEST_NATIVE_SYNC_LINEAGE:-0}" = 1 ] || [ "${WDDM_BRIDGE_TEST_EGL_NATIVE_SYNC:-0}" = 1 ]; then
    if [ "${WDDM_BRIDGE_TEST_RESOURCE_FDS:-0}" = 1 ] || [ "${WDDM_BRIDGE_TEST_MESA_FENCE_FDS:-0}" = 1 ] || [ "${WDDM_BRIDGE_TEST_SYNC_FDS:-0}" = 1 ]; then
        echo 'Select one explicit FD diagnostic at a time' >&2; exit 2
    fi
    source=${WDDM_BRIDGE_MESA_SOURCE:?Supply pinned Mesa source}
    directx=${WDDM_BRIDGE_DIRECTX_HEADERS:?Supply DirectX-Headers source}
    dxg=${WDDM_BRIDGE_DXG_HEADERS:?Supply libdxg source}
    sync_source=native_sync_file_probe.cpp
    sync_define=
    if [ "${WDDM_BRIDGE_TEST_NATIVE_SYNC_LINEAGE:-0}" = 1 ]; then
        sync_source=native_sync_lineage_probe.cpp
    fi
    if [ "${WDDM_BRIDGE_TEST_EGL_NATIVE_SYNC:-0}" = 1 ]; then
        sync_source=egl_native_sync_probe.cpp
        sync_define=-DWDDM_BRIDGE_EGL_NATIVE_SYNC_TEST=1
        if [ "${WDDM_BRIDGE_TEST_EGL_NATIVE_SYNC_DISABLED:-0}" = 1 ]; then
            sync_define="$sync_define -DWDDM_BRIDGE_EGL_NATIVE_SYNC_DISABLED=1"
        fi
    fi
    "${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror $sync_define \
        -isystem "$directx/include" -isystem "$directx/include/wsl/stubs" -isystem "$dxg/include" \
        -isystem "$source/include" -isystem "$source/src/gallium/drivers/d3d12" \
        -c "$base/$sync_source" -o "$output/native-sync-file-probe.o"
    "${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror $exit_define $sync_define -DWDDM_BRIDGE_NATIVE_SYNC_FILE_TEST=1 \
        -isystem "$mesa/include" -c "$base/egl_gl_probe.c" -o "$output/egl-gl-probe.o"
    "${CXX:-c++}" -std=c++17 -isystem "$directx/include" -isystem "$directx/include/wsl/stubs" \
        "$output/egl-gl-probe.o" "$output/native-sync-file-probe.o" "$directx/src/dxguids.cpp" \
        -L"$mesa/lib" -Wl,-rpath-link,"$mesa/lib" -l:libEGL.so.1 -ldl -o "$output/egl-gl-probe"
elif [ "${WDDM_BRIDGE_TEST_RESOURCE_FDS:-0}" = 1 ]; then
    if [ "${WDDM_BRIDGE_TEST_MESA_FENCE_FDS:-0}" = 1 ] || [ "${WDDM_BRIDGE_TEST_SYNC_FDS:-0}" = 1 ]; then
        echo 'Select one explicit FD diagnostic at a time' >&2
        exit 2
    fi
    source=${WDDM_BRIDGE_MESA_SOURCE:?Supply the pinned Mesa source}
    directx=${WDDM_BRIDGE_DIRECTX_HEADERS:?Supply DirectX-Headers source}
    resource_define=
    if [ "${WDDM_BRIDGE_TEST_RESOURCE_NATIVE_BUFFERS:-0}" = 1 ]; then
        resource_define=-DWDDM_BRIDGE_RESOURCE_DMABUF_TEST=1
        if [ "${WDDM_BRIDGE_TEST_RESOURCE_NATIVE_BUFFERS_DISABLED:-0}" = 1 ]; then
            resource_define="$resource_define -DWDDM_BRIDGE_RESOURCE_DMABUF_DISABLED=1"
        fi
    fi
    if [ "${WDDM_BRIDGE_TEST_RESOURCE_BAD_COLOR:-0}" = 1 ]; then
        resource_define="$resource_define -DWDDM_BRIDGE_RESOURCE_FD_BAD_COLOR=1"
    fi
    if [ "${WDDM_BRIDGE_TEST_RESOURCE_FULL_READBACK:-0}" = 1 ]; then
        resource_define="$resource_define -DWDDM_BRIDGE_RESOURCE_FD_FULL_READBACK=1"
    fi
    if [ "${WDDM_BRIDGE_TEST_RESOURCE_RECIPIENT:-0}" = 1 ]; then
        resource_define="$resource_define -DWDDM_BRIDGE_RESOURCE_FD_RECIPIENT=1"
    fi
    "${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror $resource_define \
        -isystem "$directx/include" -isystem "$directx/include/wsl/stubs" \
        -isystem "$source/include" -isystem "$source/src/gallium/drivers/d3d12" \
        -c "$base/mesa_resource_fd_probe.cpp" -o "$output/mesa-resource-fd-probe.o"
    "${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror $exit_define $resource_define -DWDDM_BRIDGE_MESA_RESOURCE_FD_TEST=1 \
        -isystem "$mesa/include" -c "$base/egl_gl_probe.c" -o "$output/egl-gl-probe.o"
    "${CXX:-c++}" -std=c++17 -isystem "$directx/include" -isystem "$directx/include/wsl/stubs" \
        "$output/egl-gl-probe.o" "$output/mesa-resource-fd-probe.o" "$directx/src/dxguids.cpp" \
        -L"$mesa/lib" -Wl,-rpath-link,"$mesa/lib" -l:libEGL.so.1 -ldl -o "$output/egl-gl-probe"
elif [ "${WDDM_BRIDGE_TEST_MESA_FENCE_FDS:-0}" = 1 ]; then
    if [ "${WDDM_BRIDGE_TEST_SYNC_FDS:-0}" = 1 ]; then
        echo 'Select one explicit fence diagnostic at a time' >&2
        exit 2
    fi
    source=${WDDM_BRIDGE_MESA_SOURCE:?Supply the pinned, patched Mesa source}
    directx=${WDDM_BRIDGE_DIRECTX_HEADERS:?Supply DirectX-Headers source}
    "${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
        -isystem "$directx/include" -isystem "$directx/include/wsl/stubs" \
        -isystem "$source/include" -isystem "$source/src/gallium/drivers/d3d12" \
        -c "$base/mesa_fence_fd_probe.cpp" -o "$output/mesa-fence-fd-probe.o"
    "${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror $exit_define -DWDDM_BRIDGE_MESA_FENCE_FD_TEST=1 \
        -isystem "$mesa/include" -c "$base/egl_gl_probe.c" -o "$output/egl-gl-probe.o"
    "${CXX:-c++}" -std=c++17 -isystem "$directx/include" -isystem "$directx/include/wsl/stubs" \
        "$output/egl-gl-probe.o" "$output/mesa-fence-fd-probe.o" "$directx/src/dxguids.cpp" \
        -L"$mesa/lib" -Wl,-rpath-link,"$mesa/lib" -l:libEGL.so.1 -ldl -o "$output/egl-gl-probe"
elif [ "${WDDM_BRIDGE_TEST_SYNC_FDS:-0}" = 1 ]; then
    dxg=${WDDM_BRIDGE_DXG_HEADERS:?Supply libdxg source for shared-fence acceptance}
    directx=${WDDM_BRIDGE_DIRECTX_HEADERS:?Supply DirectX-Headers source}
    "${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
        -isystem "$dxg/include" -isystem "$directx/include" -isystem "$directx/include/wsl/stubs" \
        -c "$base/sync_fd_probe.cpp" -o "$output/sync-fd-probe.o"
    "${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror $exit_define -DWDDM_BRIDGE_SYNC_FD_TEST=1 -isystem "$mesa/include" \
        -c "$base/egl_gl_probe.c" -o "$output/egl-gl-probe.o"
    "${CXX:-c++}" "$output/egl-gl-probe.o" "$output/sync-fd-probe.o" \
        -L"$mesa/lib" -Wl,-rpath-link,"$mesa/lib" -l:libEGL.so.1 -ldl -o "$output/egl-gl-probe"
else
    "${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror $exit_define -isystem "$mesa/include" \
        "$base/egl_gl_probe.c" -L"$mesa/lib" -Wl,-rpath-link,"$mesa/lib" -l:libEGL.so.1 \
        -ldl -o "$output/egl-gl-probe"
fi
if [ "${WDDM_BRIDGE_TEST_BROKER_PEER:-0}" = 1 ]; then
    "${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -shared -fPIC \
        "$base/guest_broker_finalizer_probe.cpp" -o "$output/broker-finalizer-probe.so"
    "${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pthread \
        "$base/guest_broker_peer_test.cpp" -ldl -o "$output/broker-peer-contract"
fi
if [ "${WDDM_BRIDGE_TEST_BROKER_ROUTE:-0}" = 1 ]; then
    # Resource diagnostics use a 4K native bridge/shim pair, including the
    # explicitly bounded 64 MiB single CPU-mapping profile. Keep the broker's
    # receipt validation on that same profile; the default stays at 4 MiB.
    broker_define=
    if [ "${WDDM_BRIDGE_TEST_RESOURCE_FDS:-0}" = 1 ] || [ "${WDDM_BRIDGE_TEST_NATIVE_IMAGES:-0}" = 1 ] || [ "${WDDM_BRIDGE_TEST_GBM:-0}" = 1 ]; then
        broker_define=-DWDDM_BRIDGE_4K_LAB=1
    fi
    "${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pthread $broker_define \
        "$base/guest_graphics_broker.cpp" -o "$output/guest-graphics-broker"
fi
"${CC:-cc}" -static -std=c11 -O2 -Wall -Wextra -Werror \
    "$base/runtime_guest_init.c" -o "$output/runtime-init"
