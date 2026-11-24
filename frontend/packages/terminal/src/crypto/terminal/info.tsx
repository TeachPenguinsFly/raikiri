import { useEffect, useState, type ReactNode } from 'react'
import { ExchangeIcon, venueLabel } from '@raikiri/ui'
import { CoinIcon, coinIdAsync, coinName, compact, Empty } from '@raikiri/ui/v3'
import { px, usd, type Leg, type Pair } from '../lib/market'
import { spanLabel, Stat } from './header'
import { useTerminal } from './state'

interface Found {
  id: string
  symbol: string
  market_cap_rank: number | null
}

type Usd = { usd?: number | null }

interface Coin {
  name?: string
  market_cap_rank?: number | null
  genesis_date?: string | null
  description?: { en?: string }
  links?: { homepage?: string[]; whitepaper?: string; blockchain_site?: string[] }
  market_data?: {
    market_cap?: Usd
    fully_diluted_valuation?: Usd
    circulating_supply?: number | null
    total_supply?: number | null
    max_supply?: number | null
    ath?: Usd
    ath_date?: { usd?: string }
    atl?: Usd
    atl_date?: { usd?: string }
  }
}

async function ask<T>(path: string): Promise<T> {
  const r = await fetch(`/cg/${path}`, { headers: { accept: 'application/json' } })
  if (!r.ok) throw new Error(`HTTP ${r.status}`)
  return (await r.json()) as T
}

/** a ticker scripts/coin_icons.py left unmatched: the largest coin CoinGecko's search finds under it */
async function search(base: string): Promise<string | undefined> {
  // venues list small coins by the thousand or the million: 1000PEPE and kPEPE are PEPE
  const symbol = base.replace(/^(1000000|10000|1000|1M|k)(?=[A-Z])/, '').toLowerCase()
  const { coins } = await ask<{ coins: Found[] }>(`search?query=${encodeURIComponent(symbol)}`)
  return coins
    .filter((c) => c.symbol.toLowerCase() === symbol)
    .sort((a, b) => (a.market_cap_rank ?? Infinity) - (b.market_cap_rank ?? Infinity))[0]?.id
}

/** the coin behind a base asset, null when none carries the symbol or a stock does */
async function coinOf(base: string): Promise<Coin | null> {
  const known = await coinIdAsync(base)
  const id = known === null ? undefined : (known ?? (await search(base)))
  if (id === undefined) return null
  return ask<Coin>(
    `coins/${encodeURIComponent(id)}?localization=false&tickers=false&community_data=false&developer_data=false`,
  )
}

const k_coins = new Map<string, Promise<Coin | null>>()

function cached(base: string): Promise<Coin | null> {
  let p = k_coins.get(base)
  if (p === undefined) {
    p = coinOf(base)
    k_coins.set(base, p)
    // a failure is asked again the next time the tab opens
    p.catch(() => k_coins.delete(base))
  }
  return p
}

type Facts = { state: 'loading' } | { state: 'ready'; coin: Coin } | { state: 'none' } | { state: 'failed' }

function useCoin(base: string | undefined): Facts {
  const [facts, setFacts] = useState<Facts>({ state: 'loading' })
  useEffect(() => {
    setFacts({ state: 'loading' })
    if (base === undefined) return
    let live = true
    cached(base).then(
      (coin) => live && setFacts(coin === null ? { state: 'none' } : { state: 'ready', coin }),
      () => live && setFacts({ state: 'failed' }),
    )
    return () => {
      live = false
    }
  }, [base])
  return facts
}

const day = (s: string | null | undefined) => {
  const d = s === undefined || s === null ? undefined : new Date(s)
  return d === undefined || Number.isNaN(d.getTime())
    ? undefined
    : d.toLocaleDateString('en-US', { year: 'numeric', month: 'short', day: 'numeric', timeZone: 'UTC' })
}

const dollars = (v: number) => `$${v >= 1 ? px(v, 2) : Number(v.toPrecision(4)).toString()}`

const has = (v: number | null | undefined): v is number => typeof v === 'number' && Number.isFinite(v) && v > 0

