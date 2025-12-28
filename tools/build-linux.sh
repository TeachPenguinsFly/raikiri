#!/usr/bin/env bash
# The Linux binaries, built here rather than on the server.
#
#   tools/build-linux.sh                  # gatewayd admind replay persisterd
#   TARGETS=gatewayd tools/build-linux.sh
#
# Output lands in build/linux/. Deploy it with:
#
#   BINARIES=build/linux tools/deploy.sh raikiri-tokyo
#
# THE BUILD TREE LIVES IN A DOCKER VOLUME, not in build/, because build/rel on
# a macOS desk is a macOS build and two toolchains writing one directory is a
# cache that answers for neither. The volume also makes the build incremental
# across runs, which is the difference between forty minutes and forty seconds.
set -euo pipefail
cd "$(dirname "$0")/.."

TARGETS="${TARGETS:-gatewayd admind replay persisterd}"
IMAGE="${IMAGE:-raikiri-linux-builder}"
VOLUME="${VOLUME:-raikiri-linux-build}"
CONAN_VOLUME="${CONAN_VOLUME:-raikiri-linux-conan}"
OUT="${OUT:-build/linux}"

echo "==> builder image"
docker build --platform linux/amd64 -t "$IMAGE" -f tools/linux-builder.Dockerfile tools/

echo "==> deps and build (x86_64, RelWithDebInfo)"
docker run --rm --platform linux/amd64 \
    -v "$PWD:/src:ro" \
    -v "$VOLUME:/build" \
    -v "$CONAN_VOLUME:/root/.conan2" \
    "$IMAGE" bash -euc "
        # --build=missing only rebuilds what no binary package covers, so this
        # is slow once and then nothing.
        # user_presets off: conan otherwise writes CMakeUserPresets.json into
        # the SOURCE directory, which is mounted read-only here on purpose —
        # the host copy points at macOS build trees and a container rewriting
        # it would break the desk build the next time someone ran a preset.
        # ONCE THE CACHE IS WARM THIS NEEDS NO NETWORK, and it should not fail
        # when the network is merely unreachable: conan contacts the remote to
        # check for binaries it already holds, and a DNS blip inside the
        # container then kills a build that had everything it needed. The
        # fallback resolves from the cache alone and still fails loudly if a
        # package genuinely is not there.
        deps() {
            conan install /src --output-folder=/build/deps/relwithdebinfo \
                --build=missing -pr:a /src/profiles/linux-gcc14 \
                -s build_type=RelWithDebInfo \
                -c tools.cmake.cmaketoolchain:user_presets=\"\" \"\$@\"
        }
        deps || deps --no-remote
        cmake -S /src -B /build/rel -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE=/build/deps/relwithdebinfo/conan_toolchain.cmake \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo
        cmake --build /build/rel --target $TARGETS -j\$(nproc)
    "

echo "==> out"
mkdir -p "$OUT"
docker run --rm --platform linux/amd64 -v "$VOLUME:/build" -v "$PWD/$OUT:/out" \
    "$IMAGE" bash -euc '
        for app in gatewayd admind replay persisterd; do
            src=/build/rel/apps/$app/$app
            # `|| true` because a target that was not asked for is not an
            # error, and under `set -e` the last absent one otherwise returns
            # non-zero and fails a build that had already succeeded.
            [ -x "$src" ] && cp "$src" /out/ && echo "  $app" || true
        done
    '
ls -lh "$OUT"
