#!/usr/bin/env bash
# Build psp2shell_m's control server for the PC (fake Vita APIs, ASan/UBSan) and start it.
#   tests/host/run.sh <root dir> [port]     # root dir gets ux0/, ur0/ ... subdirectories
# Then point a client at it, e.g. VITA_HOST=127.0.0.1 PSP2SHELL_PORT=<port-3> tools/vita ls ux0:/

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${HERE}/../../psp2shell_m"
ROOT_DIR="${1:?usage: run.sh <root dir> [port]}"
PORT="${2:-39436}"
OUT="${HERE}/build"

mkdir -p "${OUT}" "${ROOT_DIR}/ux0" "${ROOT_DIR}/ur0"
gcc -Wall -Wno-unused-function -g -fsanitize=address,undefined \
    -I"${HERE}/fake" -I"${SRC}/include" -I"${HERE}/../../common" \
    -o "${OUT}/ctlhost" "${HERE}/fake_sce.c" "${SRC}/source/ctl.c" "${SRC}/source/md5.c" -lpthread
exec "${OUT}/ctlhost" "${ROOT_DIR}" "${PORT}"
