import { useEffect, useState, type ReactNode } from 'react'
import { cn } from '@raikiri/ui'
import { BookWidget } from './book'
import { MarketBar } from './header'
import { MarketWidget } from './market-tabs'
import { TerminalProvider, useTerminal } from './state'
import { Tabs } from './tabs'
import { TradesWidget } from './trades'

export interface AccountTab {
  id: string
  label: string
  render: () => ReactNode
}

/** what a desk built on the terminal adds: the public terminal reads markets, order entry and the account are the
    private desk's */
export interface DeskSlots {
  /** the column right of the book */
  ticket?: ReactNode
  /** the block under the chart and the book, one tab each */
  account?: readonly AccountTab[]
}

const k_block = 'min-w-0 overflow-hidden rounded-md bg-panel'

type Side = 'book' | 'trades'

const k_side: ReadonlyArray<readonly [Side, string]> = [
  ['book', 'Order book'],
  ['trades', 'Trades'],
]

function BookBlock() {
  const { feeds } = useTerminal()
  const [picked, setTab] = useState<Side>('book')
  // a venue that prints no trades has no tape to open
  const tabs = feeds.trades ? k_side : k_side.filter(([s]) => s !== 'trades')
  const tab = feeds.trades ? picked : 'book'
  return (
    <div className="flex h-full min-h-0 flex-col">
      <Tabs tabs={tabs} value={tab} onChange={setTab} />
      <div className="min-h-0 flex-1">{tab === 'book' ? <BookWidget /> : <TradesWidget />}</div>
    </div>
  )
}

function AccountBlock({ tabs }: { tabs: readonly AccountTab[] }) {
  const [open, setOpen] = useState(tabs[0]?.id)
  const tab = tabs.find((t) => t.id === open) ?? tabs[0]
  if (tab === undefined) return null
  return (
    <div className="flex h-full min-h-0 flex-col">
      <Tabs tabs={tabs.map((t) => [t.id, t.label] as const)} value={tab.id} onChange={setOpen} />
      <div className="flex min-h-0 flex-1 flex-col">{tab.render()}</div>
    </div>
  )
}

function UtcClock() {
  const [now, setNow] = useState(() => new Date())
  useEffect(() => {
    const t = setInterval(() => setNow(new Date()), 1_000)
    return () => clearInterval(t)
  }, [])
  return <span className="mono text-fg-mid">{now.toISOString().slice(11, 19)} UTC</span>
}

function Desk({ ticket, account }: DeskSlots) {
  const { connected } = useTerminal()
  const desk = ticket !== undefined && ticket !== null
  const held = account !== undefined && account.length > 0
  return (
    <div className="flex h-full min-h-0 flex-col">
      <MarketBar />
      <div className="min-h-0 flex-1 overflow-y-auto [scrollbar-width:thin]">
        {/* in source order a narrow screen stacks chart, ticket, book, account; a wide one places each by hand */}
        <div
          className={cn(
            'grid grid-cols-1 gap-1 p-1',
            desk
              ? 'min-[1100px]:grid-cols-[minmax(0,1fr)_380px_320px]'
              : 'min-[1100px]:grid-cols-[minmax(0,1fr)_380px]',
            // the second row takes what a ticket taller than the chart leaves over
            held && 'min-[1100px]:grid-rows-[auto_minmax(260px,1fr)]',
          )}
        >
          <section className={cn(k_block, 'h-[60vh] min-h-[480px] min-[1100px]:col-start-1 min-[1100px]:row-start-1')}>
            <MarketWidget />
          </section>
          {desk && (
            <section
              className={cn(k_block, 'min-[1100px]:col-start-3 min-[1100px]:row-start-1', held && 'min-[1100px]:row-span-2')}
            >
              {ticket}
            </section>
          )}
          <section className={cn(k_block, 'h-[60vh] min-h-[480px] min-[1100px]:col-start-2 min-[1100px]:row-start-1')}>
            <BookBlock />
          </section>
          {held && (
            <section
              className={cn(k_block, 'min-h-[260px] min-[1100px]:col-span-2 min-[1100px]:col-start-1 min-[1100px]:row-start-2')}
            >
              <AccountBlock tabs={account} />
            </section>
          )}
        </div>
      </div>
      <footer className="flex h-6 shrink-0 items-center justify-between border-t border-line px-3 text-2xs">
        <span className={cn('flex items-center gap-1.5', connected ? 'text-up' : 'text-amber')}>
          <span className={cn('size-1.5 rounded-full', connected ? 'pulse-dot bg-up' : 'bg-amber')} />
          {connected ? 'Live stream' : 'Reconnecting'}
        </span>
        <UtcClock />
      </footer>
    </div>
  )
}

/** the terminal: the market bar over the chart and the book, with a desk's ticket and account in their places */
export function TerminalDesk(slots: DeskSlots) {
  return (
    <TerminalProvider>
      <Desk {...slots} />
    </TerminalProvider>
  )
}

export function TerminalPage() {
  return <TerminalDesk />
}
