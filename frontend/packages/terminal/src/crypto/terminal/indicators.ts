import type { Candle } from "@raikiri/ui/v3";

// every function returns one value per input bar, null where the indicator has not seen enough bars yet; an input may
// itself start with nulls (a signal line over MACD), never hold one in the middle

export type Series = Array<number | null>;

const start = (values: readonly (number | null)[]) => {
  const i = values.findIndex((v) => v !== null);
  return i === -1 ? values.length : i;
};

export function sma(values: readonly (number | null)[], n: number): Series {
  const out: Series = new Array<number | null>(values.length).fill(null);
  const from = start(values);
  let sum = 0;
  for (let i = from; i < values.length; i++) {
    sum += values[i] ?? 0;
    if (i - n >= from) sum -= values[i - n] ?? 0;
    if (i - from >= n - 1) out[i] = sum / n;
  }
  return out;
}

/** seeded with the plain average of the first n values; alpha is 2/(n+1), or 1/n as Wilder smooths RSI and ATR */
function smoothed(
  values: readonly (number | null)[],
  n: number,
  alpha: number,
): Series {
  const out: Series = new Array<number | null>(values.length).fill(null);
  const from = start(values);
  let sum = 0;
  let prev = 0;
  for (let i = from; i < values.length; i++) {
    const v = values[i] ?? 0;
    if (i - from < n) {
      sum += v;
      if (i - from < n - 1) continue;
      prev = sum / n;
    } else prev += alpha * (v - prev);
    out[i] = prev;
  }
  return out;
}

export const ema = (values: readonly (number | null)[], n: number) =>
  smoothed(values, n, 2 / (n + 1));
const wilder = (values: readonly (number | null)[], n: number) =>
  smoothed(values, n, 1 / n);

export function bollinger(closes: readonly number[], n: number, k: number) {
  const mid = sma(closes, n);
  const upper: Series = [];
  const lower: Series = [];
  mid.forEach((m, i) => {
    if (m === null) {
      upper.push(null);
      lower.push(null);
      return;
    }
    let sq = 0;
    for (let j = i - n + 1; j <= i; j++) sq += ((closes[j] ?? m) - m) ** 2;
    const sd = Math.sqrt(sq / n);
    upper.push(m + k * sd);
    lower.push(m - k * sd);
  });
  return { mid, upper, lower };
}

/** the typical price weighted by the bar's ticks, starting over at 00:00 UTC */
export function vwap(bars: readonly Candle[]): Series {
  let day = -1;
  let pv = 0;
  let v = 0;
  return bars.map((b) => {
    const d = Math.floor(b.time / 86_400);
    if (d !== day) {
      day = d;
      pv = 0;
      v = 0;
    }
    pv += ((b.high + b.low + b.close) / 3) * b.ticks;
    v += b.ticks;
    return v > 0 ? pv / v : null;
  });
}

export function rsi(closes: readonly (number | null)[], n: number): Series {
  const gain: Series = [null];
  const loss: Series = [null];
  for (let i = 1; i < closes.length; i++) {
    const a = closes[i - 1];
    const b = closes[i];
    const d =
      a === null || a === undefined || b === null || b === undefined
        ? null
        : b - a;
    gain.push(d === null ? null : Math.max(d, 0));
    loss.push(d === null ? null : Math.max(-d, 0));
  }
  const up = wilder(gain, n);
  const down = wilder(loss, n);
  return up.map((u, i) => {
    const d = down[i];
    if (u === null || d === null || d === undefined) return null;
    return u + d === 0 ? 50 : (100 * u) / (u + d);
  });
}

export function macd(
  closes: readonly number[],
  fast: number,
  slow: number,
  signal: number,
) {
  const f = ema(closes, fast);
  const s = ema(closes, slow);
  const line = f.map((a, i) => {
    const b = s[i];
    return a === null || b === null || b === undefined ? null : a - b;
  });
  const sig = ema(line, signal);
  const hist = line.map((a, i) => {
    const b = sig[i];
    return a === null || b === null || b === undefined ? null : a - b;
  });
  return { line, signal: sig, hist };
}

/** where RSI sits in its own range over the last `stoch` bars, smoothed into %K and again into %D */
export function stochRsi(
  closes: readonly number[],
  rsiN: number,
  stoch: number,
  k: number,
  d: number,
) {
  const r = rsi(closes, rsiN);
  const raw = r.map((v, i) => {
    if (v === null || r[i - stoch + 1] === null || i < stoch - 1) return null;
    let lo = Infinity;
    let hi = -Infinity;
    for (let j = i - stoch + 1; j <= i; j++) {
      const x = r[j] ?? v;
      lo = Math.min(lo, x);
      hi = Math.max(hi, x);
    }
    return hi === lo ? 50 : (100 * (v - lo)) / (hi - lo);
  });
  const fast = sma(raw, k);
  return { k: fast, d: sma(fast, d) };
}

export function atr(bars: readonly Candle[], n: number): Series {
  const tr = bars.map((b, i) => {
    const prev = bars[i - 1]?.close;
    if (prev === undefined) return b.high - b.low;
    return Math.max(
      b.high - b.low,
      Math.abs(b.high - prev),
      Math.abs(b.low - prev),
    );
  });
  return wilder(tr, n);
}

/** each bar averaged with the one before it, which smooths a trend into runs of one colour */
export function heikinAshi(bars: readonly Candle[]): Candle[] {
  const out: Candle[] = [];
  for (const b of bars) {
    const prev = out.at(-1);
    const close = (b.open + b.high + b.low + b.close) / 4;
    const open =
      prev === undefined
        ? (b.open + b.close) / 2
        : (prev.open + prev.close) / 2;
    out.push({
      ...b,
      open,
      close,
      high: Math.max(b.high, open, close),
      low: Math.min(b.low, open, close),
    });
  }
  return out;
}
