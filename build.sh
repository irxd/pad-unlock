#!/bin/bash
# Build helper for WSL: ./build.sh  (from Windows: wsl -d Ubuntu -- bash build.sh)
set -e
export VITASDK="${VITASDK:-/usr/local/vitasdk}"
export PATH="$VITASDK/bin:$PATH"
cd "$(dirname "$0")"
cmake -S . -B build >/dev/null
cmake --build build
