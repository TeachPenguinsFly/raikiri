#!/usr/bin/env bash
# deploy/build-linux.sh                  # gatewayd admind replay persisterd
# TARGETS=gatewayd deploy/build-linux.sh
# stripped binaries go to build/linux, debug info to build/linux-debug
set -euo pipefail
cd "$(dirname "$0")/.."

TARGETS="${TARGETS:-gatewayd admind replay persisterd}"
IMAGE="${IMAGE:-raikiri-linux-builder}"
VOLUME="${VOLUME:-raikiri-linux-build}"
CONAN_VOLUME="${CONAN_VOLUME:-raikiri-linux-conan}"
OUT="${OUT:-build/linux}"
DEBUG_OUT="${DEBUG_OUT:-build/linux-debug}"
# haswell for simdjson's avx2 kernel; x86-64-v3 lacks pclmul
MARCH="${RAIKIRI_MARCH:-haswell}"

echo "==> builder image"
docker build --platform linux/amd64 -t "$IMAGE" -f deploy/linux-builder.Dockerfile deploy/

echo "==> deps and build (x86_64, RelWithDebInfo)"
docker run --rm --platform linux/amd64 \
    -v "$PWD:/src:ro" \
    -v "$VOLUME:/build" \
    -v "$CONAN_VOLUME:/root/.conan2" \
    "$IMAGE" bash -euc "
        # user_presets off because the source mount is read-only
        # falls back to the cache alone when the remote is unreachable
        deps() {
            conan install /src --output-folder=/build/deps/relwithdebinfo \
                --build=missing -pr:a /src/profiles/linux-gcc14 \
                -s build_type=RelWithDebInfo \
                -c tools.cmake.cmaketoolchain:user_presets=\"\" \"\$@\"
        }
        deps || deps --no-remote
        cmake -S /src -B /build/rel -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE=/build/deps/relwithdebinfo/conan_toolchain.cmake \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DRAIKIRI_MARCH=$MARCH
        cmake --build /build/rel --target $TARGETS -j${JOBS:-\$(nproc)} -- -k 0
    "

echo "==> out"
mkdir -p "$OUT" "$DEBUG_OUT"
docker run --rm --platform linux/amd64 -v "$VOLUME:/build" -v "$PWD/$OUT:/out" \
    -v "$PWD/$DEBUG_OUT:/debug" \
    -e TARGETS="$TARGETS" "$IMAGE" bash -euc '
        for app in $TARGETS; do
            src=/build/rel/apps/$app/$app
            [ -x "$src" ] || continue
            # split, then strip, then add the debuglink
            objcopy --only-keep-debug "$src" "/debug/$app.debug"
            objcopy --strip-debug --strip-unneeded "$src" "/out/$app"
            objcopy --add-gnu-debuglink="/debug/$app.debug" "/out/$app"
            echo "  $app"
        done
    '
ls -lh "$OUT"
echo "symbols:"
ls -lh "$DEBUG_OUT"
