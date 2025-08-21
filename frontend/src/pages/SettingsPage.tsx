import { PanelHeader } from '@/components/shared/PanelHeader'
import { Venue } from '@/components/shared/Venue'
import { VENUES } from '@/mock/data'

/** Which markets a venue's key actually opens.
 *
 *  The screen used to report a state, an instrument count and a segment path —
 *  three facts about a process, none of which is the question anyone opens
 *  Settings to ask. That question is: can I trade this venue, and on which
 *  markets. A key is per venue and the permissions are per market, so the grid
 *  is venue by market and nothing else. */
const MARKETS = ['spot', 'perp'] as const
type Market = (typeof MARKETS)[number]

/** `null` where the venue has no key at all — which is a different fact from a
 *  key that is present and refused, and the two get different words. */
const KEYS: Record<string, Partial<Record<Market, boolean>> | null> = {
  bybit: { spot: true, perp: true },
  binance: { spot: false, perp: true },
  okx: { spot: true, perp: true },
  gate: { spot: false, perp: true },
  kucoin: { spot: true, perp: true },
  bitget: null,
  xt: { spot: false, perp: true },
  bingx: null,
}

const SOURCES: Record<string, string> = {
  bybit: 'env:RAIKIRI_BYBIT_KEY',
  binance: 'env:RAIKIRI_BINANCE_KEY',
  okx: 'env:RAIKIRI_OKX_KEY',
  gate: 'env:RAIKIRI_GATE_KEY',
  kucoin: 'env:RAIKIRI_KUCOIN_KEY',
  xt: 'env:RAIKIRI_XT_KEY',
}

export function SettingsPage() {
  const working = VENUES.filter((v) => KEYS[v] !== null && KEYS[v] !== undefined).length

  return (
    <div className="flex h-full min-h-0 flex-col gap-2">
      <section className="panel flex min-h-0 flex-1 flex-col">
        <PanelHeader
          title="API keys"
          right={
            <span className="meta">
              {working}/{VENUES.length} venues keyed
            </span>
          }
        />
        <div className="min-h-0 flex-1 overflow-auto">
          <table className="w-full border-collapse text-sm whitespace-nowrap">
            <thead className="sticky top-0 z-10 bg-panel">
              <tr className="h-6 shadow-[inset_0_-1px_0_0_var(--color-line)]">
                <th className="col-head px-3 text-left">Venue</th>
                {MARKETS.map((m) => (
                  <th key={m} className="col-head w-24 px-3 text-left">
                    {m}
                  </th>
                ))}
                <th className="col-head px-3 text-left">Credential</th>
              </tr>
            </thead>
            <tbody>
              {VENUES.map((venue) => {
                const key = KEYS[venue]
                return (
                  <tr key={venue} className="row">
                    <td className="px-3">
                      <Venue venue={venue} />
                    </td>
                    {MARKETS.map((m) => (
                      <td key={m} className="px-3">
                        {key === null || key === undefined ? (
                          <span className="text-fg-dim">no key</span>
                        ) : key[m] === true ? (
                          <span className="text-up">ok</span>
                        ) : (
                          <span className="text-down">refused</span>
                        )}
                      </td>
                    ))}
                    <td className="px-3 text-xs text-fg-mid">
                      {SOURCES[venue] ?? <span className="text-amber">not configured</span>}
                    </td>
                  </tr>
                )
              })}
            </tbody>
          </table>
        </div>
      </section>
    </div>
  )
}
