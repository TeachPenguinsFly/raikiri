import { useMemo, type CSSProperties } from 'react'
import { Link } from 'react-router'
import { cn, ExchangeIcon, venueLabel } from '@raikiri/ui'
import { compact, Empty, Flash, Glyph, Heading, Kpi, Problem, Skeleton, Ticker, trackPointer } from '@raikiri/ui/v3'
import { api, type Account, type VenueMetrics } from './api/client'
import { useQuotes } from './api/stream'
import {
  bp,
  crossOf,
  px,
  spreadBp,
  touchOf,
  useCatalog,
  useQuery,
  useRanked,
  usd,
  venueColor,
  type Cross,
  type Pair,
} from './lib/market'

/** the busiest markets carry the table and the cross-venue scan; the rest are a search away in the terminal */
const k_scan = 60
const k_table = 16

const terminalOf = (key: string, venue?: string) =>
  `/crypto/terminal?m=${encodeURIComponent(key)}${venue === undefined ? '' : `&v=${venue}`}`

const ms = (ns: number | undefined) => (ns === undefined ? '—' : `${Math.round(ns / 1e6)} ms`)

function Stat({ label, children, tone }: { label: string; children: React.ReactNode; tone?: string }) {
  return (
    <div className="flex flex-col gap-0.5">
      <span className="text-[10px] tracking-wider text-fg-dim uppercase">{label}</span>
      <span className={cn('mono text-xs', tone ?? 'text-fg')}>{children}</span>
    </div>
  )
}

function VenueCard({ account, metrics, d }: { account: Account; metrics: VenueMetrics | undefined; d: number }) {
  const g = account.gateway
  const live = g.alive && g.coverage_pct >= 50
  const rate = metrics?.channels.reduce((s, c) => s + c.msgs_per_s, 0)
  const bbo = metrics?.channels.find((c) => c.channel === 'bbo')
  const offset = metrics?.clock.offset_ns
  return (
    <div
      onPointerMove={trackPointer}
      className="v3-glow v3-rise flex flex-col gap-3 rounded-lg bg-panel p-3.5 shadow-[0_0_0_1px_var(--color-line)]"
      style={{ '--d': `${d}ms` } as CSSProperties}
    >
      <div className="flex items-center gap-2">
        <ExchangeIcon venue={account.venue} className="size-5" />
        <span className="text-sm font-semibold text-fg">{venueLabel(account.venue)}</span>
        <span className="rounded-sm bg-raised px-1.5 py-0.5 text-[10px] text-fg-dim uppercase">{account.market}</span>
        <span
          className={cn(
            'ml-auto flex items-center gap-1.5 text-2xs',
            live ? 'text-up' : g.alive ? 'text-amber' : 'text-down',
          )}
        >
          <span className={cn('size-1.5 rounded-full', live ? 'pulse-dot bg-up' : g.alive ? 'bg-amber' : 'bg-down')} />
          {live ? 'Live' : g.alive ? 'Warming up' : 'Down'}
        </span>
      </div>
      <div className="flex flex-col gap-1">
        <div className="flex items-baseline justify-between text-2xs">
          <span className="text-fg-dim">Priced</span>
          <span className="mono text-fg-mid">
            <span className="text-fg">{g.priced}</span> / {g.instruments}
          </span>
        </div>
        <div className="h-1 overflow-hidden rounded-full bg-raised">
          <div
            className="h-full rounded-full transition-[width] duration-700 ease-out"
            style={{ width: `${g.coverage_pct}%`, background: venueColor(account.venue) }}
          />
        </div>
      </div>
      <div className="grid grid-cols-4 gap-2">
        <Stat label="Msg/s">
          <Ticker value={rate} format={(v) => Math.round(v).toLocaleString('en-US')} />
        </Stat>
        <Stat label="Lag p50">{ms(bbo?.lag_p50_ns)}</Stat>
        <Stat label="Clock">{offset === undefined ? '—' : `${(offset / 1e6).toFixed(0)} ms`}</Stat>
        <Stat label="Reopens" tone={(metrics?.reopens ?? 0) > 10 ? 'text-amber' : 'text-fg'}>
          {metrics?.reopens ?? '—'}
        </Stat>
      </div>
      <div className="flex flex-wrap gap-1">
        {(metrics?.channels ?? []).map((c) => (
          <span key={c.channel} className="mono rounded-sm bg-canvas/60 px-1.5 py-0.5 text-[10px] text-fg-dim">
            {c.channel}{' '}
            <span className="text-fg-mid">
              {c.msgs_per_s < 1_000 ? Math.round(c.msgs_per_s) : compact(c.msgs_per_s)}/s
            </span>
          </span>
        ))}
      </div>
    </div>
  )
}

