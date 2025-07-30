import { cn } from '@/lib/utils'
import asterIcon from '@/assets/exchanges/aster.png'
import backpackIcon from '@/assets/exchanges/backpack.png'
import bingxIcon from '@/assets/exchanges/bingx.png'
import bitfinexIcon from '@/assets/exchanges/bitfinex.png'
import bitgetIcon from '@/assets/exchanges/bitget.png'
import bitmartIcon from '@/assets/exchanges/bitmart.png'
import bitmexIcon from '@/assets/exchanges/bitmex.png'
import btseIcon from '@/assets/exchanges/btse.png'
import coinbaseIcon from '@/assets/exchanges/coinbase.png'
import coinexIcon from '@/assets/exchanges/coinex.png'
import cryptocomIcon from '@/assets/exchanges/cryptocom.png'
import dydxIcon from '@/assets/exchanges/dydx.png'
import grvtIcon from '@/assets/exchanges/grvt.png'
import htxIcon from '@/assets/exchanges/htx.png'
import krakenIcon from '@/assets/exchanges/kraken.png'
import kucoinIcon from '@/assets/exchanges/kucoin.png'
import lighterIcon from '@/assets/exchanges/lighter.png'
import mexcIcon from '@/assets/exchanges/mexc.png'
import phemexIcon from '@/assets/exchanges/phemex.png'
import weexIcon from '@/assets/exchanges/weex.png'
import xtIcon from '@/assets/exchanges/xt.png'

// Real favicons (fetched from each exchange) for venues without a hand-drawn SVG mark.
const FAVICONS: Record<string, string> = {
  aster: asterIcon,
  backpack: backpackIcon,
  bingx: bingxIcon,
  bitfinex: bitfinexIcon,
  bitget: bitgetIcon,
  bitmart: bitmartIcon,
  bitmex: bitmexIcon,
  btse: btseIcon,
  coinbase: coinbaseIcon,
  coinex: coinexIcon,
  cryptocom: cryptocomIcon,
  dydx: dydxIcon,
  grvt: grvtIcon,
  htx: htxIcon,
  kraken: krakenIcon,
  kucoin: kucoinIcon,
  lighter: lighterIcon,
  mexc: mexcIcon,
  phemex: phemexIcon,
  weex: weexIcon,
  xt: xtIcon,
}

/** The venue's own mark. A hand-drawn SVG where one was worth drawing, the
 *  venue's favicon where it was not, and the first two letters where neither
 *  exists — a row identified only by a colour swatch is a row you have to read
 *  twice. */
