import assert from "node:assert/strict";
import { test } from "node:test";
import { liquidationPrice, maintenanceMargin } from "./liquidation.ts";

// binance's published ETHUSDT brackets
const tiers = [
  {
    notional_floor: "0",
    notional_cap: "10000",
    maintenance_margin_rate: "0.0065",
    maintenance_amount: "0",
    max_leverage: "75",
  },
  {
    notional_floor: "10000",
    notional_cap: "50000",
    maintenance_margin_rate: "0.01",
    maintenance_amount: "35",
    max_leverage: "50",
  },
  {
    notional_floor: "50000",
    maintenance_margin_rate: "0.02",
    maintenance_amount: "535",
  },
];

const near = (actual, expected, eps = 1e-6) =>
  assert.ok(Math.abs(actual - expected) < eps, `${actual} != ${expected}`);

test("maintenance margin is the tier rate less its amount", () => {
  assert.equal(maintenanceMargin(tiers, 5000), 32.5);
  assert.equal(maintenanceMargin(tiers, -20000), 165);
  assert.equal(maintenanceMargin(tiers, 1000000), 19465);
  assert.equal(maintenanceMargin([], 1000), null);
});

test("an isolated long is liquidated where its margin meets the maintenance margin", () => {
  // 1 @ 2000, 10x: margin 200; p = (2000 - 200 - 0) / (1 - 0.0065)
  const p = liquidationPrice({
    side: "buy",
    entry: 2000,
    size: 1,
    leverage: 10,
    mode: "isolated",
    tiers,
  });
  near(p, 1800 / 0.9935);
  // at that price equity equals the maintenance margin
  near(200 + (p - 2000), maintenanceMargin(tiers, p));
});

test("an isolated short is liquidated above its entry", () => {
  const p = liquidationPrice({
    side: "sell",
    entry: 2000,
    size: 1,
    leverage: 10,
    mode: "isolated",
    tiers,
  });
  near(p, 2200 / 1.0065);
  near(200 - (p - 2000), maintenanceMargin(tiers, p));
});

test("the tier is the one the liquidation notional falls in, not the entry one", () => {
  // 10 @ 2000 is 20000 notional, second tier; a 2x long falls to about half, the first tier
  const p = liquidationPrice({
    side: "buy",
    entry: 2000,
    size: 10,
    leverage: 2,
    mode: "isolated",
    tiers,
  });
  assert.ok(p * 10 < 10000 + 100);
  near(10000 + 10 * (p - 2000), maintenanceMargin(tiers, 10 * p), 1e-6);
});

test("a larger position uses its amount", () => {
  // 20 @ 2000, 20x: margin 2000, liquidation notional near 38400, second tier
  const p = liquidationPrice({
    side: "buy",
    entry: 2000,
    size: 20,
    leverage: 20,
    mode: "isolated",
    tiers,
  });
  near(p, (40000 - 2000 - 35) / (20 * 0.99));
});

test("cross puts the wallet behind the position and ignores leverage", () => {
  const a = liquidationPrice({
    side: "buy",
    entry: 2000,
    size: 1,
    leverage: 10,
    mode: "cross",
    tiers,
    walletBalance: 500,
  });
  const b = liquidationPrice({
    side: "buy",
    entry: 2000,
    size: 1,
    leverage: 50,
    mode: "cross",
    tiers,
    walletBalance: 500,
  });
  near(a, 1500 / 0.9935);
  assert.equal(a, b);
  const short = liquidationPrice({
    side: "sell",
    entry: 2000,
    size: 1,
    leverage: 10,
    mode: "cross",
    tiers,
    walletBalance: 500,
  });
  near(short, 2500 / 1.0065);
});

test("what cannot be told is null", () => {
  const base = {
    side: "buy",
    entry: 2000,
    size: 1,
    leverage: 10,
    mode: "isolated",
    tiers,
  };
  assert.equal(liquidationPrice({ ...base, tiers: [] }), null);
  assert.equal(liquidationPrice({ ...base, mode: "cross" }), null);
  assert.equal(liquidationPrice({ ...base, size: 0 }), null);
  assert.equal(liquidationPrice({ ...base, entry: 0 }), null);
  assert.equal(liquidationPrice({ ...base, leverage: 0 }), null);
});

test("a long the wallet pays for in full is never liquidated", () => {
  const p = liquidationPrice({
    side: "buy",
    entry: 2000,
    size: 1,
    leverage: 1,
    mode: "cross",
    tiers,
    walletBalance: 5000,
  });
  assert.equal(p, null);
});

test("tiers without amounts jump at a floor and the entry tier stands in", () => {
  const jumpy = [
    {
      notional_floor: "0",
      notional_cap: "1000",
      maintenance_margin_rate: "0.004",
      maintenance_amount: "0",
    },
    {
      notional_floor: "1000",
      notional_cap: "5000",
      maintenance_margin_rate: "0.4",
      maintenance_amount: "0",
    },
  ];
  // 1 @ 1010, 20x: the second tier's answer lands under its floor and the first tier's above its cap
  const p = liquidationPrice({
    side: "buy",
    entry: 1010,
    size: 1,
    leverage: 20,
    mode: "isolated",
    tiers: jumpy,
  });
  assert.ok(p !== null && p > 0);
});