function MarketRow({
  pair,
  volume,
  quotes,
  cross,
}: {
  pair: Pair
  volume: number
  quotes: ReturnType<typeof useQuotes>
  cross: Cross | undefined
}) {
  const leg = pair.legs[0]
  const t = leg === undefined ? undefined : touchOf(quotes.get(leg.id), leg)
  const places = leg?.places ?? 2
  return (
    <Link
      to={terminalOf(pair.key)}
      className="grid h-9 grid-cols-[minmax(90px,1.2fr)_72px_1fr_1fr_64px_64px_72px] items-center gap-2 px-3 text-xs transition-colors hover:bg-raised/60"
    >
      <span className="flex min-w-0 items-baseline gap-1">
        <span className="truncate font-medium text-fg">{pair.base}</span>
        <span className="text-[10px] text-fg-dim">{pair.quote}</span>
      </span>
      <span className="flex items-center -space-x-1">
        {pair.legs.map((l) => (
          <ExchangeIcon key={l.venue} venue={l.venue} className="size-3.5 ring-2 ring-panel" />
        ))}
      </span>
      <span className="mono text-right text-up">
        <Flash value={t?.bid}>{px(t?.bid, places)}</Flash>
      </span>
      <span className="mono text-right text-down">
        <Flash value={t?.ask}>{px(t?.ask, places)}</Flash>
      </span>
      <span className="mono text-right text-fg-mid">{bp(spreadBp(t))}</span>
      <span
        className={cn(
          'mono text-right',
          cross === undefined ? 'text-fg-dim' : cross.bp > 0 ? 'text-up' : 'text-fg-mid',
        )}
      >
        {cross === undefined ? '—' : cross.bp.toFixed(1)}
      </span>
      <span className="mono text-right text-fg-mid">{usd(volume)}</span>
    </Link>
  )
}

/** the pair's best bid on one venue against its best ask on another, and every venue's mid off their average */
function CrossCard({ x, d }: { x: Cross; d: number }) {
  const places = Math.max(...x.pair.legs.map((l) => l.places))
  const avg = x.mids.reduce((s, m) => s + m.mid, 0) / x.mids.length
  return (
    <Link
      to={terminalOf(x.pair.key, x.buy.leg.venue)}
      className="v3-rise flex flex-col gap-1.5 rounded-md bg-panel px-3 py-2.5 shadow-[0_0_0_1px_var(--color-line)] transition-colors hover:bg-raised"
      style={{ '--d': `${d}ms` } as CSSProperties}
    >
      <span className="flex items-center gap-2 text-xs">
        <span className="font-medium text-fg">{x.pair.base}</span>
        <span className="text-fg-dim">{x.pair.legs.length} venues</span>
        <span className={cn('mono ml-auto font-semibold', x.bp > 0 ? 'text-up' : 'text-fg-mid')}>
          {x.bp > 0 ? '+' : ''}
          {bp(x.bp)}
        </span>
      </span>
      <span className="flex flex-wrap items-center gap-x-3 gap-y-0.5 text-[11px] text-fg-mid">
        <span className="flex items-center gap-1">
          <span className="text-up">Buy</span>
          <ExchangeIcon venue={x.buy.leg.venue} className="size-3" />
          <span className="mono text-fg">{px(x.buy.px, places)}</span>
        </span>
        <Glyph name="arrow" className="size-3 text-fg-dim" />
        <span className="flex items-center gap-1">
          <span className="text-down">Sell</span>
          <ExchangeIcon venue={x.sell.leg.venue} className="size-3" />
          <span className="mono text-fg">{px(x.sell.px, places)}</span>
        </span>
        <span className="ml-auto flex items-center gap-2">
          {x.mids.map((m) => (
            <span key={m.leg.venue} className="mono flex items-center gap-1 text-[10px] text-fg-dim">
              <span className="size-1.5 rounded-full" style={{ background: venueColor(m.leg.venue) }} />
              {(((m.mid - avg) / avg) * 1e4).toFixed(1)}
            </span>
          ))}
        </span>
      </span>
    </Link>
  )
}

