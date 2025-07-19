import { useEffect, useMemo } from 'react'
import { api, type AccountBalances } from '@/api/client'
import { Failed, Loading } from '@/components/Chrome'
import { ChartFrame, colorFor, LinePlot } from '@/components/chart'
import { cn, compact, quantity, toNumber, usd } from '@/lib/format'
import { useEquity } from '@/lib/equity'
import { useQuery } from '@/lib/query'

/** Venues whose spot request answers the SAME wallet as the perpetual one.
 *  Running both gateways there and adding the two figures counts the money
 *  twice — the second row is not a second wallet, it is the first one seen
 *  again through another endpoint. */
const k_unified = new Set(['bybit', 'okx'])

/** The asset the desk is denominated in. Only this one is summed across venues:
 *  adding USDT to BTC produces a figure nobody holds. */
const k_denomination = 'USDT'

/** One asset held on one account, flattened out of the per-account reply. */
interface Row {
  account: string
  venue: string
  market: string
  asset: string
  total: number | null
  available: number | null
  unrealized: number | null
  stale: boolean
}

interface VenueGroup {
  venue: string
  rows: Row[]
  /** Named rather than counted: an account that has not answered is a hole in
   *  the total, and the reader has to know which one. */
  silent: string[]
  unified: boolean
}

/** The market a gateway serves, read off its account name — perp is the
 *  unsuffixed default, as `format_account` writes it. */
function marketOf(account: string, venue: string): string {
  const suffix = account.slice(venue.length)
  if (suffix === '-spot') return 'spot'
  if (suffix === '-fut') return 'future'
  if (suffix === '-opt') return 'option'
  return 'perp'
}

function decimal(value: string | null | undefined): number | null {
  return value === null || value === undefined ? null : toNumber(value)
}

export function Treasury() {
  const balances = useQuery(() => api.balances(), [], 5_000)
  const record = useEquity((s) => s.record)
  const series = useEquity((s) => s.points)

  const model = useMemo(() => {
    if (balances.state !== 'ready') return null

    const groups = new Map<string, VenueGroup>()
    const totals = new Map<string, number>()
    const reporting: Record<string, number> = {}

    for (const account of balances.data as AccountBalances[]) {
      const group = groups.get(account.venue) ?? {
        venue: account.venue,
        rows: [],
        silent: [],
        unified: k_unified.has(account.venue),
      }
      if (account.reported_at === null || account.reported_at === undefined) {
        group.silent.push(account.account)
        groups.set(account.venue, group)
        continue
      }
      for (const asset of account.assets) {
        const total = decimal(asset.total)
        group.rows.push({
          account: account.account,
          venue: account.venue,
          market: marketOf(account.account, account.venue),
          asset: asset.asset,
          total,
          available: decimal(asset.available),
          unrealized: decimal(asset.unrealized),
          stale: account.stale,
        })
        if (total !== null) {
          totals.set(asset.asset, (totals.get(asset.asset) ?? 0) + total)
        }
      }
      // Keyed by ACCOUNT, not venue: two markets on one venue are two wallets
      // to carry forward separately, and the unified caveat above is about
      // adding them, not about tracking them.
      const denominated = account.assets.find((a) => a.asset === k_denomination)
      if (denominated !== undefined) {
        reporting[account.account] = toNumber(denominated.total)
      }
      groups.set(account.venue, group)
    }

    return {
      groups: [...groups.values()].sort((a, b) => a.venue.localeCompare(b.venue)),
      totals: [...totals.entries()].sort((a, b) => b[1] - a[1]),
      reporting,
      denominated: totals.get(k_denomination) ?? 0,
      accounts: (balances.data as AccountBalances[]).length,
      answering: (balances.data as AccountBalances[]).filter(
        (a) => a.reported_at !== null && a.reported_at !== undefined,
      ).length,
    }
  }, [balances])

  // Recorded from the poll, not from the render: a component that re-rendered
  // for its own reasons would otherwise stamp a new sample carrying no news.
  const stamp = balances.state === 'ready' ? balances.data : null
  useEffect(() => {
    if (model === null || stamp === null) return
    record(Date.now(), model.reporting)
  }, [stamp, model, record])

  if (balances.state === 'loading') return <Loading what="balances" />
  if (balances.state === 'failed') return <Failed {...balances} />
  if (model === null) return null

  const x = series.map((p) => p.t)
  const values = series.map((p) => p.value)

  return (
    <div className="flex h-full flex-col gap-4 overflow-auto p-5">
      <ChartFrame
        title={`${k_denomination} across venues`}
        note="Only one asset is summed — adding it to another coin would be a number nobody holds. Each venue's last figure is carried forward, so an account that has not answered this round holds its value instead of dropping the line to a floor that never happened."
        height={260}
        readout={
          <>
            <div className="tnum text-2xl text-ink">{usd(model.denominated)}</div>
            <div className="text-[11px] text-faint">
              {model.answering} of {model.accounts} accounts answering
            </div>
          </>
        }
        placeholder={
          series.length < 2
            ? 'Collecting — nothing persists a balance, so this series starts when the page opens.'
            : undefined
        }
      >
        {({ width, height }) => (
          <LinePlot
            width={width}
            height={height}
            x={x}
            format={compact}
            baseline="data"
            series={[
              {
                key: 'equity',
                label: k_denomination,
                color: colorFor('equity'),
                values,
                area: true,
              },
            ]}
          />
        )}
      </ChartFrame>

      <section className="overflow-hidden rounded-xl border border-line bg-surface/80">
        <header className="flex items-center justify-between border-b border-line px-4 py-2.5">
          <h2 className="text-[11px] font-medium uppercase tracking-[0.14em] text-muted">
            Balances by exchange
          </h2>
          <span className="text-[11px] text-faint">live from the gateway segments</span>
        </header>

        <table className="w-full text-sm">
          <thead className="text-left text-[10px] uppercase tracking-[0.12em] text-faint">
            <tr className="border-b border-line">
              <th className="px-4 py-2 font-normal">Venue</th>
              <th className="px-3 py-2 font-normal">Market</th>
              <th className="px-3 py-2 font-normal">Asset</th>
              <th className="px-3 py-2 text-right font-normal">Total</th>
              <th className="px-3 py-2 text-right font-normal">Available</th>
              <th className="px-4 py-2 text-right font-normal">Unrealized</th>
            </tr>
          </thead>
          <tbody>
            {model.groups.map((group) => (
              <VenueRows key={group.venue} group={group} />
            ))}
            {model.groups.length === 0 && (
              <tr>
                <td colSpan={6} className="px-4 py-6 text-center text-sm text-muted">
                  No gateway is attached. Start one and it appears here without a restart.
                </td>
              </tr>
            )}
          </tbody>
          <tfoot>
            {model.totals.map(([asset, total]) => (
              <tr key={asset} className="border-t border-line bg-raised/40">
                <td colSpan={3} className="px-4 py-2 text-[10px] uppercase tracking-[0.12em] text-faint">
                  Total across venues
                </td>
                <td className="tnum px-3 py-2 text-right font-medium text-ink">
                  {usd(total)}
                </td>
                <td className="px-3 py-2 text-right text-xs text-faint">{asset}</td>
                <td className="px-4 py-2" />
              </tr>
            ))}
          </tfoot>
        </table>
      </section>
    </div>
  )
}

