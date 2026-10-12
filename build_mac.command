#!/bin/zsh
set -e
cd "$(dirname "$0")"
echo "=== SCULPT AUTOTONE / Beta 0.1 ==="
if ! command -v xcodebuild >/dev/null; then
  echo "Install Xcode Command Line Tools: xcode-select --install"
  exit 1
fi
if ! command -v cmake >/dev/null; then
  echo "CMake is required. Install with: brew install cmake"
  exit 1
fi
if ! command -v git >/dev/null; then
  echo "Git is required. Install Xcode Command Line Tools."
  exit 1
fi
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
cmake --build build --config Release -j 4
APP="$(find build -path '*Sculpt AutoTone.app' -type d -print -quit)"
if [[ -z "$APP" ]]; then
  echo "Build completed but the app bundle was not found; check build/ directory."
  exit 1
fi
echo "App created at: $APP"
open "$APP"
