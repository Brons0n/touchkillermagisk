#!/usr/bin/env bash
# Cross-compile touchkillerd for arm64-v8a + armeabi-v7a and package the
# Magisk module zip.
#
#   NDK=/path/to/android-ndk-r27c ./build.sh
#
# Binaries are linked -static so the daemon has no dependency on the ROM's
# linker or libc version (matters on Android 9 .. 16 and on custom ROMs).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/src/touchkillerd.c"
MODULE="$ROOT/module"
OUT="$ROOT/out"

NDK="${NDK:-${ANDROID_NDK_HOME:-$HOME/ndk-dl/android-ndk-r27c}}"
API="${API:-21}"   # min API 21; module targets Android 9 (28) and up

case "$(uname -s)" in
  Darwin) HOST_TAG=darwin-x86_64 ;;
  Linux)  HOST_TAG=linux-x86_64 ;;
  *)      echo "unsupported host $(uname -s)" >&2; exit 1 ;;
esac

TC="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin"
[ -x "$TC/clang" ] || { echo "NDK toolchain not found at $TC" >&2; exit 1; }

CFLAGS="-Os -Wall -Wextra -std=c11 -fno-strict-aliasing -static \
        -ffunction-sections -fdata-sections -Wl,--gc-sections -s"

build() {
  local abi="$1" triple="$2"
  echo ">>> $abi ($triple$API)"
  mkdir -p "$MODULE/bin/$abi"
  # shellcheck disable=SC2086
  "$TC/clang" --target="$triple$API" $CFLAGS -o "$MODULE/bin/$abi/touchkillerd" "$SRC"
  ls -l "$MODULE/bin/$abi/touchkillerd"
  file "$MODULE/bin/$abi/touchkillerd" 2>/dev/null || true
}

build arm64-v8a   aarch64-linux-android
build armeabi-v7a armv7a-linux-androideabi

# ---- package -------------------------------------------------------------
VER="$(sed -n 's/^version=//p' "$MODULE/module.prop")"
mkdir -p "$OUT"
ZIP="$OUT/touchkiller-$VER.zip"
rm -f "$ZIP"
( cd "$MODULE" && zip -qr9 "$ZIP" . -x '.DS_Store' '*/.DS_Store' )
echo
echo "built $ZIP"
unzip -l "$ZIP"
