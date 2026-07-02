#!/bin/bash -e

# Cross-builds the touchscreen input tools shipped in the base image:
#   - xlibinput_calibrator (upstream, pinned): X11 touchscreen calibration
#     for the libinput driver, used by x-chip-touch-calibrate when present.
#   - x-chip-touch2mouse (src/x-chip-touch2mouse.c): grabs the resistive
#     touch panel and re-emits it as a uinput mouse for mouse-only apps
#     such as PICO-8.
# Needs the armhf cross toolchain plus libx11-dev/libxi-dev/libxrandr-dev
# for armhf (both provided by the build container). Skip explicitly with
# TOUCH_TOOLS_BUILD=0.

HERE=$(cd "$(dirname "$0")/.." && pwd); cd "$HERE"
source ./config.env

[ "${TOUCH_TOOLS_BUILD:-1}" = 1 ] || {
    echo ">> touch tools build disabled"
    exit 0
}

CC=${CROSS_COMPILE}gcc
CXX=${CROSS_COMPILE}g++

command -v "$CXX" >/dev/null || {
    echo "ERROR: $CXX not found; use 'make container-build' or set TOUCH_TOOLS_BUILD=0" >&2
    exit 1
}

SRC="$HERE/build/xlibinput-calibrator-src"
OUT_DIR="$HERE/build/touch-tools"

command -v git >/dev/null || { echo "need git to fetch xlibinput_calibrator source" >&2; exit 1; }

if [ ! -d "$SRC/.git" ]; then
    git clone "$XLIBINPUT_CALIBRATOR_REPO" "$SRC"
fi

if ! git -C "$SRC" cat-file -e "$XLIBINPUT_CALIBRATOR_COMMIT^{commit}" 2>/dev/null; then
    git -C "$SRC" fetch --tags origin "$XLIBINPUT_CALIBRATOR_COMMIT"
fi
git -C "$SRC" checkout -f "$XLIBINPUT_CALIBRATOR_COMMIT"
git -C "$SRC" clean -fdx

# libX11/libXi/libXrandr come from Debian armhf dev packages; the device gets
# the matching runtime libraries from the preseeded Xorg/xinput/xrandr .tcz
# extensions. libstdc++/libgcc are linked statically so the base image needs
# no new runtime extension.
make -C "$SRC/src" xlibinput_calibrator \
    CXX="$CXX" \
    LDFLAGS='-std=c++17 -static-libstdc++ -static-libgcc'

"$CC" -O2 -Wall -Wextra -o "$SRC/x-chip-touch2mouse" "$HERE/src/x-chip-touch2mouse.c"

for bin in "$SRC/src/xlibinput_calibrator" "$SRC/x-chip-touch2mouse"; do
    "${CROSS_COMPILE}readelf" -h "$bin" | grep -q 'Machine:.*ARM' || {
        echo "ERROR: $bin is not an ARM binary" >&2
        exit 1
    }
    "${CROSS_COMPILE}strip" "$bin"
done

install -D -m755 "$SRC/src/xlibinput_calibrator" "$OUT_DIR/xlibinput_calibrator"
install -D -m755 "$SRC/x-chip-touch2mouse" "$OUT_DIR/x-chip-touch2mouse"
echo ">> built touch tools: $OUT_DIR"