const web = (s: string | undefined) => (s !== undefined && /^https?:\/\//.test(s) ? s : undefined)

function Tiles({ children }: { children: ReactNode }) {
  return <div className="flex flex-wrap gap-x-8 gap-y-3">{children}</div>
}

function CoinFacts({ coin, base }: { coin: Coin; base: string }) {
  const m = coin.market_data
  const amount = (v: number) => `${compact(v)} ${base}`
  const high = m?.ath?.usd
  const low = m?.atl?.usd
  const highDay = day(m?.ath_date?.usd)
  const lowDay = day(m?.atl_date?.usd)
  const launched = day(coin.genesis_date)
  const links = [
    ['Website', web(coin.links?.homepage?.find((l) => l !== ''))],
    ['Whitepaper', web(coin.links?.whitepaper)],
    ['Explorer', web(coin.links?.blockchain_site?.find((l) => l !== ''))],
  ] as const
  const about = (coin.description?.en ?? '').replace(/<[^>]+>/g, '').trim()
  return (
    <>
      <Tiles>
        {has(coin.market_cap_rank) && <Stat label="Rank">#{coin.market_cap_rank}</Stat>}
        {has(m?.market_cap?.usd) && <Stat label="Market cap">{usd(m.market_cap.usd)}</Stat>}
        {has(m?.fully_diluted_valuation?.usd) && (
          <Stat label="Fully diluted">{usd(m.fully_diluted_valuation.usd)}</Stat>
        )}
        {has(m?.circulating_supply) && <Stat label="Circulating">{amount(m.circulating_supply)}</Stat>}
        {has(m?.total_supply) && <Stat label="Total supply">{amount(m.total_supply)}</Stat>}
        {has(m?.max_supply) && <Stat label="Max supply">{amount(m.max_supply)}</Stat>}
        {has(high) && (
          <Stat label="All-time high">
            {dollars(high)}
            {highDay !== undefined && <span className="ml-2 text-fg-dim">{highDay}</span>}
          </Stat>
        )}
        {has(low) && (
          <Stat label="All-time low">
            {dollars(low)}
            {lowDay !== undefined && <span className="ml-2 text-fg-dim">{lowDay}</span>}
          </Stat>
        )}
        {launched !== undefined && <Stat label="Launched">{launched}</Stat>}
      </Tiles>
      {links.some(([, href]) => href !== undefined) && (
        <div className="flex flex-wrap gap-2">
          {links.map(
            ([label, href]) =>
              href !== undefined && (
                <a
                  key={label}
                  href={href}
                  target="_blank"
                  rel="noreferrer"
                  className="rounded-md bg-raised px-2.5 py-1 text-xs text-fg-mid transition-colors hover:bg-hover hover:text-fg"
                >
                  {label}
                </a>
              ),
          )}
        </div>
      )}
      {about !== '' && <p className="line-clamp-6 max-w-[70ch] text-sm leading-relaxed text-fg-mid">{about}</p>}
    </>
  )
}

function Contract({ pair, leg, asset }: { pair: Pair; leg: Leg; asset: string | undefined }) {
  const { funding } = useTerminal()
  const every = pair.market === 'perp' ? funding.get(leg.id)?.interval_seconds : undefined
  return (
    <Tiles>
      {asset !== undefined && <Stat label="Asset">{asset}</Stat>}
      <Stat label="Venue">
        <span className="flex items-center gap-1.5 font-sans">
          <ExchangeIcon venue={leg.venue} className="size-3.5" />
          {venueLabel(leg.venue)}
        </span>
      </Stat>
      <Stat label="Symbol">{leg.exchange}</Stat>
      {/* until the venue's instrument lands these are defaults, not its numbers */}
      {leg.sized && <Stat label="Tick size">{leg.tick.toFixed(leg.places)}</Stat>}
      {leg.sized && (
        <Stat label="Contract unit">
          {leg.unit} {pair.base}
        </Stat>
      )}
      {leg.quote !== '' && <Stat label="Quote">{leg.quote}</Stat>}
      {every !== undefined && every !== null && <Stat label="Funding interval">{spanLabel(every)}</Stat>}
    </Tiles>
  )
}

const k_classes: Record<string, string> = { equity: 'Stock', index: 'Index', commodity: 'Commodity', fx: 'Currency' }

/** what the coin is, and what the chosen venue's contract on it is */
export function InfoTab() {
  const { pair, leg } = useTerminal()
  // a market admind keys by a class other than crypto is no coin, whatever coin shares its ticker
  const asset = pair === undefined ? undefined : k_classes[pair.key.split(':')[0] ?? '']
  // a listing split off its market for quoting far from it is not the coin its ticker names
  const facts = useCoin(pair === undefined || pair.stray || asset !== undefined ? undefined : pair.base)
  if (pair === undefined || leg === undefined) return <Empty title="Pick a market" />
  return (
    <div className="flex h-full flex-col gap-5 overflow-y-auto p-4">
      <div className="flex items-center gap-2.5">
        <CoinIcon symbol={pair.base} plain={pair.stray} className="size-7" />
        <span className="text-base font-semibold text-fg">
          {(facts.state === 'ready' ? facts.coin.name : undefined) ??
            (pair.stray ? undefined : coinName(pair.base)) ??
            pair.base}
        </span>
        <span className="text-sm text-fg-dim">{pair.base}</span>
      </div>
      <Contract pair={pair} leg={leg} asset={asset} />
      {asset !== undefined ? null : pair.stray || facts.state === 'none' ? (
        <Empty title={`No coin facts for ${pair.base}`} />
      ) : facts.state === 'failed' ? (
        <Empty title="Coin facts unavailable" />
      ) : facts.state === 'loading' ? (
        <div className="text-sm text-fg-dim">Loading</div>
      ) : (
        <CoinFacts coin={facts.coin} base={pair.base} />
      )}
    </div>
  )
}
