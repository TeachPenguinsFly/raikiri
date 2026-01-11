#!/usr/bin/env bash
# Build here, ship there, restart. One command.
#
#   tools/deploy.sh raikiri@1.2.3.4
#   HOST=raikiri@1.2.3.4 tools/deploy.sh
#
# NOT YET RUN AGAINST A REAL HOST. This repository has never been built on
# Linux — its CI targets ubuntu-24.04 and has never executed, because nothing
# has ever been pushed. Step one of any first deployment is proving the build,
# not running this.
#
# WHAT IT DELIBERATELY DOES NOT DO: cross-compile. The binary is built ON the
# target, because the venue adapters are the part most likely to differ between
# a macOS arm64 desk and a Linux arm64 server, and finding that out at link time
# on the box is better than finding it out at run time.
set -euo pipefail
cd "$(dirname "$0")/.."

HOST="${1:-${HOST:?usage: tools/deploy.sh user@host}}"

# SOURCE AND INSTALL ARE TWO PLACES, and conflating them is why this script
# failed on its first real run: /opt/raikiri is root-owned, so rsyncing the
# tree into it is denied before anything is built — and the deploy then
# "succeeded" against the binary that was already there. Build where the
# deploy account can write, install where systemd looks. SRC_DIR is relative
# to that account's home: rsync resolves a relative destination there and so
# does the remote shell.
SRC_DIR="${SRC_DIR:-raikiri}"
REMOTE_ROOT="${REMOTE_ROOT:-/opt/raikiri}"
PRESET="${PRESET:-rel}"

# A DIRECTORY OF ALREADY-BUILT LINUX BINARIES, which skips the remote compile
# entirely. tools/build-linux.sh produces one in build/linux in seconds; the
# same build on this two-vCPU server ran at one translation unit a minute
# against the live gateways, which is seven hours and a hole in the recording.
#
#   BINARIES=build/linux tools/deploy.sh raikiri-tokyo
#
# Unset keeps the remote build, which is the only path for anyone without
# Docker and the reason it is still here.
BINARIES="${BINARIES:-}"

echo "==> the terminal, as static files"
# `npm run build` is a COMPILER: it writes dist/ and exits. `npm run dev` is a
# server that transpiles on demand and proxies /v1 — neither of which belongs on
# a box that is meant to stay up. deploy/Caddyfile takes over the proxying.
( cd frontend && npm ci --no-audit --no-fund && npm run build )

echo "==> source and units to $HOST"
# raikiri-data/ IS NOT SOURCE. It is recorded market data kept for tests, and
# shipping ~100 MB of it to a 2-vCPU box on every deploy buys nothing there.
rsync -a --delete \
    --exclude build/ --exclude .git/ --exclude frontend/node_modules/ \
    --exclude raikiri-data/ \
    ./ "$HOST:$SRC_DIR/"
rsync -a frontend/dist/ "$HOST:$SRC_DIR/web-dist/"
if [[ -n "$BINARIES" ]]; then
    rsync -a --delete "$BINARIES/" "$HOST:$SRC_DIR/prebuilt/"
fi

if [[ -n "$BINARIES" ]]; then
    # REFUSED BEFORE ANY ssh, because the failure otherwise surfaces at exec —
    # after the units have already been restarted onto a binary the box cannot
    # run, which is an outage rather than an error message.
    for app in gatewayd admind replay persisterd; do
        [[ -x "$BINARIES/$app" ]] || continue
        if ! file -b "$BINARIES/$app" | grep -q "ELF 64-bit LSB.*x86-64"; then
            echo "deploy: $BINARIES/$app is not a Linux x86-64 binary" >&2
            exit 1
        fi
    done
fi

echo "==> build on the target, install, restart"
# shellcheck disable=SC2087
ssh "$HOST" bash -s <<REMOTE
set -euo pipefail
cd "\$HOME/$SRC_DIR"
if [[ -z "$BINARIES" ]]; then
    cmake --preset $PRESET
    cmake --build build/$PRESET --target gatewayd admind replay persisterd -j\$(nproc)
fi

sudo install -d -o raikiri -g raikiri "$REMOTE_ROOT/bin" "$REMOTE_ROOT/share" "$REMOTE_ROOT/web" /var/lib/raikiri
sudo rsync -a --delete web-dist/ "$REMOTE_ROOT/web/"
for app in gatewayd admind replay persisterd; do
    if [[ -n "$BINARIES" ]]; then
        src="prebuilt/\$app"
    else
        src="build/$PRESET/apps/\$app/\$app"
    fi
    # A TARGET THAT WAS NOT ASKED FOR IS NOT A FAILURE, but a missing one that
    # WAS asked for must not leave the box running yesterday's binary in
    # silence.
    if [[ -x "\$src" ]]; then
        sudo install -m 0755 "\$src" "$REMOTE_ROOT/bin/"
    else
        echo "deploy: no \$app to install, leaving the one already there" >&2
    fi
done
sudo install -m 0755 deploy/gateway-run "$REMOTE_ROOT/bin/"
sudo install -m 0644 tools/venue_args.sh "$REMOTE_ROOT/share/"
sudo install -m 0644 deploy/raikiri-gatewayd@.service deploy/raikiri-admind.service /etc/systemd/system/
sudo systemctl daemon-reload

# The venue set lives in /etc/raikiri/raikiri.env so a restart does not need a
# deploy. Read here to know which units to enable.
set -a; . /etc/raikiri/raikiri.env; set +a
for account in \$RAIKIRI_VENUES \$RAIKIRI_SPOT; do
    sudo systemctl enable --now "raikiri-gatewayd@\$account"
    sudo systemctl restart "raikiri-gatewayd@\$account"
done
sudo systemctl enable --now raikiri-admind
sudo systemctl restart raikiri-admind
REMOTE

echo "==> up. check: ssh $HOST 'systemctl list-units raikiri-*'"
