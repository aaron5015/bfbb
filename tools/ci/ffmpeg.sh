#!/bin/sh
# Builds the FFmpeg the Linux and macOS packages link: static, position
# independent and with no external libraries, so the executable carries it and
# a player installs nothing. Only the five libraries iFMVDecoderFFmpeg.cpp and
# iSoundtrack.cpp use, and only the decoding side of them.
#
#   tools/ci/ffmpeg.sh <prefix>
#
# Pass <prefix> to CMake as CMAKE_PREFIX_PATH. Windows uses vcpkg's instead.
set -eu

version=n9.0.2
prefix=$1

src=$(mktemp -d)
git clone --quiet --depth 1 --branch "$version" https://github.com/FFmpeg/FFmpeg.git "$src"
cd "$src"

# --disable-autodetect keeps out every library configure would otherwise pick
# up from the build machine (zlib, iconv, VAAPI, VideoToolbox...), none of which
# the release can assume the player has. --disable-x86asm drops the nasm
# dependency; the movies are 640x480 and decode fast enough without it.
./configure --prefix="$prefix" \
  --enable-static --disable-shared --enable-pic \
  --disable-autodetect --disable-x86asm \
  --disable-programs --disable-doc --disable-debug \
  --disable-avdevice --disable-avfilter --disable-network \
  --disable-encoders --disable-muxers

make -j"$(getconf _NPROCESSORS_ONLN)"
make install
