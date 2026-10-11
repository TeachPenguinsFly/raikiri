# Releases

raikiri follows [semantic versioning](https://semver.org). Until 1.0 a minor release may change the API or a
shared-memory layout; each such change is listed under its release. The version lives in [`VERSION`](VERSION) and
every binary prints it with `--version`.

## 0.1.0-beta.1 (2026-10-11)

The first public beta.

**Market data**
- 43 venues, CEX and DEX, across spot, perpetuals, dated futures and options, one gateway process per venue and
  market.
- Quotes, depth, trades, funding and option greeks as published by each venue, normalised to fixed-point ticks and
  lots, with the venue's clock and the kernel receive time on every record.
- Per-venue rate limits from `config/venues.yaml`, websocket compression where the venue supports it.

**Order entry**
- 23 venues: REST everywhere, websocket order entry on binance, bybit, okx, bitget, gate, kucoin, hyperliquid and
  lighter.
- Signers for HMAC venues, EVM wallets (EIP-712 and agent wallets) and lighter's native Schnorr key.
- Private streams, reconciliation pulls and funding ledgers where the venue offers them.
- A risk gate on every order: notional, price band and open-order limits per strategy.

**Runtime**
- Shared-memory transport: seqlock tables, broadcast rings and SPSC command rings; cross-process p50 167 ns.
- `strategyd` kernel with one book across all attached gateways, `joe` parent orders (TWAP, VWAP, smart routing).
- Append-only SBE journals copied into PostgreSQL by `persisterd`; `refd` reference mids.
- `admind` HTTP and WebSocket API described in `schema/admin-api.yaml`, and the web terminal.

**Known limits**
- Not every order path has run against a real account; the caps table does not yet record which have.
- `grvt` and `cryptocom` are market data only; daemons do not pin threads to cores.
- Shared-memory ABI 16; a gateway and a kernel on different ABIs refuse to attach.
