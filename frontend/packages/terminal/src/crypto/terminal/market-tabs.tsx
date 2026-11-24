import { useLayoutEffect, useSyncExternalStore } from 'react'
import { cn } from '@raikiri/ui'
import { Seg } from '@raikiri/ui/v3'
import { ChartWidget } from './chart'
import { DataTab } from './data'
import { InfoTab } from './info'

type Tab = 'chart' | 'info' | 'data'

const k_tabs: ReadonlyArray<readonly [Tab, string]> = [
  ['chart', 'Chart'],
  ['info', 'Info'],
  ['data', 'Data'],
]

// the switch sits in the dock's tab bar and the pages in the widget's body, two places the dock renders apart, so the
// choice lives here; the count tells the body whether a bar is showing the switch for it
let k_tab: Tab = 'chart'
let k_bars = 0
const k_watchers = new Set<() => void>()
const changed = () => k_watchers.forEach((w) => w())
const watch = (w: () => void) => {
  k_watchers.add(w)
  return () => k_watchers.delete(w)
}

function Switch() {
  const tab = useSyncExternalStore(watch, () => k_tab)
  return (
    <Seg
      size="xs"
      value={tab}
      onChange={(t) => {
        k_tab = t
        changed()
      }}
      options={k_tabs}
    />
  )
}

/** the chart widget's pages, for the widget's `tools` */
export function MarketTabs() {
  useLayoutEffect(() => {
    k_bars++
    changed()
    return () => {
      k_bars--
      changed()
    }
  }, [])
  return <Switch />
}

/** the chart with the coin's facts and the contract's history behind tabs */
export function MarketWidget() {
  const tab = useSyncExternalStore(watch, () => k_tab)
  const barred = useSyncExternalStore(watch, () => k_bars > 0)
  return (
    <div className="flex h-full min-h-0 flex-col">
      {!barred && (
        <div className="flex shrink-0 px-2 pt-1.5">
          <Switch />
        </div>
      )}
      <div className="relative min-h-0 flex-1">
        {/* the chart stays mounted and sized under the other pages: it keeps its interval and zoom, and a canvas
            resized from nothing redraws badly */}
        <div className={cn('absolute inset-0', tab !== 'chart' && 'invisible')}>
          <ChartWidget />
        </div>
        {tab !== 'chart' && <div className="absolute inset-0 bg-panel">{tab === 'info' ? <InfoTab /> : <DataTab />}</div>}
      </div>
    </div>
  )
}
