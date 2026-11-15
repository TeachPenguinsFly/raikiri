import { useEffect, useState } from 'react'
import { group, split, Workspace, type Preset, type Widget } from '@raikiri/ui/v3'
import { BookWidget } from './book'
import { ChartWidget } from './chart'
import { PositionsWidget, VenuesWidget, WatchlistWidget } from './panels'
import { TerminalProvider, useTerminal } from './state'
import { TicketWidget } from './ticket'

const k_widgets: Record<string, Widget> = {
  markets: { title: 'Markets', icon: 'search', render: () => <WatchlistWidget /> },
  chart: { title: 'Chart', icon: 'chart', render: () => <ChartWidget /> },
  book: { title: 'Book', icon: 'grip', render: () => <BookWidget /> },
  venues: { title: 'Venues', icon: 'layers', render: () => <VenuesWidget /> },
  ticket: { title: 'Execution', icon: 'trade', render: () => <TicketWidget /> },
  positions: { title: 'Positions', icon: 'star', render: () => <PositionsWidget /> },
}

const k_presets: Record<'simple' | 'advanced' | 'focus', Preset> = {
  simple: {
    label: 'Simple',
    build: () =>
      split(
        'row',
        [
          group(['markets']),
          split('col', [group(['chart']), split('row', [group(['venues']), group(['positions'])], [55, 45])], [68, 32]),
          group(['book']),
          group(['ticket']),
        ],
        [14, 48, 20, 18],
      ),
  },
  advanced: {
    label: 'Advanced',
    build: () =>
      split(
        'row',
        [
          split('col', [group(['markets']), group(['positions'])], [64, 36]),
          split('col', [group(['chart']), group(['venues'])], [70, 30]),
          group(['book']),
          group(['ticket']),
        ],
        [15, 43, 21, 21],
      ),
  },
  focus: {
    label: 'Chart focus',
    build: () =>
      split(
        'row',
        [
          split('col', [group(['chart']), group(['venues', 'positions', 'markets'])], [76, 24]),
          split('col', [group(['book']), group(['ticket'])], [55, 45]),
        ],
        [72, 28],
      ),
  },
}

function UtcClock() {
  const [now, setNow] = useState(() => new Date())
  useEffect(() => {
    const t = setInterval(() => setNow(new Date()), 1_000)
    return () => clearInterval(t)
  }, [])
  return <span className="mono text-fg-mid">{now.toISOString().slice(11, 19)} UTC</span>
}

function Desk() {
  const { connected } = useTerminal()
  return (
    <Workspace
      widgets={k_widgets}
      presets={k_presets}
      fallback="simple"
      storageKey="crypto.terminal.layout"
      connected={connected}
      clock={<UtcClock />}
      stacked={['markets', 'chart', 'book', 'ticket', 'venues', 'positions']}
    />
  )
}

export function TerminalPage() {
  return (
    <TerminalProvider>
      <Desk />
    </TerminalProvider>
  )
}
