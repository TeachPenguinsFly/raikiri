import { useState, useSyncExternalStore } from 'react'
import { cn } from '../lib/format'
import k_names from '../assets/coins/names.json'
import k_marks_url from '../assets/coins/marks.json?url'

/** every file in assets/coins is a coin's mark, keyed by its ticker; scripts/coin_icons.py refreshes them. Each is
 * a file of its own, fetched when shown: inlined, the marks put a megabyte into the terminal's script */
const k_icons: Record<string, string> = Object.fromEntries(
  Object.entries(
    import.meta.glob<string>('../assets/coins/*.webp', { eager: true, import: 'default', query: '?no-inline' }),
  ).map(([path, url]) => [path.slice(path.lastIndexOf('/') + 1, path.lastIndexOf('.')), url]),
)

/** every other ticker scripts/coin_icons.py matched: catalogue id, name and image path on the catalogue's image host,
 * empty for the coins with a file above; a stock's has no id, its logo's path and 'eq'. Thousands of them, so fetched
 * once, when the first icon without a file of its own is drawn. admind reads the same file and serves the images */
type Mark = [id: string, name: string, image: string, kind?: 'eq']
let loaded: Record<string, Mark> | undefined
let loading: Promise<Record<string, Mark>> | undefined
const waiting = new Set<() => void>()

function marks(): Promise<Record<string, Mark>> {
  loading ??= fetch(k_marks_url)
    .then((r): Promise<Record<string, Mark>> | Record<string, Mark> => (r.ok ? r.json() : {}))
    .catch(() => ({}))
    .then((m) => {
      loaded = m
      for (const f of waiting) f()
      return m
    })
  return loading
}

const subscribe = (f: () => void) => {
  waiting.add(f)
  void marks()
  return () => void waiting.delete(f)
}
const unsubscribed = () => () => {}
const snapshot = () => loaded

/** where admind answers; '' at the root, a prefix inside a larger shell. This package knows no api client, so the
 * app says */
let iconBase = ''

export function setCoinIconBase(base: string) {
  iconBase = base
}

/** admind's copy of the mark, by ticker */
const markSrc = (key: string, m: Mark | undefined) =>
  m === undefined || m[2] === '' ? undefined : `${iconBase}/v1/assets/${encodeURIComponent(key)}/icon`

/** 1000PEPE and kPEPE are PEPE counted in thousands */
const bare = (symbol: string) => symbol.toUpperCase().replace(/^(1000000|1000|1M|K)(?=[A-Z])/, '')

/** metals that perps list under their iso codes, which no coin list carries: name, element, disc and ink */
const k_metals: Record<string, [string, string, string, string]> = {
  XAU: ['Gold', 'Au', '#d9aa3b', '#4a3507'],
  XAG: ['Silver', 'Ag', '#c3c7cd', '#33373d'],
  XPT: ['Platinum', 'Pt', '#a9b7c2', '#26323b'],
  XPD: ['Palladium', 'Pd', '#cfc6b6', '#3f382c'],
}

/** what a ticker is called in full, where the coin list knows it */
export function coinName(symbol: string): string | undefined {
  const key = bare(symbol)
  return (k_names as Record<string, string>)[key] ?? (loaded?.[key]?.[1] || undefined) ?? k_metals[key]?.[0]
}

/** a coin's round mark, or its first letter on a disc when the coin list has none */
export function CoinIcon({
  symbol,
  className,
  plain = false,
}: {
  symbol: string
  className?: string
  plain?: boolean
}) {
  const key = bare(symbol)
  // plain: a listing admind split off for quoting apart from the coin its ticker names, so the coin's mark would mislead
  const icon = plain ? undefined : k_icons[key]
  const all = useSyncExternalStore(plain || icon !== undefined ? unsubscribed : subscribe, snapshot)
  const [broken, setBroken] = useState<string>()
  const src = icon ?? (plain ? undefined : markSrc(key, all?.[key]))
  const stock = icon === undefined && all?.[key]?.[3] === 'eq'
  const metal = plain ? undefined : k_metals[key]
  const c = cn('inline-block shrink-0 rounded-full', className)
  if (src !== undefined && src !== broken)
    // stock logos are drawn for white pages: some are white on clear, so they sit on a light disc and fit inside it
    return (
      <img
        src={src}
        alt=""
        decoding="async"
        onError={() => setBroken(src)}
        className={cn(c, stock ? 'bg-white object-contain p-[12%]' : 'object-cover')}
      />
    )
  if (metal !== undefined)
    return (
      <svg viewBox="0 0 20 20" className={c} aria-hidden="true">
        <circle cx="10" cy="10" r="10" fill={metal[2]} />
        <circle cx="10" cy="10" r="8.4" fill="none" stroke={metal[3]} strokeOpacity="0.3" strokeWidth="0.8" />
        <text x="10" y="13.4" fontSize="9" fontWeight="700" textAnchor="middle" fill={metal[3]}>
          {metal[1]}
        </text>
      </svg>
    )
  return (
    // the letter is drawn in the disc's own units so it scales with whatever size the disc is given
    <svg viewBox="0 0 20 20" className={cn(c, 'text-fg-mid')} aria-hidden="true">
      <circle cx="10" cy="10" r="10" fill="currentColor" fillOpacity="0.14" />
      <text x="10" y="14" fontSize="11" fontWeight="600" textAnchor="middle" fill="currentColor">
        {key.slice(0, 1)}
      </text>
    </svg>
  )
}
