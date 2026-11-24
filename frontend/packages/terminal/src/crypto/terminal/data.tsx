import { useMemo, useState } from 'react'
import { compact, Empty, n, Seg } from '@raikiri/ui/v3'
import { api, type CandlePage, type MetricPoint } from '../api/client'
import { px, useQuery, type Leg } from '../lib/market'
import { MiniChart, type MiniSeries } from './mini-chart'
import { useTerminal } from './state'

type Interval = '5m' | '15m' | '1h' | '4h' | '1d'
const k_interval_s: Record<Interval, number> = { '5m': 300, '15m': 900, '1h': 3_600, '4h': 14_400, '1d': 86_400 }
const k_limit = 500
const k_refresh_ms = 60_000
const k_funding_s = 28_800

type Points = Array<readonly [number, number]>
type Panel = { title: string; series: MiniSeries[] }

const pct = (places: number) => (v: number) => `${v.toFixed(places)}%`

function panelsOf(
  points: readonly MetricPoint[],
  candles: CandlePage['candles'],
  leg: Leg,
  bucket: number,
  fundingSeconds: number,
): Panel[] {
  const rate: Points = []
  const paid: Points = []
  const interest: Points = []
  const basis: Points = []
  const marks: Points = []
  const indexes: Points = []
  let sum = 0
  let lastMark: number | undefined
  let lastT = -Infinity
  let inDollars = true
  for (const p of [...points].sort((a, b) => a.t - b.t)) {
    const t = Math.floor(p.t / 1000)
    if (t <= lastT) continue
    lastT = t
    const mark = n(p.mark)
    const index = n(p.index)
    const r = n(p.funding_rate)
    const oi = n(p.open_interest)
    if (mark !== undefined) {
      lastMark = mark
      marks.push([t, mark])
    }
    if (index !== undefined) indexes.push([t, index])
    if (mark !== undefined && index !== undefined && index > 0) basis.push([t, ((mark - index) / index) * 1e4])
    if (r !== undefined) {
      // the rate is per funding interval, so a bucket accrues only its share of it
      sum += r * 100 * (bucket / fundingSeconds)
      rate.push([t, r * 100])
      paid.push([t, sum])
    }
    if (oi !== undefined) {
      if (lastMark === undefined) inDollars = false
      interest.push([t, oi * (lastMark ?? 1)])
    }
  }
  // without a mark in every bucket the line stays in the base asset rather than mixing the two
  if (!inDollars) {
    interest.length = 0
    for (const p of points) {
      const oi = n(p.open_interest)
      if (oi !== undefined) interest.push([Math.floor(p.t / 1000), oi])
    }
    interest.sort((a, b) => a[0] - b[0])
  }
  const from = interest[0]?.[0] ?? Infinity
  const volume: Points = candles.flatMap((c) => {
    const v = n(c.v)
    const close = n(c.c)
    return v === undefined || close === undefined || c.t < from ? [] : [[c.t, v * leg.unit * close] as const]
  })
  const price = (v: number) => px(v, leg.places)
  const out: Panel[] = []
  if (rate.length > 0)
    out.push({
      title: 'Funding rate',
      series: [
        { name: 'Rate', kind: 'bars', tone: (v) => (v >= 0 ? 'up' : 'down'), points: rate, format: pct(4) },
        { name: 'Cumulative', kind: 'line', tone: 'amber', points: paid, format: pct(3), scale: 'left' },
      ],
    })
  if (interest.length > 0)
    out.push({
      title: 'Open interest',
      series: [
        {
          name: 'Open interest',
          kind: 'line',
          tone: 'accent',
          points: interest,
          format: (v) => compact(v, inDollars),
        },
        ...(volume.length > 0
          ? [
              {
                name: 'Volume',
                kind: 'bars',
                tone: 'fg-mid',
                points: volume,
                format: (v: number) => compact(v, true),
                scale: 'under',
              } satisfies MiniSeries,
            ]
          : []),
      ],
    })
  if (basis.length > 0)
    out.push({
      title: 'Basis',
      series: [
        {
          name: 'Mark − index',
          kind: 'bars',
          tone: (v) => (v >= 0 ? 'up' : 'down'),
          points: basis,
          format: (v) => `${v.toFixed(2)} bp`,
        },
      ],
    })
  if (marks.length > 0 || indexes.length > 0)
    out.push({
      title: 'Mark and index',
      series: [
        { name: 'Mark', kind: 'line', tone: 'accent', points: marks, format: price },
        { name: 'Index', kind: 'line', tone: 'amber', points: indexes, format: price },
      ].filter((s) => s.points.length > 0) as MiniSeries[],
    })
  return out
}

function Panels({ leg, interval }: { leg: Leg; interval: Interval }) {
  const { funding } = useTerminal()
  const history = useQuery(
    `metrics:${leg.id}:${interval}`,
    () =>
      Promise.all([
        api.metricsHistory(leg.id, interval, k_limit),
        // the volume bars are an extra: the panels draw without them
        api.candles(leg.id, interval, k_limit).then(
          (page) => page.candles,
          () => [],
        ),
      ]),
    k_refresh_ms,
  )
  const fundingSeconds = funding.get(leg.id)?.interval_seconds ?? k_funding_s
  const data = history.state === 'ready' ? history.data : undefined
  const panels = useMemo(
    () => (data === undefined ? [] : panelsOf(data[0].points, data[1], leg, k_interval_s[interval], fundingSeconds)),
    [data, leg, interval, fundingSeconds],
  )
  if (history.state === 'loading') return <div className="px-4 py-8 text-center text-sm text-fg-dim">Loading</div>
  if (panels.length === 0) return <Empty title="No history yet" />
  return (
    <div className="grid grid-cols-[repeat(auto-fit,minmax(min(100%,24rem),1fr))] gap-2">
      {panels.map((p, i) => (
        <MiniChart key={p.title} title={p.title} series={p.series} logo={i === 0} />
      ))}
    </div>
  )
}

/** how the chosen leg's funding, open interest and basis moved */
export function DataTab() {
  const { leg } = useTerminal()
  const [interval, setSpan] = useState<Interval>('1h')
  if (leg === undefined) return <Empty title="Pick a market" />
  return (
    <div className="flex h-full min-h-0 flex-col">
      <div className="flex shrink-0 items-center px-2 py-1.5">
        <Seg
          size="xs"
          value={interval}
          onChange={setSpan}
          options={(Object.keys(k_interval_s) as Interval[]).map((i) => [i, i] as const)}
        />
      </div>
      <div className="min-h-0 flex-1 overflow-y-auto px-2 pb-2">
        <Panels key={`${leg.id}:${interval}`} leg={leg} interval={interval} />
      </div>
    </div>
  )
}
