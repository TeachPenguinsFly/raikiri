export interface Level<V extends string = string> {
  price: number;
  size: number;
  /** size per venue, when the book is several venues' merged */
  parts: Partial<Record<V, number>>;
}

/** several venues' levels at one price summed, best first; prices are one when they match to 1/scale */
export function merged<V extends string>(
  books: Level<V>[][],
  side: "bids" | "asks",
  scale = 10_000,
): Level<V>[] {
  const at = new Map<number, Level<V>>();
  for (const book of books) {
    for (const l of book) {
      const key = Math.round(l.price * scale);
      const was = at.get(key);
      if (was === undefined) at.set(key, { ...l, parts: { ...l.parts } });
      else {
        was.size += l.size;
        for (const [v, s] of Object.entries(l.parts) as Array<[V, number]>)
          was.parts[v] = (was.parts[v] ?? 0) + s;
      }
    }
  }
  return [...at.values()].sort((a, b) =>
    side === "asks" ? a.price - b.price : b.price - a.price,
  );
}

/** levels gathered onto a price step, best first: a bid goes down to its step and an ask up, so no level looks
 *  better than it is */
export function grouped<V extends string>(
  books: Level<V>[][],
  side: "bids" | "asks",
  step: number,
  places: number,
): Level<V>[] {
  const at = new Map<number, Level<V>>();
  for (const book of books) {
    for (const l of book) {
      const q = l.price / step;
      // the nudge keeps a price already on the step from slipping a whole step on a float's last digit
      const nudge = 1e-9 + Math.abs(q) * 1e-12;
      const key =
        side === "bids" ? Math.floor(q + nudge) : Math.ceil(q - nudge);
      const was = at.get(key);
      if (was === undefined)
        at.set(key, {
          price: Number((key * step).toFixed(places)),
          size: l.size,
          parts: { ...l.parts },
        });
      else {
        was.size += l.size;
        for (const [v, s] of Object.entries(l.parts) as Array<[V, number]>)
          was.parts[v] = (was.parts[v] ?? 0) + s;
      }
    }
  }
  return [...at.entries()]
    .sort((a, b) => (side === "asks" ? a[0] - b[0] : b[0] - a[0]))
    .map(([, l]) => l);
}

/** the steps a ladder can be grouped by: the finest and its tens, between a millionth and a hundredth of the
 *  price's decade, so the choice suits a coin at any price; the finest always stays */
export function steps(finest: number, decade: number): number[] {
  const out = [Number(finest.toPrecision(12))];
  for (
    let s = finest * 10;
    s <= decade * 0.0100001 && out.length < 5;
    s *= 10
  ) {
    if (s >= decade * 0.999e-6) out.push(Number(s.toPrecision(12)));
  }
  return out;
}

export interface Sum {
  shares: number;
  usd: number;
}

/** what the book holds down to each level */
export function sums(levels: readonly Level[]): Sum[] {
  let shares = 0;
  let usd = 0;
  return levels.map((l) => {
    shares += l.size;
    usd += l.size * l.price;
    return { shares, usd };
  });
}

export interface Fill {
  shares: number;
  usd: number;
  levels: number;
  worst: number | undefined;
  /** the order wanted more than the book held inside the limit */
  short: boolean;
}

/** takes levels best first while they sit inside the limit, until the dollars or the shares run out */
export function walk(
  levels: readonly Level[],
  want: number,
  unit: "usd" | "shares",
  limit: number | undefined,
  action: "buy" | "sell",
): Fill {
  let shares = 0;
  let usd = 0;
  let used = 0;
  let worst: number | undefined;
  for (const l of levels) {
    if (
      limit !== undefined &&
      (action === "buy" ? l.price > limit + 1e-9 : l.price < limit - 1e-9)
    )
      break;
    const left = unit === "usd" ? want - usd : want - shares;
    if (left <= 1e-9) break;
    const take =
      unit === "usd"
        ? Math.min(l.size, left / l.price)
        : Math.min(l.size, left);
    shares += take;
    usd += take * l.price;
    used += 1;
    worst = l.price;
  }
  const got = unit === "usd" ? usd : shares;
  return {
    shares,
    usd,
    levels: used,
    worst,
    short: want > 0 && got < want - 1e-6,
  };
}
