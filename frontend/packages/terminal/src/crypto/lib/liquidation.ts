// where a linear position is liquidated, from the venue's maintenance tiers. an estimate: fees, funding and
// other positions sharing a cross wallet are left out

export interface MaintenanceTier {
  notional_floor: string;
  notional_cap?: string;
  maintenance_margin_rate: string;
  maintenance_amount: string;
  max_leverage?: string;
}

export interface LiquidationInput {
  side: "buy" | "sell";
  entry: number;
  // base units, positive
  size: number;
  leverage: number;
  mode: "cross" | "isolated";
  // ascending by notional_floor, as GET /v1/accounts/{account}/terms returns them
  tiers: readonly MaintenanceTier[];
  // quote currency; cross margin puts all of it behind the position, isolated ignores it
  walletBalance?: number;
}

interface Tier {
  floor: number;
  cap: number;
  rate: number;
  amount: number;
}

function read(tiers: readonly MaintenanceTier[]): Tier[] {
  return tiers
    .map((t) => ({
      floor: Number(t.notional_floor),
      cap: t.notional_cap === undefined ? Infinity : Number(t.notional_cap),
      rate: Number(t.maintenance_margin_rate),
      amount: Number(t.maintenance_amount),
    }))
    .filter(
      (t) =>
        Number.isFinite(t.floor) &&
        Number.isFinite(t.rate) &&
        Number.isFinite(t.amount) &&
        !Number.isNaN(t.cap),
    );
}

// maintenance margin for a notional: the tier's rate less its amount, never below zero; null without tiers
export function maintenanceMargin(
  tiers: readonly MaintenanceTier[],
  notional: number,
): number | null {
  const rows = read(tiers);
  const size = Math.abs(notional);
  let found: Tier | null = null;
  for (const t of rows) {
    if (found !== null && size < t.floor) break;
    found = t;
  }
  return found === null ? null : Math.max(0, size * found.rate - found.amount);
}

// null where it cannot be told (no tiers, cross without a balance, bad input) or where the position cannot be
// liquidated at any positive price, as a long fully paid for
export function liquidationPrice(input: LiquidationInput): number | null {
  const { side, entry, size, leverage, mode, walletBalance } = input;
  if (!(entry > 0) || !(size > 0)) return null;
  const rows = read(input.tiers);
  if (rows.length === 0) return null;
  let margin: number;
  if (mode === "isolated") {
    if (!(leverage > 0)) return null;
    margin = (entry * size) / leverage;
  } else {
    if (walletBalance === undefined || !Number.isFinite(walletBalance))
      return null;
    margin = walletBalance;
  }
  const dir = side === "buy" ? 1 : -1;
  // margin + dir * size * (p - entry) = size * p * rate - amount, solved for p inside one tier
  const solve = (t: Tier): number =>
    (margin - dir * size * entry + t.amount) / (size * (t.rate - dir));
  let price: number | null = null;
  for (const t of rows) {
    const p = solve(t);
    const notional = size * p;
    if (notional >= t.floor && notional < t.cap) {
      price = p;
      break;
    }
  }
  if (price === null) {
    // tiers that jump at a floor can leave no tier holding its own answer: the entry's tier stands in
    const at = entry * size;
    let tier: Tier | null = null;
    for (const t of rows) {
      if (tier === null || at >= t.floor) tier = t;
    }
    price = tier === null ? null : solve(tier);
  }
  return price !== null && Number.isFinite(price) && price > 0 ? price : null;
}
