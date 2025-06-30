import { useMemo, useRef, useState } from 'react'
import { useVirtualizer } from '@tanstack/react-virtual'
import { allInstruments, api, type Instrument } from '@/api/client'
import { Failed, Loading } from '@/components/Chrome'
import { useQuery } from '@/lib/query'

const columns = [
  { key: 'exchange_symbol', label: 'Symbol', align: 'left' },
  { key: 'market', label: 'Market', align: 'left' },
  { key: 'contract_kind', label: 'Kind', align: 'left' },
  { key: 'tick_size', label: 'Tick', align: 'right' },
  { key: 'lot_size', label: 'Lot', align: 'right' },
  { key: 'min_notional', label: 'Min', align: 'right' },
] as const

export function Instruments() {
  const [account, setAccount] = useState('')
  const [filter, setFilter] = useState('')
  const accounts = useQuery(() => api.accounts(), [])
  const rows = useQuery(
    () => allInstruments(account === '' ? {} : { account }),
    [account],
  )
  const parent = useRef<HTMLDivElement>(null)

  const shown = useMemo(() => {
    if (rows.state !== 'ready') return [] as Instrument[]
    const needle = filter.trim().toUpperCase()
    if (needle === '') return rows.data
    return rows.data.filter((r) => r.exchange_symbol.toUpperCase().includes(needle))
  }, [rows, filter])

  // Thousands of rows on an options venue, so the list is virtualised rather
  // than paginated: a chain is read by scrolling, not by clicking "next".
  const virtual = useVirtualizer({
    count: shown.length,
    getScrollElement: () => parent.current,
    estimateSize: () => 28,
    overscan: 16,
  })

  if (rows.state === 'failed') return <Failed {...rows} />

  return (
    <div className="flex h-full flex-col">
      <div className="flex items-center gap-3 border-b border-line px-5 py-2">
        <select
          value={account}
          onChange={(e) => setAccount(e.target.value)}
          className="rounded border border-line bg-surface px-2 py-1 text-sm"
        >
          <option value="">every gateway</option>
          {accounts.state === 'ready' &&
            accounts.data.map((a) => (
              <option key={a.account} value={a.account}>
                {a.account}
              </option>
            ))}
        </select>
        <input
          value={filter}
          onChange={(e) => setFilter(e.target.value)}
          placeholder="filter by symbol"
          className="rounded border border-line bg-surface px-2 py-1 text-sm placeholder:text-faint"
        />
        <span className="tnum ml-auto text-sm text-muted">
          {rows.state === 'ready' ? `${shown.length.toLocaleString()} instruments` : 'loading…'}
        </span>
      </div>

      {rows.state === 'loading' ? (
        <Loading what="the catalogue" />
      ) : (
        <>
          <div className="grid grid-cols-[2fr_1fr_1fr_1fr_1fr_1fr] gap-2 border-b border-line px-5 py-1.5 text-xs uppercase tracking-wider text-faint">
            {columns.map((c) => (
              <span key={c.key} className={c.align === 'right' ? 'text-right' : ''}>
                {c.label}
              </span>
            ))}
          </div>
          <div ref={parent} className="min-h-0 flex-1 overflow-auto">
            <div style={{ height: virtual.getTotalSize(), position: 'relative' }}>
              {virtual.getVirtualItems().map((item) => {
                const row = shown[item.index]
                if (row === undefined) return null
                return (
                  <div
                    key={row.instrument_id}
                    className="absolute grid w-full grid-cols-[2fr_1fr_1fr_1fr_1fr_1fr] gap-2 px-5 text-sm hover:bg-raised/60"
                    style={{ height: item.size, transform: `translateY(${item.start}px)` }}
                  >
                    <span className="truncate py-1 font-mono">{row.exchange_symbol}</span>
                    <span className="py-1 text-muted">{row.market}</span>
                    <span className="py-1 text-muted">{row.contract_kind}</span>
                    <span className="tnum py-1 text-right">{row.tick_size}</span>
                    <span className="tnum py-1 text-right">{row.lot_size}</span>
                    <span className="tnum py-1 text-right text-muted">{row.min_notional}</span>
                  </div>
                )
              })}
            </div>
          </div>
        </>
      )}
    </div>
  )
}
