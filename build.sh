#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────
#  build.sh – Build DemoMediaPlayer for Windows via Docker
#
#  Any extra arguments are forwarded to "docker build", e.g.:
#    ./build.sh --build-arg MPV_DEV_URL="https://…"
#    ./build.sh --no-cache
# ──────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== Building DemoMediaPlayer for Windows (x86_64) ==="
echo ""

DOCKER_BUILDKIT=1 docker build \
    --target dist \
    --output "type=local,dest=${SCRIPT_DIR}/dist" \
    "$@" \
    "$SCRIPT_DIR"

echo ""
echo "Build complete!  Output:"
ls -lh "${SCRIPT_DIR}/dist/"
echo ""
echo "Copy the dist/ folder to a Windows machine and run mediaplayer.exe"
