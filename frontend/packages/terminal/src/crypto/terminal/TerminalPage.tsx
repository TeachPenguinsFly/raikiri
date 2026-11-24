import { useEffect, useState } from 'react'
import { group, split, Workspace, type Preset, type Widget } from '@raikiri/ui/v3'
import { BookWidget } from './book'
import { MarketBar } from './header'
import { MarketTabs, MarketWidget } from './market-tabs'
import { VenuesWidget } from './panels'
import { TerminalProvider, useTerminal } from './state'

// the public terminal reads markets; order entry is the private desk's, which adds its widgets to these
export const terminalWidgets: Record<string, Widget> = {
  chart: { title: 'Chart', icon: 'chart', render: () => <MarketWidget />, tools: () => <MarketTabs /> },
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

/** what a desk built on the terminal changes: its widgets and layouts, and where it keeps the layout */
export interface DeskOptions {
  widgets?: Record<string, Widget>
  presets?: Record<string, Preset>
  fallback?: string
  storageKey?: string
  /** the widgets a narrow screen shows one under another, in order */
  stacked?: string[]
}

function Desk({
  widgets = terminalWidgets,
  presets = k_presets,
  fallback = 'standard',
  storageKey = 'crypto.terminal.layout.v2',
  stacked = ['chart', 'book', 'venues'],
}: DeskOptions) {
  const { connected } = useTerminal()
  return (
    <div className="flex h-full min-h-0 flex-col">
      <MarketBar />
      <div className="min-h-0 flex-1">
        <Workspace
          widgets={widgets}
          presets={presets}
          fallback={fallback}
          storageKey={storageKey}
          connected={connected}
          clock={<UtcClock />}
          stacked={stacked}
        />
      </div>
    </div>
  )
}

/** the terminal with the market bar over a workspace of widgets */
export function TerminalDesk(options: DeskOptions) {
  return (
    <TerminalProvider>
      <Desk {...options} />
    </TerminalProvider>
  )
}

export function TerminalPage() {
  return <TerminalDesk />
}
