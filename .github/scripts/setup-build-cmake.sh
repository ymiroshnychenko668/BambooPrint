#!/usr/bin/env bash
# Use the same CMake release for native dependency and application builds.
set -euo pipefail
version=3.31.0
case "$RUNNER_OS" in
  macOS)
    platform=macos-universal
    checksum=50d5b9f370e71c8eee87c123b7fb9272caf2bf2b372ea7c9423f10f1cd52813b
    bin=CMake.app/Contents/bin
    ;;
  Linux)
    platform=linux-x86_64
    checksum=0fcb338b4515044f9ac77543550ac92c314c58f6f95aafcac5cd36aa75db6924
    bin=bin
    ;;
  *) echo "Unsupported CMake package for $RUNNER_OS" >&2; exit 1 ;;
esac
package="cmake-$version-$platform"
cd "$RUNNER_TEMP"
curl --fail --location --silent --show-error --retry 3 \
  --output "$package.tar.gz" "https://github.com/Kitware/CMake/releases/download/v$version/$package.tar.gz"
printf '%s  %s\n' "$checksum" "$package.tar.gz" | shasum -a 256 -c -
tar -xzf "$package.tar.gz"
"$RUNNER_TEMP/$package/$bin/cmake" --version
printf '%s\n' "$RUNNER_TEMP/$package/$bin" >> "$GITHUB_PATH"
