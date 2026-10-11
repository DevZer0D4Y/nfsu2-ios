#!/usr/bin/env bash
# The movie decoder's FFmpeg (src/movie_vp6.c) for Apple targets: the same
# minimal LGPL 2.1 build as tools/build_ffmpeg_vp6.sh (libavcodec + libavutil,
# only the vp6 decoder), but as *dynamic* frameworks, so the app links FFmpeg
# the way the LGPL expects and a user can replace it.
#
#   tools/build_ffmpeg_apple.sh macos|sim|device
#
# Installs into $DEPS/<target>/:
#   Frameworks/avcodec.framework, Frameworks/avutil.framework
#       (install names @rpath/<name>.framework/<name>)
#   include/libavcodec, include/libavutil
# then configure the game with -DNFSU2_FFMPEG_DIR=$DEPS/<target>.
#
# Source: FFmpeg 7.1.1 (https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz,
# sha256 733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1)
# unpacked under $DEPS/src/ffmpeg/.
#
# Environment:
#   DEPS          default ~/nfsu2/deps
#   IOS_MIN       iOS deployment target (default 16.0)
#   MACOS_MIN     macOS deployment target (default 13.0)
#   JOBS          parallel jobs (default: hw.ncpu)
set -euo pipefail

TARGET="${1:-}"
DEPS="${DEPS:-$HOME/nfsu2/deps}"
IOS_MIN="${IOS_MIN:-16.0}"
MACOS_MIN="${MACOS_MIN:-13.0}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
SRC="$(ls -d "$DEPS"/src/ffmpeg/ffmpeg-* | head -1)"

case "$TARGET" in
macos)  SDK=macosx;          MINFLAG="-mmacosx-version-min=$MACOS_MIN"; PLAT=MacOSX ;;
sim)    SDK=iphonesimulator; MINFLAG="-mios-simulator-version-min=$IOS_MIN"; PLAT=iPhoneSimulator ;;
device) SDK=iphoneos;        MINFLAG="-miphoneos-version-min=$IOS_MIN"; PLAT=iPhoneOS ;;
*)      echo "usage: $0 macos|sim|device" >&2; exit 2 ;;
esac

PREFIX="$DEPS/$TARGET"
BUILD="$DEPS/build-$TARGET-ffmpeg"
STAGE="$BUILD/stage"
SYSROOT="$(xcrun --sdk "$SDK" --show-sdk-path)"
CC="$(xcrun --sdk "$SDK" -f clang)"

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"
"$SRC/configure" --prefix="$STAGE" \
    --disable-everything --enable-decoder=vp6 \
    --disable-avformat --disable-avdevice --disable-avfilter --disable-swscale \
    --disable-swresample --disable-programs --disable-doc --disable-network \
    --disable-pthreads --disable-debug --disable-autodetect \
    --disable-videotoolbox --disable-audiotoolbox \
    --enable-shared --disable-static --enable-pic \
    --enable-cross-compile --target-os=darwin --arch=aarch64 \
    --cc="$CC" --sysroot="$SYSROOT" \
    --extra-cflags="-arch arm64 $MINFLAG" --extra-ldflags="-arch arm64 $MINFLAG" \
    >configure.log 2>&1 || { echo "configure failed, see $BUILD/configure.log" >&2; exit 1; }
grep -E '^License' configure.log
make -j"$JOBS" >make.log 2>&1 || { echo "build failed, see $BUILD/make.log" >&2; exit 1; }
make install >install.log 2>&1

# Each dylib becomes a framework; avcodec's reference to avutil follows.
mkdir -p "$PREFIX/Frameworks" "$PREFIX/include"
rm -rf "$PREFIX/include/libavcodec" "$PREFIX/include/libavutil"
cp -R "$STAGE/include/libavcodec" "$STAGE/include/libavutil" "$PREFIX/include/"
for lib in avutil avcodec; do
    fw="$PREFIX/Frameworks/$lib.framework"
    real="$(cd "$STAGE/lib" && ls lib$lib.*.dylib | awk 'length > max { max = length; f = $0 } END { print f }')"
    rm -rf "$fw"
    mkdir -p "$fw"
    cp "$STAGE/lib/$real" "$fw/$lib"
    install_name_tool -id "@rpath/$lib.framework/$lib" "$fw/$lib"
    if [ "$lib" = avcodec ]; then
        old="$(otool -L "$fw/$lib" | awk '/libavutil/ { print $1 }')"
        install_name_tool -change "$old" "@rpath/avutil.framework/avutil" "$fw/$lib"
    fi
    version="$(echo "$real" | sed -E 's/^lib[a-z]+\.([0-9.]+)\.dylib$/\1/')"
    cat >"$fw/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>$lib</string>
    <key>CFBundleIdentifier</key><string>org.ffmpeg.$lib</string>
    <key>CFBundleName</key><string>$lib</string>
    <key>CFBundlePackageType</key><string>FMWK</string>
    <key>CFBundleShortVersionString</key><string>$version</string>
    <key>CFBundleVersion</key><string>$version</string>
    <key>CFBundleSupportedPlatforms</key><array><string>$PLAT</string></array>
    <key>MinimumOSVersion</key><string>$IOS_MIN</string>
</dict>
</plist>
EOF
    codesign --force --sign - "$fw/$lib" >/dev/null 2>&1   # re-signed when embedded
done
cp "$SRC/COPYING.LGPLv2.1" "$PREFIX/Frameworks/FFmpeg-COPYING.LGPLv2.1"
rm -rf "$BUILD/stage"
echo "FFmpeg (vp6, LGPL, dynamic): installed into $PREFIX (pass -DNFSU2_FFMPEG_DIR=$PREFIX)"
