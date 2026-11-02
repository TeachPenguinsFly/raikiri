# deploy

Scripts and units for one Linux host that runs the gateways, admind and the terminal behind Caddy.

- `build-linux.sh` builds x86_64 binaries in Docker.
- `deploy.sh user@host` syncs the source, the built terminal and the units to the host, then restarts them.
- `raikiri-gatewayd@.service` runs one gateway per account; `raikiri-admind.service` runs admind on loopback.
- `Caddyfile` serves the terminal and proxies `/v1` to admind.
- `raikiri.env.example` and `credentials.env.example` go to `/etc/raikiri/` without the suffix.

Caddy puts basic auth on everything except the WebSocket at `/v1/stream`, because browsers send no
basic auth on an upgrade. The terminal first fetches `/v1/stream/token`, which is behind basic auth,
and admind refuses an upgrade without a valid token. A token is an HMAC over its expiry and a nonce
and lives for 60 seconds. `--stream-secret` names the key as an `env:`, `file:` or `exec:` reference.

Caddy also sets `X-Raikiri-Login` to the basic auth user on every `/v1` request, replacing what a client sent, and
admind books a hand order to `manual:<login>`. A request with no login lands on the shared `manual` book. Admind on
loopback takes the header from anyone there, so only the proxy may be reachable from outside.

`$RAIKIRI_DATA_DIR/books.jsonl` is the book registry, written by admind and strategyd under an flock, so both must
see the same file: set `RAIKIRI_BOOKS_FILE` where their data dirs differ. Back it up with the journals: without it a book id could be handed out twice.

TLS gap: the Caddyfile listens on plain `:80`, so the password and the token cross the network unencrypted until a domain is put in front of it and Caddy adds TLS.
