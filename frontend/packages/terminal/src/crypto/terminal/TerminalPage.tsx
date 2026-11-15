import { useEffect, useState } from 'react'
import { group, split, Workspace, type Preset, type Widget } from '@raikiri/ui/v3'
import { BookWidget } from './book'
import { ChartWidget } from './chart'
import { MarketBar } from './header'
import { VenuesWidget } from './panels'
import { TerminalProvider, useTerminal } from './state'

// the public terminal reads markets; order entry is the private desk's
const k_widgets: Record<string, Widget> = {
  chart: { title: 'Chart', icon: 'chart', render: () => <ChartWidget /> },
  book: { title: 'Book', icon: 'grip', render: () => <BookWidget /> },
  venues: { title: 'Venues', icon: 'layers', render: () => <VenuesWidget /> },
}

const k_presets: Record<'standard' | 'focus', Preset> = {
  standard: {
    label: 'Standard',
    build: () =>
      split('row', [split('col', [group(['chart']), group(['venues'])], [70, 30]), group(['book'])], [76, 24]),
  },
  focus: {
    label: 'Chart focus',
    build: () => split('row', [group(['chart']), group(['book', 'venues'])], [80, 20]),
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
    <div className="flex h-full min-h-0 flex-col">
      <MarketBar />
      <div className="min-h-0 flex-1">
        <Workspace
          widgets={k_widgets}
          presets={k_presets}
          fallback="standard"
          storageKey="crypto.terminal.layout.v2"
          connected={connected}
          clock={<UtcClock />}
          stacked={['chart', 'book', 'venues']}
        />
      </div>
    </div>
  )
}

export function TerminalPage() {
  return (
    <TerminalProvider>
      <Desk />
    </TerminalProvider>
  )
}
