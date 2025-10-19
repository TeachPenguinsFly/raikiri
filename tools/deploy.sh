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
REMOTE_ROOT="${REMOTE_ROOT:-/opt/raikiri}"
PRESET="${PRESET:-rel}"

echo "==> the terminal, as static files"
# `npm run build` is a COMPILER: it writes dist/ and exits. `npm run dev` is a
# server that transpiles on demand and proxies /v1 — neither of which belongs on
# a box that is meant to stay up. deploy/Caddyfile takes over the proxying.
( cd frontend && npm ci --no-audit --no-fund && npm run build )

echo "==> source and units to $HOST"
rsync -a --delete \
    --exclude build/ --exclude .git/ --exclude frontend/node_modules/ \
    ./ "$HOST:$REMOTE_ROOT/src/"
rsync -a frontend/dist/ "$HOST:$REMOTE_ROOT/web/"

echo "==> build on the target, install, restart"
# shellcheck disable=SC2087
ssh "$HOST" bash -s <<REMOTE
set -euo pipefail
cd "$REMOTE_ROOT/src"
cmake --preset $PRESET
cmake --build build/$PRESET --target gatewayd admind replay persisterd -j\$(nproc)

sudo install -d -o raikiri -g raikiri "$REMOTE_ROOT/bin" "$REMOTE_ROOT/share" /var/lib/raikiri
sudo install -m 0755 build/$PRESET/apps/gatewayd/gatewayd "$REMOTE_ROOT/bin/"
sudo install -m 0755 build/$PRESET/apps/admind/admind "$REMOTE_ROOT/bin/"
sudo install -m 0755 build/$PRESET/apps/replay/replay "$REMOTE_ROOT/bin/"
sudo install -m 0755 build/$PRESET/apps/persisterd/persisterd "$REMOTE_ROOT/bin/"
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
