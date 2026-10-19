#!/usr/bin/env bash
set -euo pipefail
ferro_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
ferro_tmp="$(mktemp -d -t ferro-tsan-XXXXXXXX)"
trap 'rm -rf -- "$ferro_tmp"' EXIT
cat > "$ferro_tmp/main.cpp" <<'CPP'
#include <QCoreApplication>
int audioTests();
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    return audioTests();
}
CPP
read -r -a ferro_cflags <<< "$(pkg-config --cflags Qt6Core portaudio-2.0)"
read -r -a ferro_libs <<< "$(pkg-config --libs Qt6Core portaudio-2.0)"
"${CXX:-c++}" -std=c++17 -O1 -g -fsanitize=thread -fno-omit-frame-pointer -fPIC -no-pie \
    "${ferro_cflags[@]}" -I"$ferro_root/src" \
    "$ferro_tmp/main.cpp" "$ferro_root/src/audio.cpp" "$ferro_root/tests/audio_tests.cpp" \
    "${ferro_libs[@]}" -o "$ferro_tmp/audio-tsan"
TSAN_OPTIONS=halt_on_error=1 "$ferro_tmp/audio-tsan"
printf 'Audio ThreadSanitizer tests passed.\n'
