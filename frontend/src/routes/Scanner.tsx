import { useEffect, useMemo, useState } from 'react'
import { api, type Market } from '@/api/client'
import { Failed, Loading } from '@/components/Chrome'
import { colorFor } from '@/components/chart'
import { cn, toNumber } from '@/lib/format'
import { useQuery } from '@/lib/query'
import { useFunding, useQuotes } from '@/lib/stream'

/** Debounced, because a search is a keystroke and the catalogue is thousands of
 *  rows: firing on every letter asks the server four questions to answer one. */
function useDebounced(value: string, ms: number): string {
  const [settled, setSettled] = useState(value)
  useEffect(() => {
    const timer = setTimeout(() => setSettled(value), ms)
    return () => clearTimeout(timer)
  }, [value, ms])
  return settled
}

export function Scanner() {
  const [typed, setTyped] = useState('')
  const [chosen, setChosen] = useState<string | null>(null)
  const query = useDebounced(typed.trim(), 200)

  const markets = useQuery(() => api.markets({ q: query, limit: '40' }), [query], undefined)

  const market: Market | null = useMemo(() => {
    if (markets.state !== 'ready') return null
    return markets.data.find((m) => m.symbol === chosen) ?? markets.data[0] ?? null
  }, [markets, chosen])

  // The whole selected market, in one subscription each. Quotes at ten a second
  // for a row of venues — the eye cannot use thirty and eight venues at thirty
  // is bandwidth for nothing. Funding rides its own, at the rate it changes.
  const ids = useMemo(() => (market?.listings ?? []).map((l) => l.instrument_id), [market])
  const quotes = useQuotes(ids, 10)
  const funding = useFunding(ids)

  if (markets.state === 'loading') return <Loading what="the catalogue" />
  if (markets.state === 'failed') return <Failed {...markets} />

  return (
    <div className="flex h-full min-h-0">
      <aside className="flex w-72 shrink-0 flex-col border-r border-line">
        <div className="border-b border-line p-3">
          <input
            value={typed}
            onChange={(e) => {
              setTyped(e.target.value)
              setChosen(null)
            }}
            placeholder="BTC, ETH, SOL…"
            autoFocus
            className={cn(
              'h-9 w-full rounded-lg border border-line bg-canvas/60 px-3 text-sm text-ink',
              'outline-none transition-colors placeholder:text-faint focus:border-accent/70',
            )}
          />
        </div>
        <ul className="min-h-0 flex-1 overflow-auto py-1">
          {markets.data.map((row) => {
            const active = row.symbol === market?.symbol
            return (
              <li key={row.symbol}>
                <button
                  type="button"
                  onClick={() => setChosen(row.symbol)}
                  className={cn(
                    'flex w-full items-center justify-between px-3 py-1.5 text-left text-sm',
                    active ? 'bg-raised text-ink' : 'text-muted hover:bg-raised/50',
                  )}
                >
                  <span className="truncate">{row.symbol}</span>
                  {/* The count IS the reason this view exists: a market four
                      venues quote is worth opening, one venue's is not. */}
                  <span className="tnum ml-2 shrink-0 text-xs text-faint">
                    {row.listings.length}
                  </span>
                </button>
              </li>
            )
          })}
          {markets.data.length === 0 && (
            <li className="px-3 py-6 text-center text-sm text-muted">
              Nothing matches. The catalogue is what the attached gateways loaded.
            </li>
          )}
        </ul>
      </aside>

      <main className="min-h-0 flex-1 overflow-auto">
        {market === null ? (
          <div className="p-8 text-sm text-muted">Pick a market.</div>
        ) : (
          <>
            <header className="flex items-baseline gap-3 border-b border-line px-5 py-3">
              <h1 className="font-mono text-lg tracking-tight text-ink">{market.symbol}</h1>
              <span className="text-sm text-muted">
                {market.listings.length}{' '}
                {market.listings.length === 1 ? 'venue' : 'venues'}
              </span>
            </header>

            <table className="w-full text-sm">
              <thead className="text-left text-[10px] uppercase tracking-[0.12em] text-faint">
                <tr className="border-b border-line">
                  <th className="px-5 py-2 font-normal">Venue</th>
                  <th className="px-3 py-2 font-normal">Symbol</th>
                  <th className="px-3 py-2 text-right font-normal">Bid</th>
                  <th className="px-3 py-2 text-right font-normal">Ask</th>
                  <th className="px-3 py-2 text-right font-normal">Spread</th>
                  <th className="px-5 py-2 text-right font-normal">Funding</th>
                </tr>
              </thead>
              <tbody>
                {market.listings.map((listing) => (
                  <Row
                    key={listing.instrument_id}
                    venue={listing.venue}
                    market={listing.market}
                    symbol={listing.exchange_symbol}
                    quote={quotes.get(listing.instrument_id)}
                    funding={funding.get(listing.instrument_id)}
                  />
                ))}
              </tbody>
            </table>

            <p className="px-5 py-3 text-[11px] leading-relaxed text-faint">
              Quotes and funding are live from the gateway segments. A blank row is a venue whose
              gateway is attached and has not ticked yet — not a venue with no market.
            </p>
          </>
        )}
      </main>
    </div>
  )
}

function Row({
  venue,
  market,
  symbol,
  quote,
  funding,
}: {
  venue: string
  market: string
  symbol: string
  quote: { bid: string; ask: string } | undefined
  funding: { rate: string; interval_seconds?: number | null } | undefined
}) {
  const bid = quote === undefined ? null : toNumber(quote.bid)
  const ask = quote === undefined ? null : toNumber(quote.ask)
  // In basis points of the mid, because a spread in currency is not comparable
  // across venues quoting the same market at different tick sizes.
  const spread = bid !== null && ask !== null && bid > 0 ? ((ask - bid) / ((ask + bid) / 2)) * 10_000 : null

  return (
    <tr className="border-t border-line/50">
      <td className="px-5 py-1.5">
        <span className="flex items-center gap-2">
          <span className="size-2 rounded-full" style={{ backgroundColor: colorFor(venue) }} />
          {venue}
          <span className="text-xs text-faint">{market}</span>
        </span>
      </td>
      <td className="px-3 py-1.5 font-mono text-xs text-muted">{symbol}</td>
      <td className="tnum px-3 py-1.5 text-right text-up">{quote?.bid ?? '—'}</td>
      <td className="tnum px-3 py-1.5 text-right text-down">{quote?.ask ?? '—'}</td>
      <td className="tnum px-3 py-1.5 text-right text-muted">
        {spread === null ? '—' : `${spread.toFixed(1)} bp`}
      </td>
      <td className="tnum px-5 py-1.5 text-right">
        <Funding funding={funding} />
      </td>
    </tr>
  )
}

/** The rate, per its OWN interval, with the interval said out loud. Venues fund
 *  on eight hours and on one; showing the bare number side by side compares two
 *  things that are not the same, and annualising without the interval is wrong
 *  by a factor of eight. */
function Funding({
  funding,
}: {
  funding: { rate: string; interval_seconds?: number | null } | undefined
}) {
  if (funding === undefined) return <span className="text-faint">—</span>
  const rate = toNumber(funding.rate)
  const hours = funding.interval_seconds ? funding.interval_seconds / 3600 : null
  return (
    <span className={rate > 0 ? 'text-up' : rate < 0 ? 'text-down' : 'text-muted'}>
      {(rate * 100).toFixed(4)}%
      <span className="ml-1 text-[10px] text-faint">
        {hours === null ? 'interval unstated' : `/${hours}h`}
      </span>
    </span>
  )
}
