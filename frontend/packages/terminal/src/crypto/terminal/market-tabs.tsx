import { useState } from 'react'
import { cn } from '@raikiri/ui'
import { ChartWidget } from './chart'
import { DataTab } from './data'
import { InfoTab } from './info'
import { useMarketTabRequest } from './market-tab-signal'
import { Tabs } from './tabs'

type Tab = 'chart' | 'info' | 'data'

const k_tabs: ReadonlyArray<readonly [Tab, string]> = [
  ['chart', 'Chart'],
  ['info', 'Info'],
  ['data', 'Data'],
]

/** the chart with the coin's facts and the contract's history behind tabs */
export function MarketWidget() {
  const [tab, setTab] = useState<Tab>('chart')
  useMarketTabRequest(setTab)
  return (
    <div className="flex h-full min-h-0 flex-col">
      <Tabs tabs={k_tabs} value={tab} onChange={setTab} />
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
