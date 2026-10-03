import { useMemo, useState } from 'react'
import { EmptyState } from '@/components/shared/EmptyState'
import { PanelHeader } from '@/components/shared/PanelHeader'
import { SortHead, type Sort } from '@/components/shared/SortHead'
import { Venue } from '@/components/shared/Venue'
import { VenueFilter } from '@/components/scanner/VenueFilter'
import { api, type UpstreamColumn } from '@/api/client'
import { cn, compact, nanosToMillis, toNumber } from '@/lib/format'
import { useQuery } from '@/lib/query'

/** upstream name, set with admind --upstream boards=<port> */
const k_upstream = 'boards'

const k_refresh_ms = 2000

const k_compact_from = 10_000

const Blank = () => <span className="text-fg-dim/40">—</span>

function Cell({ column, value }: { column: UpstreamColumn; value: string | undefined }) {
  if (value === undefined || value === '') return <Blank />
  switch (column.kind) {
    case 'venue':
      return <Venue venue={value} />
    case 'signed': {
      const n = toNumber(value)
      return (
        <span className={cn(n > 0 ? 'text-down' : n < 0 ? 'text-up' : 'text-fg-mid')}>{value}</span>
      )
    }
    case 'duration': {
      const left = nanosToMillis(value) - Date.now()
      if (Number.isNaN(left)) return <Blank />
      if (left <= 0) return <span className="text-fg-dim">due</span>
      const minutes = Math.floor(left / 60_000)
      return (
        <span className="text-fg-dim">
          {minutes < 60 ? `${minutes}m` : `${Math.floor(minutes / 60)}h${String(minutes % 60).padStart(2, '0')}`}
        </span>
      )
    }
    case 'number':
      return <span>{Math.abs(toNumber(value)) >= k_compact_from ? compact(toNumber(value)) : value}</span>
    default:
      return <span className="text-fg">{value}</span>
  }
}

const numeric = (column: UpstreamColumn): boolean =>
  column.kind === 'number' || column.kind === 'signed' || column.kind === 'duration'

/** opaque because a sticky tr paints nothing */
const k_head = 'sticky top-0 z-10 border-b border-line bg-panel'

export function ScannerPage() {
  const index = useQuery(() => api.upstreamIndex(k_upstream), [])
  const [tab, setTab] = useState<string | null>(null)
  const tables = index.state === 'ready' ? index.data.tables : []
  const active = tab ?? tables[0]?.path ?? ''

  const table = useQuery(
    active === '' ? null : () => api.upstreamTable(k_upstream, active),
    [active],
    k_refresh_ms,
  )

  const [sort, setSort] = useState<Sort<string>>({ key: '', desc: true })
  const [venues, setVenues] = useState<Set<string>>(new Set())

  const columns = table.state === 'ready' ? table.data.columns : []
  const venueKeys = useMemo(
    () => columns.filter((c) => c.kind === 'venue').map((c) => c.key),
    [columns],
  )

  const available = useMemo(() => {
    if (table.state !== 'ready') return []
    const seen = new Set<string>()
    for (const row of table.data.rows) {
      for (const key of venueKeys) {
        const v = row[key]
        if (v !== undefined && v !== '') seen.add(v)
      }
    }
    return [...seen].sort()
  }, [table, venueKeys])

  const rows = useMemo(() => {
    if (table.state !== 'ready') return []
    const kept = (table.data.rows).filter((row) => {
      if (venues.size === 0) return true
      // every venue leg must pass the filter
      return venueKeys.every((key) => {
        const v = row[key]
        return v === undefined || v === '' || venues.has(v)
      })
    })
    if (sort.key === '') return kept
    const column = columns.find((c) => c.key === sort.key)
    return [...kept].sort((a, b) => {
      const left = a[sort.key]
      const right = b[sort.key]
      if (left === undefined || left === '') return right === undefined || right === '' ? 0 : 1
      if (right === undefined || right === '') return -1
      const cmp =
        column !== undefined && numeric(column)
          ? toNumber(left) - toNumber(right)
          : left.localeCompare(right)
      return sort.desc ? -cmp : cmp
    })
  }, [table, columns, sort, venues, venueKeys])

  return (
    <div className="flex h-full min-h-0 flex-col gap-2">
      <section className="panel flex min-h-0 flex-1 flex-col">
        <PanelHeader
          title={
            <div className="flex items-center gap-4">
              {tables.map((t) => (
                <button
                  key={t.path}
                  type="button"
                  role="tab"
                  aria-selected={t.path === active}
                  onClick={() => {
                    setTab(t.path)
                    setSort({ key: '', desc: true })
                  }}
                  className={cn(
                    'relative flex h-7 items-center text-xs transition-colors duration-150',
                    t.path === active
                      ? 'font-medium text-fg after:absolute after:inset-x-0 after:bottom-0 after:h-[2px] after:rounded-t-full after:bg-accent'
                      : 'text-fg-dim hover:text-fg-mid',
                  )}
                >
                  {t.title}
                </button>
              ))}
            </div>
          }
          right={
            available.length > 0 ? (
              <VenueFilter available={available} selected={venues} onChange={setVenues} />
            ) : undefined
          }
        />

        {index.state === 'failed' && (
          <EmptyState title={index.title} hint={index.detail} />
        )}
        {index.state === 'ready' && tables.length === 0 && (
          <EmptyState title="The upstream serves no tables" />
        )}

        {index.state === 'ready' && tables.length > 0 && table.state === 'loading' && (
          <EmptyState title="Waiting on the upstream…" />
        )}

        {table.state === 'failed' && (
          <EmptyState title={table.title} hint={table.detail} />
        )}

        {table.state === 'ready' && rows.length === 0 && (
          <EmptyState
            title={venues.size > 0 ? 'No row uses only the venues you kept' : 'No rows'}
            hint={venues.size > 0 ? undefined : (table.data.note ?? undefined)}
          />
        )}

        {table.state === 'ready' && rows.length > 0 && (
          <div className="min-h-0 flex-1 overflow-auto">
            <table className="w-full border-separate border-spacing-0 text-xs">
              <thead>
                <tr>
                  {columns.map((column) => (
                    <SortHead
                      key={column.key}
                      k={column.key}
                      sort={sort}
                      onSort={setSort}
                      right={column.align === 'right'}
                      className={k_head}
                    >
                      {column.label}
                    </SortHead>
                  ))}
                </tr>
              </thead>
              <tbody>
                {rows.map((row, i) => (
                  <tr key={i} className="border-t border-line/40 hover:bg-hover">
                    {columns.map((column) => (
                      <td
                        key={column.key}
                        className={cn(
                          'whitespace-nowrap px-3 py-1.5',
                          numeric(column) && 'num',
                          column.align === 'right' ? 'text-right' : 'text-left',
                        )}
                      >
                        <Cell column={column} value={row[column.key]} />
                      </td>
                    ))}
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </section>
    </div>
  )
}
