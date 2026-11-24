import { useEffect, useRef } from 'react'

export type MarketTab = 'chart' | 'info' | 'data'

const k_listeners = new Set<(tab: MarketTab) => void>()

/** asks the chart block to show a tab, from anywhere on the page */
export function requestMarketTab(tab: MarketTab) {
  for (const listener of [...k_listeners]) listener(tab)
}

export function useMarketTabRequest(onRequest: (tab: MarketTab) => void) {
  const latest = useRef(onRequest)
  latest.current = onRequest
  useEffect(() => {
    const listener = (tab: MarketTab) => latest.current(tab)
    k_listeners.add(listener)
    return () => {
      k_listeners.delete(listener)
    }
  }, [])
}
