#!/usr/bin/env bash
# deploy/deploy.sh raikiri@1.2.3.4
# HOST=raikiri@1.2.3.4 deploy/deploy.sh
set -euo pipefail
cd "$(dirname "$0")/.."

HOST="${1:-${HOST:?usage: deploy/deploy.sh user@host}}"

# built in the deploy account's home, installed into root-owned /opt/raikiri
SRC_DIR="${SRC_DIR:-raikiri}"
REMOTE_ROOT="${REMOTE_ROOT:-/opt/raikiri}"
PRESET="${PRESET:-rel}"

# a directory of prebuilt linux binaries skips the remote build, e.g. BINARIES=build/linux
BINARIES="${BINARIES:-}"

echo "==> the terminal, as static files"
( cd frontend && npm ci --no-audit --no-fund && npm run build )

echo "==> source and units to $HOST"
rsync -a --delete \
    --exclude build/ --exclude .git/ --exclude frontend/node_modules/ \
    --exclude raikiri-data/ --exclude tools/ \
    ./ "$HOST:$SRC_DIR/"
rsync -a frontend/dist/ "$HOST:$SRC_DIR/web-dist/"
if [[ -n "$BINARIES" ]]; then
    rsync -a --delete "$BINARIES/" "$HOST:$SRC_DIR/prebuilt/"
fi

if [[ -n "$BINARIES" ]]; then
    # checked before ssh so a binary the box cannot run never reaches a restart
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
    if [[ -x "\$src" ]]; then
        sudo install -m 0755 "\$src" "$REMOTE_ROOT/bin/"
    else
        echo "deploy: no \$app to install, leaving the one already there" >&2
    fi
done
sudo install -m 0755 deploy/gateway-run "$REMOTE_ROOT/bin/"
sudo install -m 0644 deploy/venue_args.sh "$REMOTE_ROOT/share/"
sudo install -m 0644 deploy/raikiri-gatewayd@.service deploy/raikiri-admind.service /etc/systemd/system/
sudo systemctl daemon-reload

set -a; . /etc/raikiri/raikiri.env; set +a
for account in \$RAIKIRI_VENUES \$RAIKIRI_SPOT; do
    sudo systemctl enable --now "raikiri-gatewayd@\$account"
    sudo systemctl restart "raikiri-gatewayd@\$account"
done
sudo systemctl enable --now raikiri-admind
sudo systemctl restart raikiri-admind
REMOTE

echo "==> up. check: ssh $HOST 'systemctl list-units raikiri-*'"