export function ExchangeIcon({ venue, className }: { venue: string; className?: string }) {
  const c = cn('inline-block shrink-0', className)
  const favicon = FAVICONS[venue]
  if (favicon) {
    return <img src={favicon} alt={venue} className={cn(c, 'rounded-sm object-cover')} />
  }
  switch (venue) {
    case 'binance':
      return (
        <svg viewBox="0 0 24 24" className={c} role="img" aria-label="Binance">
          <rect width="24" height="24" rx="5" fill="#F0B90B" />
          {/* official Binance mark: 4 chevrons + centre diamond */}
          <g fill="#181A20" transform="translate(4 4) scale(0.1264)">
            <path d="M38.73 53.2 63.32 28.62 87.92 53.22 102.22 38.91 63.32 0 24.42 38.9Z" />
            <path d="M0 63.31 14.3 49 28.61 63.31 14.3 77.61Z" />
            <path d="M38.73 73.42 63.32 98.01 87.92 73.41 102.23 87.71 63.33 126.62 24.42 87.72 24.4 87.7Z" />
            <path d="M97.99 63.31 112.29 49 126.6 63.3 112.3 77.62Z" />
            <path d="M77.83 63.3 63.32 48.78 52.59 59.51 51.36 60.75 48.81 63.29 48.79 63.31 48.81 63.33 63.32 77.83Z" />
          </g>
        </svg>
      )
    case 'okx':
      return (
        <svg viewBox="0 0 24 24" className={c} role="img" aria-label="OKX">
          <rect width="24" height="24" rx="5" fill="#0B0E13" />
          <g fill="#fff">
            <rect x="4.8" y="4.8" width="4.3" height="4.3" rx="0.7" />
            <rect x="14.9" y="4.8" width="4.3" height="4.3" rx="0.7" />
            <rect x="9.85" y="9.85" width="4.3" height="4.3" rx="0.7" />
            <rect x="4.8" y="14.9" width="4.3" height="4.3" rx="0.7" />
            <rect x="14.9" y="14.9" width="4.3" height="4.3" rx="0.7" />
          </g>
        </svg>
      )
    case 'bybit':
      return (
        <svg viewBox="0 0 24 24" className={c} role="img" aria-label="Bybit">
          <rect width="24" height="24" rx="5" fill="#1E2433" />
          {/* official Bybit app-icon: BYBIT wordmark, white with a gold "I" */}
          <g transform="translate(12 12) scale(1.15) translate(-12 -12)">
            <path fill="#fff" d="M4.993 15H3v-4.626h1.913c.93 0 1.471.507 1.471 1.3c0 .513-.348.845-.588.955c.287.13.655.423.655 1.04c0 .863-.609 1.33-1.458 1.33m-.154-3.82h-.91v1.065h.91c.395 0 .615-.214.615-.533c0-.317-.22-.532-.615-.532m.06 1.877h-.97v1.137h.97c.42 0 .622-.259.622-.571s-.201-.565-.622-.565zm4.388.046V15h-.923v-1.898l-1.431-2.728h1.01l.889 1.864l.877-1.864h1.01zM13.355 15h-1.993v-4.626h1.913c.93 0 1.47.507 1.47 1.3c0 .513-.347.845-.588.955c.287.13.655.423.655 1.04c0 .863-.608 1.33-1.457 1.33m-.155-3.82h-.91v1.065h.91c.395 0 .616-.214.616-.533c0-.317-.22-.532-.616-.532m.06 1.877h-.97v1.137h.97c.422 0 .622-.259.622-.571s-.2-.565-.622-.565zm6.495-1.876V15h-.929v-3.82h-1.245v-.806H21v.806z" />
            <path fill="#F6A500" d="M15.829 13.626V9h.93v4.626z" />
          </g>
        </svg>
      )
    case 'gate':
      return (
        <svg viewBox="0 0 24 24" className={c} role="img" aria-label="Gate">
          <rect width="24" height="24" rx="5" fill="#0B0E13" />
          {/* official Gate mark: blue gateway ring + teal square */}
          <g transform="translate(12 12) scale(0.82) translate(-12 -12)">
            <path fill="#2354E6" d="M12 16.95a4.95 4.95 0 1 1 0-9.9V3a9 9 0 1 0 9 9h-4.05A4.95 4.95 0 0 1 12 16.95" />
            <path fill="#17E6A1" d="M16.95 7.05H12V12h4.95z" />
          </g>
        </svg>
      )
    case 'hyperliquid':
      return (
        <svg viewBox="0 0 144 144" className={c} role="img" aria-label="Hyperliquid">
          <rect width="144" height="144" rx="30" fill="#0a1a17" />
          {/* official Hyperliquid mint logomark, inset on a dark teal tile */}
          <path
            transform="translate(14.4 14.4) scale(0.8)"
            fill="#97FCE4"
            d="M144 71.6991C144 119.306 114.866 134.582 99.5156 120.98C86.8804 109.889 83.1211 86.4521 64.116 84.0456C39.9942 81.0113 37.9057 113.133 22.0334 113.133C3.5504 113.133 0 86.2428 0 72.4315C0 58.3063 3.96809 39.0542 19.736 39.0542C38.1146 39.0542 39.1588 66.5722 62.132 65.1073C85.0007 63.5379 85.4184 34.8689 100.247 22.6271C113.195 12.0593 144 23.4641 144 71.6991Z"
          />
        </svg>
      )
    case 'deribit':
      return (
        <svg viewBox="0 0 24 24" className={c} role="img" aria-label="Deribit">
          <rect width="24" height="24" rx="5" fill="#0c1226" />
          {/* teal "d" mark with the brand slice */}
          <g fill="#05c0a5">
            <path d="M14.5 4.5h3v15h-3z" />
            <path d="M14.5 8.5a5.5 5.5 0 1 0 0 11v-3a2.5 2.5 0 1 1 0-5z" />
          </g>
        </svg>
      )
    default: {
      // Generic monogram chip for the long tail of venues (coinbase, mexc, kucoin, …).
      const palette: Record<string, string> = {
        kraken: '#5741d9', kucoin: '#23af91', bitget: '#00f0ff', bingx: '#2f7bff', mexc: '#1972f5',
        coinbase: '#0052ff', htx: '#1d72f3', coinex: '#15b08e', bitmart: '#15c0e6', cryptocom: '#103f9e',
        deribit: '#15b079', dydx: '#6966ff', lighter: '#9aa0a6', bitfinex: '#16b157', weex: '#f5b500', phemex: '#2d5af0', bitmex: '#e0413d', backpack: '#e44d2e',
      }
      const fill = palette[venue] ?? '#475569'
      return (
        <svg viewBox="0 0 24 24" className={c} role="img" aria-label={venue}>
          <rect width="24" height="24" rx="5" fill={fill} />
          <text x="12" y="16" textAnchor="middle" fontSize="11" fontWeight="700" fill="#fff" fontFamily="system-ui">
            {venue.slice(0, 2).toUpperCase()}
          </text>
        </svg>
      )
    }
  }
}
