#!/bin/bash
# Builds mplayer-arm — MPlayer 1.5 with MiSTerEmby's patched framebuffer
# driver (mplayer/vo_fbdev.c) — without Docker or root.
#
# The repo already carries a tested mplayer-arm, and releases ship that file;
# this script is only needed to rebuild it (e.g. after changing
# mplayer/vo_fbdev.c).
#
# How it works: Zig (the same toolchain `make arm` uses) is the C compiler,
# targeting glibc 2.31 like the MiSTer's own Linux. FFmpeg's hand-written ARM
# NEON assembly is GNU-assembler syntax that clang's assembler can't read, so
# .S files are preprocessed by Zig and assembled by Debian bullseye's GNU
# `as`. ALSA headers/library come from bullseye's armhf packages. Everything
# is downloaded into a work directory; nothing is installed.
#
# Usage:  tools/build-mplayer.sh [workdir]      (needs zig on PATH, curl, xz, dpkg-deb)
# Output: ./mplayer-arm in the repo root

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${1:-$REPO/.mplayer-build}"
MPLAYER_VER=1.5
DEBIAN=https://deb.debian.org/debian/pool/main
ALSA_VER=1.2.4-1.1
BINUTILS_VER=2.35.2-2

command -v zig >/dev/null || { echo "zig not found on PATH" >&2; exit 1; }
ZIG="$(command -v zig)"

mkdir -p "$WORK/dl" "$WORK/bin" "$WORK/sysroot" "$WORK/binutils"
cd "$WORK/dl"

fetch() { [ -f "$(basename "$1")" ] || curl -sSfLO "$1"; }
fetch "https://mplayerhq.hu/MPlayer/releases/MPlayer-$MPLAYER_VER.tar.xz"
fetch "$DEBIAN/a/alsa-lib/libasound2_${ALSA_VER}_armhf.deb"
fetch "$DEBIAN/a/alsa-lib/libasound2-dev_${ALSA_VER}_armhf.deb"
fetch "$DEBIAN/b/binutils/binutils-arm-linux-gnueabihf_${BINUTILS_VER}_amd64.deb"

dpkg-deb -x "libasound2_${ALSA_VER}_armhf.deb"     "$WORK/sysroot"
dpkg-deb -x "libasound2-dev_${ALSA_VER}_armhf.deb" "$WORK/sysroot"
dpkg-deb -x "binutils-arm-linux-gnueabihf_${BINUTILS_VER}_amd64.deb" "$WORK/binutils"

BU_LIB="$WORK/binutils/usr/lib/x86_64-linux-gnu"
BU_BIN="$WORK/binutils/usr/bin"
S="$WORK/sysroot"

# ── toolchain wrappers ─────────────────────────────────────────────────────
cat > "$WORK/bin/arm-linux-gnueabihf-gcc" <<EOF
#!/bin/sh
exec "$ZIG" cc -target arm-linux-gnueabihf.2.31 -mcpu=cortex_a9 "\$@"
EOF
for t in ar ranlib; do
    printf '#!/bin/sh\nexec "%s" %s "$@"\n' "$ZIG" "$t" > "$WORK/bin/arm-linux-gnueabihf-$t"
done
printf '#!/bin/sh\nLD_LIBRARY_PATH="%s" exec "%s/arm-linux-gnueabihf-nm" "$@"\n' "$BU_LIB" "$BU_BIN" \
    > "$WORK/bin/arm-linux-gnueabihf-nm"
cat > "$WORK/bin/arm-as-wrap" <<EOF
#!/bin/bash
# Preprocess an FFmpeg .S file with zig, assemble it with GNU as.
set -e
out=""; src=""; args=()
while [ \$# -gt 0 ]; do
  case "\$1" in
    -o) out="\$2"; shift ;;
    -c|-MMD|-MP) ;;
    -MF|-MT) shift ;;
    *.S|*.s) src="\$1" ;;
    *) args+=("\$1") ;;
  esac
  shift
done
tmp="\${out%.o}.pp.s"
"$ZIG" cc -target arm-linux-gnueabihf -mcpu=cortex_a9 -E -x assembler-with-cpp "\${args[@]}" "\$src" -o "\$tmp"
LD_LIBRARY_PATH="$BU_LIB" "$BU_BIN/arm-linux-gnueabihf-as" -march=armv7-a -mfpu=neon -mfloat-abi=hard "\$tmp" -o "\$out"
rm -f "\$tmp"
EOF
chmod +x "$WORK/bin/"*
export PATH="$WORK/bin:$PATH"

# ── MPlayer ────────────────────────────────────────────────────────────────
cd "$WORK"
rm -rf "MPlayer-$MPLAYER_VER"
tar xf "dl/MPlayer-$MPLAYER_VER.tar.xz"
cp "$REPO/mplayer/vo_fbdev.c" "MPlayer-$MPLAYER_VER/libvo/vo_fbdev.c"
# The banner in vo_fbdev.c draws text with the app's own 8x8 font.
cp "$REPO/src/font8x8.h"     "MPlayer-$MPLAYER_VER/libvo/font8x8.h"
cd "MPlayer-$MPLAYER_VER"

# Only what MiSTerEmby uses: fbdev video, ALSA audio, network streams. The
# -Wno-error flags cover MPlayer 1.5 code that newer clang rejects by default.
./configure \
    --target=arm-linux-gnueabihf --enable-cross-compile \
    --cc=arm-linux-gnueabihf-gcc --as=arm-linux-gnueabihf-gcc \
    --ar=arm-linux-gnueabihf-ar --nm=arm-linux-gnueabihf-nm \
    --ranlib=arm-linux-gnueabihf-ranlib --host-cc=gcc \
    --extra-cflags="-O2 -mfpu=neon -mfloat-abi=hard -I$S/usr/include -Wno-error=incompatible-function-pointer-types -Wno-error=int-conversion -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types" \
    --extra-ldflags="-L$S/usr/lib/arm-linux-gnueabihf" \
    --disable-mencoder --disable-x11 --disable-xv --disable-xvmc --disable-gl \
    --disable-sdl --disable-gui --disable-png --disable-jpeg --disable-gif \
    --disable-lirc --disable-lircc --disable-joystick --disable-tv --disable-pvr \
    --disable-radio --disable-ossaudio --disable-arts --disable-esd --disable-nas \
    --disable-openal --disable-jack --disable-ladspa --disable-libdv --disable-speex \
    --disable-theora --disable-toolame --disable-twolame --disable-dvdnav \
    --disable-dvdread --enable-fbdev --enable-alsa > "$WORK/configure.log" 2>&1

# configure insists on using the C compiler driver for .S files; point the
# build at the GNU-as wrapper instead.
sed -i "s#^AS      = .*#AS      = $WORK/bin/arm-as-wrap#" config.mak

# MPlayer embeds its whole configure command line in the binary (shown by
# `mplayer -v`), and that line carries this machine's work-directory paths.
# Record a neutral description instead.
sed -i 's#^\#define CONFIGURATION .*#\#define CONFIGURATION "MiSTerEmby build: see tools/build-mplayer.sh"#' config.h

make -j"$(nproc)" > "$WORK/make.log" 2>&1 || { tail -30 "$WORK/make.log"; exit 1; }

cp mplayer "$REPO/mplayer-arm"
LD_LIBRARY_PATH="$BU_LIB" "$BU_BIN/arm-linux-gnueabihf-strip" "$REPO/mplayer-arm"
echo "Built $REPO/mplayer-arm"
