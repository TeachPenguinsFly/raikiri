# deploy

Scripts and units for one Linux host that runs the gateways, admind and the terminal behind Caddy.

- `build-linux.sh` builds x86_64 binaries in Docker.
- `deploy.sh user@host` syncs the source, the built terminal and the units to the host, then restarts them.
- `raikiri-gatewayd@.service` runs one gateway per account; `raikiri-admind.service` runs admind on loopback.
- `raikiri-apid.service` runs the client API on loopback, on a host that has `/etc/raikiri/client-keys.json`.
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

## Client API

`apid` is the only process meant to take traffic from clients. It speaks plain HTTP on loopback and refuses any other
address without `--plaintext-ok`, so TLS has to be in front of it. A separate site in Caddy does that; Caddy passes the
path and query through unchanged, and they are part of what a client signs:

    api.example.com {
        reverse_proxy 127.0.0.1:8900
    }

Caddy upgrades WebSockets on its own. Keep `--trust-proxy 127.0.0.1` so a key's IP allowlist sees the real caller.

To issue a key:

1. `apid --new-secret` prints 32 random bytes as hex. Store it where the host can read it and no one else can, such as
   `/etc/raikiri/client-secrets/acme-1`, mode 0400, owner `raikiri`, or a secret manager behind an `exec:` reference.
2. Add the key to `/etc/raikiri/client-keys.json`, using `config/client-keys.example.json` as the shape. The file holds a
   reference to the secret, never the secret. `accounts` and `symbols` have to be listed; `"*"` means all.
3. `systemctl reload raikiri-apid`. A file that does not load leaves the keys in force and logs why.
4. Give the client the key id, the secret over a channel you trust, and the URL.

To rotate, add a second key for the same `client_id`, let the client move over, and remove the first. To revoke, remove
the key and reload: calls stop at once, and an open stream closes at its next heartbeat.

Each client trades in one book, `client:<client_id>`, which apid opens on its first order through admind's `/v1/books`,
so admind needs its book registry. `/v1/risk` stays operator-only: apid has no route to it, and limits for a client's
book are set there by the owner.

Its state is in `--state-dir`. `ledger.jsonl` says which client owns which order and holds each client's fills and
idempotency results, so it has to be backed up with the journals and cannot be rebuilt from anything else.
It is not rotated or compacted yet. Every mutating call and refusal goes to the hash-chained audit log in
`/var/lib/raikiri/audit/apid`, where `/v1/audit` and `auditcheck` read it alongside the other processes' streams.
