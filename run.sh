#!/usr/bin/env bash
set -euo pipefail
ferro_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ ! -x "$ferro_root/build/ferro" ]]; then
  cmake -S "$ferro_root" -B "$ferro_root/build" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$ferro_root/build" -j 4
fi
exec "$ferro_root/build/ferro" "$@"
