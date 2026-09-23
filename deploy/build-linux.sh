#!/usr/bin/env bash
# The Linux binaries, built here rather than on the server.
#
#   deploy/build-linux.sh                  # gatewayd admind replay persisterd
#   TARGETS=gatewayd deploy/build-linux.sh
#
# Output lands in build/linux/, STRIPPED. Deploy it with:
#
#   BINARIES=build/linux deploy/deploy.sh raikiri-tokyo
#
# THE SYMBOLS ARE NOT THROWN AWAY, they are moved: each binary's debug info is
# split into build/linux-debug/<app>.debug and the shipped file carries a
# gnu-debuglink back to it. gdb finds it given the directory, so a crash on the
# box is still readable — from here, with the matching .debug beside the core.
#
# WHY IT MATTERS ON A 48 GB DISK: unstripped gatewayd is 402 MB and stripped is
# about 40. Two copies of it lived on the server (/opt and prebuilt/) and the
# deploy pushed the whole 402 MB every time.
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
DEBUG_OUT="${DEBUG_OUT:-build/linux-debug}"
# simdjson picks its on-demand kernel at compile time; haswell needs AVX2, BMI,
# LZCNT and PCLMUL, and x86-64-v3 lacks PCLMUL, so it stays the scalar fallback.
MARCH="${RAIKIRI_MARCH:-haswell}"

echo "==> builder image"
docker build --platform linux/amd64 -t "$IMAGE" -f deploy/linux-builder.Dockerfile deploy/

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
            -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DRAIKIRI_MARCH=$MARCH
        cmake --build /build/rel --target $TARGETS -j${JOBS:-\$(nproc)} -- -k 0
    "

echo "==> out"
mkdir -p "$OUT" "$DEBUG_OUT"
docker run --rm --platform linux/amd64 -v "$VOLUME:/build" -v "$PWD/$OUT:/out" \
    -v "$PWD/$DEBUG_OUT:/debug" \
    -e TARGETS="$TARGETS" "$IMAGE" bash -euc '
        # WHATEVER WAS ASKED FOR, not a list that drifts from TARGETS: a
        # target that built and was silently not extracted looks exactly like
        # a target that failed to build.
        for app in $TARGETS; do
            src=/build/rel/apps/$app/$app
            # `|| true` because a target that was not asked for is not an
            # error, and under `set -e` the last absent one otherwise returns
            # non-zero and fails a build that had already succeeded.
            [ -x "$src" ] || continue
            # SPLIT, THEN STRIP, THEN LINK — in that order. Stripping first
            # would leave nothing to keep, and adding the link before the
            # strip would have the strip remove it again.
            objcopy --only-keep-debug "$src" "/debug/$app.debug"
            objcopy --strip-debug --strip-unneeded "$src" "/out/$app"
            objcopy --add-gnu-debuglink="/debug/$app.debug" "/out/$app"
            echo "  $app"
        done
    '
ls -lh "$OUT"
echo "symbols:"
ls -lh "$DEBUG_OUT"
