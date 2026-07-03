#!/usr/bin/env bash
# One-shot build for the VietKi macOS desktop shell.
set -euo pipefail

cd "$(dirname "$0")"

# --- check OS -------------------------------------------------------------
if [ "$(uname)" != "Darwin" ]; then
  echo "Error: This script is only for macOS (Darwin)."
  exit 1
fi

# --- configure + build -----------------------------------------------------
if [ -d build-mac ]; then
  echo "Removing existing build-mac/..."
  rm -rf build-mac
fi

echo "Configuring macOS Release build..."
cmake -B build-mac -S . -DVIETKI_MACOS=ON -DCMAKE_BUILD_TYPE=Release

echo "Building VietKi..."
cmake --build build-mac --parallel

echo "Codesigning VietKi.app..."
codesign --force --deep --sign - build-mac/VietKi.app

echo
echo "Done: build-mac/VietKi.app"
echo "Run it with:  open build-mac/VietKi.app"