export function DashboardPage() {
  const health = useQuery('health', () => api.health(), 5_000)
  const metrics = useQuery('metrics', () => api.metrics(), 5_000)
  const catalog = useCatalog()
  const all = useRanked(catalog.state === 'ready' ? catalog.data : undefined)
  // the board ranks perpetuals; spot listings of the same coins would show each market twice
  const ranked = useMemo(() => all.filter((r) => r.pair.market === 'perp'), [all])
  const top = useMemo(() => ranked.slice(0, k_scan), [ranked])
  const ids = useMemo(() => top.flatMap((r) => r.pair.legs.map((l) => l.id)), [top])
  const quotes = useQuotes(ids)
  const crosses = useMemo(() => {
    const out = new Map<string, Cross>()
    for (const r of top) {
      const x = crossOf(r.pair, quotes)
      if (x !== undefined) out.set(r.pair.key, x)
    }
    return out
  }, [top, quotes])
  const widest = useMemo(() => [...crosses.values()].sort((a, b) => b.bp - a.bp), [crosses])

  const gateways = health.state === 'ready' ? health.data.gateways : []
  const byAccount = new Map((metrics.state === 'ready' ? metrics.data : []).map((m) => [m.account, m]))
  const live = gateways.filter((g) => g.gateway.alive).length
  const priced = gateways.reduce((s, g) => s + g.gateway.priced, 0)
  const listed = gateways.reduce((s, g) => s + g.gateway.instruments, 0)
  const rate =
    metrics.state === 'ready'
      ? metrics.data.reduce((s, m) => s + m.channels.reduce((a, c) => a + c.msgs_per_s, 0), 0)
      : undefined
  const pairs = catalog.state === 'ready' ? catalog.data.pairs.filter((p) => p.market === 'perp') : []
  const shared = pairs.filter((p) => p.legs.length > 1).length
  const turnover = ranked.reduce((s, r) => s + r.volume, 0)
  const count = (v: number) => Math.round(v).toLocaleString('en-US')

  return (
    <div className="h-full overflow-y-auto">
      <div className="mx-auto flex max-w-[1600px] flex-col gap-6 p-3 min-[900px]:p-5">
        <section
          className="v3-glow grid grid-cols-2 divide-line overflow-hidden rounded-lg bg-panel shadow-[0_0_0_1px_var(--color-line)] min-[700px]:grid-cols-3 min-[1200px]:grid-cols-6 min-[1200px]:divide-x"
          onPointerMove={trackPointer}
        >
          <Kpi
            d={0}
            to="/crypto/dashboard"
            label="Venues live"
            value={health.state === 'ready' ? live : undefined}
            format={count}
            sub={`of ${gateways.length}`}
          />
          <Kpi
            d={40}
            to="/crypto/terminal"
            label="Priced"
            value={health.state === 'ready' ? priced : undefined}
            format={count}
            sub={`of ${listed.toLocaleString('en-US')} listed`}
          />
          <Kpi d={80} to="/crypto/dashboard" label="Messages / s" value={rate} format={count} />
          <Kpi
            d={120}
            to="/crypto/terminal"
            label="24h turnover"
            value={ranked.length > 0 ? turnover : undefined}
            format={usd}
          />
          <Kpi
            d={160}
            to="/crypto/terminal"
            label="On 2+ venues"
            value={catalog.state === 'ready' ? shared : undefined}
            format={count}
            sub={`of ${pairs.length} markets`}
          />
          <Kpi
            d={200}
            to={widest[0] === undefined ? '/crypto/terminal' : terminalOf(widest[0].pair.key, widest[0].buy.leg.venue)}
            label="Widest cross"
            value={widest[0]?.bp}
            format={(v) => `${v.toFixed(1)} bp`}
            sub={widest[0]?.pair.base}
          />
        </section>

        <section className="flex flex-col gap-3">
          <Heading glyph={<Glyph name="g-live" className="size-4 text-accent" />} title="Venues">
            <span className="mono text-xs text-fg-dim">{gateways.length}</span>
          </Heading>
          <div className="grid gap-3 [grid-template-columns:repeat(auto-fill,minmax(300px,1fr))]">
            {health.state === 'loading' && Array.from({ length: 3 }, (_, i) => <Skeleton key={i} className="h-44" />)}
            {health.state === 'failed' && <Problem title={health.title} />}
            {gateways.map((g, i) => (
              <VenueCard key={g.account} account={g} metrics={byAccount.get(g.account)} d={i * 40} />
            ))}
          </div>
        </section>

        <div className="grid gap-5 min-[1200px]:grid-cols-[3fr_2fr]">
          <section className="flex min-w-0 flex-col gap-3">
            <Heading
              glyph={<Glyph name="g-trending" className="size-4 text-accent" />}
              title="Top markets"
              to="/crypto/terminal"
              more="Terminal"
            />
            <div className="overflow-hidden rounded-lg bg-panel shadow-[0_0_0_1px_var(--color-line)]">
              <div className="col-head grid h-8 grid-cols-[minmax(90px,1.2fr)_72px_1fr_1fr_64px_64px_72px] items-center gap-2 border-b border-line px-3">
                <span>Market</span>
                <span>Venues</span>
                <span className="text-right">Bid</span>
                <span className="text-right">Ask</span>
                <span className="text-right">Spread</span>
                <span className="text-right">Cross</span>
                <span className="text-right">24h</span>
              </div>
              {ranked.length === 0 ? (
                <div className="flex flex-col gap-1 p-2">
                  {Array.from({ length: 8 }, (_, i) => (
                    <Skeleton key={i} className="h-7" />
                  ))}
                </div>
              ) : (
                <div className="divide-y divide-line/60">
                  {ranked.slice(0, k_table).map((r) => (
                    <MarketRow
                      key={r.pair.key}
                      pair={r.pair}
                      volume={r.volume}
                      quotes={quotes}
                      cross={crosses.get(r.pair.key)}
                    />
                  ))}
                </div>
              )}
            </div>
          </section>
          <section className="flex min-w-0 flex-col gap-3">
            <Heading glyph={<Glyph name="layers" className="size-4 text-accent" />} title="Across venues">
              <span className="mono text-xs text-fg-dim">{crosses.size}</span>
            </Heading>
            <div className="flex flex-col gap-1.5">
              {widest.length === 0 &&
                (catalog.state === 'failed' ? (
                  <Empty title={catalog.title} />
                ) : (
                  Array.from({ length: 6 }, (_, i) => <Skeleton key={i} className="h-14" />)
                ))}
              {widest.slice(0, 9).map((x, i) => (
                <CrossCard key={x.pair.key} x={x} d={Math.min(i, 8) * 40} />
              ))}
            </div>
          </section>
        </div>
      </div>
    </div>
  )
}