function VenueRows({ group }: { group: VenueGroup }) {
  const markets = new Set(group.rows.map((r) => r.market))
  const doubleCounted = group.unified && markets.size > 1

  return (
    <>
      {group.rows.map((row, index) => (
        <tr key={`${row.account}:${row.asset}`} className="border-t border-line/50">
          <td className="px-4 py-1.5">
            {index === 0 && (
              <span className="flex items-center gap-2">
                <span
                  className="size-2 rounded-full"
                  style={{ backgroundColor: colorFor(row.venue) }}
                />
                {row.venue}
              </span>
            )}
          </td>
          <td className="px-3 py-1.5 text-muted">{row.market}</td>
          <td className="px-3 py-1.5">{row.asset}</td>
          <Figure value={row.total} asset={row.asset} strong />
          <Figure value={row.available} asset={row.asset} />
          <Figure value={row.unrealized} asset={row.asset} last />
          {row.stale && (
            <td className="px-2 text-[10px] uppercase text-warn" title="older than three pulls">
              stale
            </td>
          )}
        </tr>
      ))}

      {doubleCounted && (
        <Note venue={group.venue}>
          reports one wallet for both markets — the rows above are the same money seen twice, and
          the total counts it once
        </Note>
      )}
      {group.silent.length > 0 && (
        <Note venue={group.silent.join(', ')}>
          has not answered. That is not an empty account, and the total below is a floor rather
          than a sum
        </Note>
      )}
    </>
  )
}

/** A figure the venue did not report renders as a DASH, never as zero: several
 *  venues publish a total and no free figure, and a zero there reads as "none
 *  of it may be committed", which is the opposite of what silence means. */
function Figure({
  value,
  asset,
  strong,
  last,
}: {
  value: number | null
  asset: string
  strong?: boolean
  last?: boolean
}) {
  return (
    <td
      className={cn(
        'tnum py-1.5 text-right',
        last === true ? 'px-4' : 'px-3',
        value === null ? 'text-faint' : strong === true ? 'text-ink' : 'text-muted',
      )}
      title={value === null ? 'the venue did not report this' : undefined}
    >
      {value === null ? '—' : quantity(value, asset, k_denomination)}
    </td>
  )
}

function Note({ venue, children }: { venue: string; children: React.ReactNode }) {
  return (
    <tr className="border-t border-line/30">
      <td colSpan={6} className="px-4 py-1.5 text-[11px] leading-relaxed text-faint">
        <span className="text-muted">{venue}</span> {children}.
      </td>
    </tr>
  )
}

