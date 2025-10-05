import { useMemo, useState } from 'react'
import { EmptyState } from '@/components/shared/EmptyState'
import { PanelHeader } from '@/components/shared/PanelHeader'
import { SortHead, type Sort } from '@/components/shared/SortHead'
import { Venue } from '@/components/shared/Venue'
import { VenueFilter } from '@/components/scanner/VenueFilter'
import { api, type UpstreamColumn } from '@/api/client'
import { compact, nanosToMillis, toNumber } from '@/lib/format'
import { useQuery } from '@/lib/query'
import { cn } from '@/lib/utils'

/** A table this screen was not written for.
 *
 *  THE COLUMNS ARRIVE WITH THE DATA. What is worth scanning for, and how the
 *  edge in it is computed, is not this repository's to hold — so the upstream
 *  names its own tables and its own columns, and this file knows only how to
 *  draw a cell of each KIND. Hardcoding "funding spread" here would put the
 *  model in a public repository as surely as shipping the arithmetic would.
 *
 *  What it does own is the reading: sorting, a venue filter, and the difference
 *  between two kinds of empty. */

/** The upstream a terminal offers boards from. One neutral name, configurable
 *  at the proxy — `admind --upstream boards=<port>` — so the screen names no
 *  service and no service names itself here. */
const k_upstream = 'boards'

type Row = Record<string, string>

/** A cell, drawn by the column's declared KIND and never by its meaning.
 *
 *  `signed` is painted by sign because a number that can be a cost has to read
 *  as one; `venue` gets the mark that makes an identity scannable in a column
 *  of numbers; `duration` is an instant on the wire and time-remaining to a
 *  reader, which is the only form anybody acts on. */
function Cell({ column, value }: { column: UpstreamColumn; value: string | undefined }) {
  if (value === undefined || value === '') return <span className="text-fg-dim/40">—</span>
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
      if (Number.isNaN(left)) return <span className="text-fg-dim/40">—</span>
      if (left <= 0) return <span className="text-fg-dim">due</span>
      const minutes = Math.floor(left / 60_000)
      return (
        <span className="text-fg-dim">
          {minutes < 60 ? `${minutes}m` : `${Math.floor(minutes / 60)}h${String(minutes % 60).padStart(2, '0')}`}
        </span>
      )
    }
    case 'number':
      return <span>{Math.abs(toNumber(value)) >= 10_000 ? compact(toNumber(value)) : value}</span>
    default:
      return <span className="text-fg">{value}</span>
  }
}

const numeric = (column: UpstreamColumn): boolean =>
  column.kind === 'number' || column.kind === 'signed' || column.kind === 'duration'

/** OPAQUE, because a sticky `<tr>` paints nothing: the background has to sit on
 *  the cells or the rows scroll through the labels. */
const head = 'sticky top-0 z-10 border-b border-line bg-bg'

export function ScannerPage() {
  const index = useQuery(() => api.upstreamIndex(k_upstream), [])
  const [tab, setTab] = useState<string | null>(null)
  const tables = index.state === 'ready' ? index.data.tables : []
  const active = tab ?? tables[0]?.path ?? ''

  const table = useQuery(
    () => (active === '' ? Promise.reject(new Error('no table')) : api.upstreamTable(k_upstream, active)),
    [active],
    2000,
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
    for (const row of table.data.rows as Row[]) {
      for (const key of venueKeys) {
        const v = row[key]
        if (v !== undefined && v !== '') seen.add(v)
      }
    }
    return [...seen].sort()
  }, [table, venueKeys])

  const rows = useMemo(() => {
    if (table.state !== 'ready') return []
    const kept = (table.data.rows as Row[]).filter((row) => {
      if (venues.size === 0) return true
      // EVERY venue leg must be wanted, not just one: a row is a route, and a
      // route through a venue the reader excluded is not a route they can take.
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
          <EmptyState title={index.title} hint={index.detail ?? undefined} />
        )}
        {index.state === 'ready' && tables.length === 0 && (
          <EmptyState title="The upstream serves no tables" />
        )}

        {index.state === 'ready' && tables.length > 0 && table.state === 'loading' && (
          <EmptyState title="Waiting on the upstream…" />
        )}

        {/* THE REFUSAL IS THE ANSWER. A service that is not running produces no
            table, and an empty one here would say the search ran and found
            nothing — a claim about the market rather than about us. */}
        {table.state === 'failed' && (
          <EmptyState title={table.title} hint={table.detail ?? undefined} />
        )}

        {/* The upstream says which empty this is; only it knows whether it
            looked at nothing or looked at everything and kept none of it. */}
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
                      className={head}
                    >
                      {column.label}
                    </SortHead>
                  ))}
                </tr>
              </thead>
              <tbody>
                {rows.map((row, i) => (
                  <tr
                    key={columns.map((c) => row[c.key] ?? '').join('') + String(i)}
                    className="border-t border-line/40 hover:bg-bg-hi/40"
                  >
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
