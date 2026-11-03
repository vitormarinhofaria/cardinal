#!/usr/bin/env bash
# Builds (incrementally) and runs Cardinal. Extra arguments go to the app.
# Usage: ./run.sh [app args...]
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

[[ -f build/build.ninja ]] || ./setup.sh --no-deps
cmake --build build --target cardinal_gtk
exec ./build/ui-gtk/cardinal_gtk "$@"
