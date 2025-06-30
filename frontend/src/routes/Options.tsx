import { useMemo, useState } from 'react'
import { allInstruments, type Instrument } from '@/api/client'
import { Failed, Loading } from '@/components/Chrome'
import { useQuery } from '@/lib/query'

/** Nanoseconds to a date. Expiries arrive as digit strings because the value
 *  is past 2^53 and a JSON number would round it; BigInt is what reads one
 *  back without losing the low bits. */
function expiryLabel(ns: string): string {
  const ms = Number(BigInt(ns) / 1_000_000n)
  return new Date(ms).toISOString().slice(0, 10)
}

type Row = { strike: string; call?: Instrument; put?: Instrument }

/** One expiry's chain, calls and puts sharing a strike row. Sorted NUMERICALLY:
 *  strikes are decimal strings, and sorting them as text puts 9000 above
 *  40000. */
function chain(rows: Instrument[]): Row[] {
  const byStrike = new Map<string, Row>()
  for (const row of rows) {
    if (row.strike === undefined || row.put_call === undefined) continue
    const entry = byStrike.get(row.strike) ?? { strike: row.strike }
    if (row.put_call === 'call') entry.call = row
    else entry.put = row
    byStrike.set(row.strike, entry)
  }
  return [...byStrike.values()].sort((a, b) => Number(a.strike) - Number(b.strike))
}

export function Options() {
  const rows = useQuery(() => allInstruments({ kind: 'option' }), [])
  const [expiry, setExpiry] = useState<string | null>(null)
  const [underlying, setUnderlying] = useState<string>('')

  const { expiries, underlyings, chainRows } = useMemo(() => {
    if (rows.state !== 'ready') {
      return { expiries: [] as string[], underlyings: [] as string[], chainRows: [] as Row[] }
    }
    // The venue puts the underlying at the front of the symbol; nothing in the
    // catalogue names it separately, so this reads it rather than inventing a
    // field the contract does not have.
    const roots = [...new Set(rows.data.map((r) => r.exchange_symbol.split('-')[0] ?? ''))].sort()
    const root = underlying === '' ? (roots[0] ?? '') : underlying
    const forRoot = rows.data.filter((r) => r.exchange_symbol.startsWith(`${root}-`))
    const dates = [...new Set(forRoot.map((r) => r.expiry ?? ''))].filter(Boolean).sort()
    const chosen = expiry !== null && dates.includes(expiry) ? expiry : (dates[0] ?? null)
    return {
      expiries: dates,
      underlyings: roots,
      chainRows: chosen === null ? [] : chain(forRoot.filter((r) => r.expiry === chosen)),
    }
  }, [rows, expiry, underlying])

  if (rows.state === 'loading') return <Loading what="the option chain" />
  if (rows.state === 'failed') return <Failed {...rows} />

  if (rows.data.length === 0) {
    return (
      <div className="p-8 text-sm text-muted">
        No options are loaded. Only Deribit serves them, and only through a gateway started with
        <code className="mx-1 text-ink">--market=option</code>.
      </div>
    )
  }

  const active = expiry ?? expiries[0] ?? null

  return (
    <div className="flex h-full flex-col">
      <div className="flex flex-wrap items-center gap-2 border-b border-line px-5 py-2">
        <select
          value={underlying === '' ? (underlyings[0] ?? '') : underlying}
          onChange={(e) => {
            setUnderlying(e.target.value)
            setExpiry(null)
          }}
          className="rounded border border-line bg-surface px-2 py-1 text-sm"
        >
          {underlyings.map((u) => (
            <option key={u} value={u}>
              {u}
            </option>
          ))}
        </select>
        <div className="flex flex-wrap gap-1">
          {expiries.map((e) => (
            <button
              key={e}
              onClick={() => setExpiry(e)}
              className={`rounded px-2 py-1 font-mono text-xs ${
                e === active ? 'bg-accent text-white' : 'bg-surface text-muted hover:text-ink'
              }`}
            >
              {expiryLabel(e)}
            </button>
          ))}
        </div>
        <span className="tnum ml-auto text-sm text-muted">{chainRows.length} strikes</span>
      </div>

      <div className="min-h-0 flex-1 overflow-auto">
        <table className="w-full text-sm">
          <thead className="sticky top-0 bg-canvas text-xs uppercase tracking-wider text-faint">
            <tr>
              <th className="px-3 py-2 text-left font-normal">Call</th>
              <th className="px-3 py-2 text-right font-normal">Tick</th>
              <th className="px-3 py-2 text-center font-normal text-ink">Strike</th>
              <th className="px-3 py-2 text-left font-normal">Tick</th>
              <th className="px-3 py-2 text-left font-normal">Put</th>
            </tr>
          </thead>
          <tbody>
            {chainRows.map((row) => (
              <tr key={row.strike} className="border-t border-line/50 hover:bg-raised/50">
                <td className="px-3 py-1 font-mono text-up/90">
                  {row.call?.exchange_symbol ?? '—'}
                </td>
                <td className="tnum px-3 py-1 text-right text-muted">
                  {row.call?.tick_size ?? ''}
                </td>
                <td className="tnum bg-surface/60 px-3 py-1 text-center">{row.strike}</td>
                <td className="tnum px-3 py-1 text-muted">{row.put?.tick_size ?? ''}</td>
                <td className="px-3 py-1 font-mono text-down/90">
                  {row.put?.exchange_symbol ?? '—'}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      <div className="border-t border-line px-5 py-2 text-xs text-faint">
        Greeks, implied vol and quotes ride the live stream and are not wired yet — the chain
        below is the catalogue. The venue publishes all of them; nothing here invents one.
      </div>
    </div>
  )
}
