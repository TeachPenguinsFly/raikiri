import { createContext, useCallback, useContext, useEffect, useMemo, useState, type ReactNode } from 'react'
import { useSearchParams } from 'react-router'
import type { Book, Funding, Quote } from '../api/client'
import { useBooks, useFunding, useQuotes, useStreamDown } from '../api/stream'
import { useCatalog, type Catalog, type Leg, type Pair, type Query } from '../lib/market'

/** what a click on the book hands the ticket */
export interface Draft {
  action: 'buy' | 'sell'
  price?: number
  /** a fresh one each click, so the same level clicked twice still lands */
  n: number
}

/** one venue's ladder, or every venue's summed by price */
export type BookVenue = string | 'all'

export interface Terminal {
  catalog: Query<Catalog>
  connected: boolean
  pair: Pair | undefined
  /** the venue the chart and the ticket work on */
  leg: Leg | undefined
  bookVenue: BookVenue
  quotes: Map<string, Quote>
  books: Map<string, Book>
  funding: Map<string, Funding>
  draft: Draft | undefined
  pick: (p: { market?: string; venue?: string }) => void
  setBookVenue: (v: BookVenue) => void
  setDraft: (d: Omit<Draft, 'n'>) => void
}

const Ctx = createContext<Terminal | null>(null)

export function useTerminal(): Terminal {
  const t = useContext(Ctx)
  if (t === null) throw new Error('a terminal widget belongs inside TerminalProvider')
  return t
}

const k_default = 'crypto:BTC/USD*'
const k_levels = 25

export function TerminalProvider({ children }: { children: ReactNode }) {
  const [params, setParams] = useSearchParams()
  const catalog = useCatalog()
  const down = useStreamDown()
  const [bookAll, setBookAll] = useState(false)
  const [draft, setDraftState] = useState<Draft | undefined>(undefined)

  const cat = catalog.state === 'ready' ? catalog.data : undefined
  const key = params.get('m') ?? ''
  const pair = cat?.byKey.get(key)
  const leg = pair?.legs.find((l) => l.venue === params.get('v')) ?? pair?.legs[0]
  // one venue's book is the chosen venue's, so the chart, the book and the ticket stay on one place
  const bookVenue = bookAll ? 'all' : (leg?.venue ?? 'all')

  const pick = useCallback(
    (p: { market?: string; venue?: string }) =>
      setParams(
        (prev) => {
          const next = new URLSearchParams(prev)
          if (p.market !== undefined) next.set('m', p.market)
          if (p.venue !== undefined) next.set('v', p.venue)
          return next
        },
        { replace: true },
      ),
    [setParams],
  )

  // a bare terminal, or one on a market no venue lists any more, opens on the default
  useEffect(() => {
    if (cat === undefined || cat.byKey.has(key)) return
    const fallback = cat.byKey.get(k_default) ?? cat.pairs[0]
    if (fallback !== undefined) pick({ market: fallback.key })
  }, [cat, key, pick])

  const ids = useMemo(() => pair?.legs.map((l) => l.id) ?? [], [pair])
  const quotes = useQuotes(ids)
  const books = useBooks(ids, k_levels)
  const funding = useFunding(ids)

  const value: Terminal = {
    catalog,
    connected: !down,
    pair,
    leg,
    bookVenue,
    quotes,
    books,
    funding,
    draft,
    pick,
    setBookVenue: (v) => {
      setBookAll(v === 'all')
      if (v !== 'all') pick({ venue: v })
    },
    setDraft: (d) => setDraftState((old) => ({ ...d, n: (old?.n ?? 0) + 1 })),
  }
  return <Ctx.Provider value={value}>{children}</Ctx.Provider>
}
