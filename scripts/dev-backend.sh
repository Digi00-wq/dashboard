#!/usr/bin/env bash
# Dev loop: build the backend, run it, and rebuild + restart whenever
# backend/src, backend/CMakeLists.txt or proto/ change. No extra tools needed.
#
#   ./scripts/dev-backend.sh          # uses .env if present
#   STATIC_DIR=frontend/dist ./scripts/dev-backend.sh
set -uo pipefail
cd "$(dirname "$0")/.."

if [[ -f .env ]]; then set -a; source .env; set +a; fi

BUILD=backend/build
STAMP=$BUILD/.dev-stamp
PID=""

stop() { [[ -n "$PID" ]] && kill "$PID" 2>/dev/null && wait "$PID" 2>/dev/null; PID=""; }
trap 'stop; exit 0' INT TERM

build_and_run() {
  stop
  echo -e "\n\033[1;34m[dev] building…\033[0m"
  if { [[ -f $BUILD/CMakeCache.txt ]] || cmake -S backend -B $BUILD -DCMAKE_BUILD_TYPE=Debug; } \
     && cmake --build $BUILD -j"$(nproc)"; then
    echo -e "\033[1;32m[dev] build ok, starting backend\033[0m"
    ./$BUILD/dashboard_backend &
    PID=$!
  else
    echo -e "\033[1;31m[dev] build failed – waiting for changes\033[0m"
  fi
}

mkdir -p $BUILD
touch "$STAMP"
build_and_run
while true; do
  sleep 1
  if [[ -n $(find backend/src backend/CMakeLists.txt proto -newer "$STAMP" -type f 2>/dev/null | head -1) ]]; then
    touch "$STAMP"
    sleep 0.3 # let editors finish writing
    build_and_run
  fi
done
