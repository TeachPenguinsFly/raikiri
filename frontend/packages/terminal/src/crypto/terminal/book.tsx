import { memo, useCallback, useMemo, useRef, useState } from "react";
import { createPortal } from "react-dom";
import { cn, ExchangeIcon, venueLabel } from "@raikiri/ui";
import {
  compact,
  Corners,
  Empty,
  Flash,
  grouped,
  merged,
  n,
  Seg,
  steps,
  sums,
  type Level,
  type Sum,
} from "@raikiri/ui/v3";
import type { Book } from "../api/client";
import { bp, px, qty, venueColor, type Leg } from "../lib/market";
import { useTerminal, type BookVenue } from "./state";

type Side = "bids" | "asks";
type View = "both" | Side;
type Unit = "base" | "usd";

const k_stale_ns = 10e9;
const k_rows = 40;
const k_saved = "raikiri.book.view";

function saved(): { view: View | undefined; unit: Unit | undefined } {
  try {
    const v = JSON.parse(localStorage.getItem(k_saved) ?? "{}") as { view?: unknown; unit?: unknown };
    return {
      view: v.view === "both" || v.view === "bids" || v.view === "asks" ? v.view : undefined,
      unit: v.unit === "base" || v.unit === "usd" ? v.unit : undefined,
    };
  } catch {
    return { view: undefined, unit: undefined };
  }
}

function save(v: { view: View; unit: Unit }) {
  try {
    localStorage.setItem(k_saved, JSON.stringify(v));
  } catch {
    // private windows refuse storage; the choice then lasts for the tab
  }
}

function levelsOf(book: Book | undefined, leg: Leg, side: Side): Level[] {
  const out: Level[] = [];
  if (!leg.sized) return out;
  for (const l of book?.[side] ?? []) {
    const price = n(l.price);
    const size = n(l.size);
    if (price === undefined || size === undefined || size <= 0) continue;
    out.push({
      price,
      size: size * leg.unit,
      parts: { [leg.venue]: size * leg.unit },
    });
  }
  return out;
}

function placesOf(step: number): number {
  const s = step.toFixed(10).replace(/0+$/, "");
  return Math.min(8, s.length - s.indexOf(".") - 1);
}

const stepLabel = (step: number) =>
  step.toLocaleString("en-US", { maximumFractionDigits: 10 });

/** The ladder of one venue, or of every venue summed by price, sizes in the base asset. A venue's ladder comes as
 *  the venue ticks unless a coarser step is picked; the summed one always sits on a step. */
export function useBook(venue: BookVenue, pick?: number) {
  const { pair, leg, books } = useTerminal();
  // Combined sums the venues quoting in the chosen venue's currency: a dollar book under a tether book is a
  // different price for the same coin, and the two summed cross by the difference
  const quote = leg?.quote;
  const legs = useMemo(
    () =>
      (pair?.legs ?? []).filter((l) =>
        venue === "all"
          ? quote === undefined || l.quote === quote
          : l.venue === venue,
      ),
    [pair, venue, quote],
  );
  const raw = useMemo(() => {
    // a venue whose ladder stopped moving while the others went on would sit across them
    let newest = 0;
    for (const l of legs)
      newest = Math.max(newest, Number(books.get(l.id)?.ts_event ?? 0));
    const live = (l: Leg) => {
      const book = books.get(l.id);
      return legs.length > 1 && Number(book?.ts_event ?? 0) < newest - k_stale_ns
        ? undefined
        : book;
    };
    const of = (side: Side) => legs.map((l) => levelsOf(live(l), l, side));
    const bids = of("bids");
    const asks = of("asks");
    const best = (side: Level[][], better: (...v: number[]) => number) => {
      const tops = side.flatMap((b) =>
        b[0] === undefined ? [] : [b[0].price],
      );
      return tops.length === 0 ? undefined : better(...tops);
    };
    return {
      bids,
      asks,
      // the touch as quoted, which a grouped ladder no longer shows
      bid: best(bids, Math.max),
      ask: best(asks, Math.min),
      has: legs.some((l) => books.has(l.id)),
    };
  }, [legs, books]);

  const near = raw.bid ?? raw.ask ?? 1;
  const exp = near > 0 ? Math.floor(Math.log10(near)) : 0;
  const ladder = useMemo(() => {
    const ticks = legs.map((l) => (l.tick > 0 ? l.tick : 0.01));
    if (venue !== "all") {
      const all = steps(ticks[0] ?? 0.01, 10 ** exp);
      return {
        steps: all,
        usual: all[0] as number,
        fine: Math.max(0, ...legs.map((l) => l.places)),
      };
    }
    // Venues tick in different sizes, down to a hundred-thousandth of a dollar, so summed as quoted no two levels
    // meet and the ladder reads as noise. The summed ladder opens on a step of five significant figures instead,
    // with one finer on offer when some venue ticks that fine.
    const five = 10 ** (exp - 4);
    const all = steps(
      Math.min(...ticks) <= five / 9 ? 10 ** (exp - 5) : five,
      10 ** exp,
    );
    return {
      steps: all,
      usual: all.find((s) => s >= five * 0.999) ?? (all[0] as number),
      fine: Math.max(0, 4 - exp),
    };
  }, [legs, venue, exp]);
  const step =
    pick !== undefined && ladder.steps.includes(pick) ? pick : ladder.usual;

  return useMemo(() => {
    const asQuoted = venue !== "all" && step === ladder.usual;
    const places = asQuoted ? ladder.fine : placesOf(step);
    const side = (s: Side) =>
      asQuoted
        ? merged(raw[s], s, 10 ** places)
        : grouped(raw[s], s, step, places);
    return {
      // a block shows two dozen rows a side; forty leave room for one side alone, and the rest would only be drawn
      bids: side("bids").slice(0, k_rows),
      asks: side("asks").slice(0, k_rows),
      has: raw.has,
      places,
      legs,
      step,
      steps: ladder.steps,
      bid: raw.bid,
      ask: raw.ask,
      /** decimals of the touch, finer than a grouped ladder's */
      fine: ladder.fine,
    };
  }, [raw, legs, venue, step, ladder]);
}

export function BookTools() {
  const { pair, leg, bookVenue, setBookVenue } = useTerminal();
  if (pair === undefined || pair.legs.length < 2) return null;
  return (
    <Seg
      size="xs"
      value={bookVenue === "all" ? "all" : "one"}
      onChange={(v) =>
        setBookVenue(v === "all" ? "all" : (leg?.venue ?? "all"))
      }
      options={[
        [
          "one",
          <span key="one" className="flex items-center gap-1.5">
            {leg !== undefined && (
              <ExchangeIcon venue={leg.venue} className="size-3.5" />
            )}
            {leg === undefined ? "One" : venueLabel(leg.venue)}
          </span>,
        ],
        ["all", "Combined"],
      ]}
    />
  );
}

/** the few levels far above their side's run, where the book would stop a move */
function wallsOf(levels: readonly Level[]): Set<number> {
  if (levels.length < 8) return k_no_walls;
  const mean = levels.reduce((a, l) => a + l.size, 0) / levels.length;
  return new Set(
    levels
      .filter((l) => l.size >= mean * 3)
      .sort((a, b) => b.size - a.size)
      .slice(0, 3)
      .map((l) => l.price),
  );
}
const k_no_walls: Set<number> = new Set();

/** what the side holds down to each level, as one curve behind its rows; the asks' runs upward from the spread */
const Depth = memo(function Depth({
  side,
  sums: s,
  deepest,
}: {
  side: Side;
  sums: Sum[];
  deepest: number;
}) {
  const path = useMemo(() => {
    const rows = s.length;
    if (rows === 0) return undefined;
    const x = (i: number) => (((s[i] as Sum).usd / deepest) * 100).toFixed(2);
    const y = (i: number) => (side === "asks" ? rows - i - 0.5 : i + 0.5);
    const [touch, far] = side === "asks" ? [rows, 0] : [0, rows];
    let line = `M${x(0)},${touch}L${x(0)},${y(0)}`;
    for (let i = 1; i < rows; i++) {
      const half = (y(i - 1) + y(i)) / 2;
      line += `C${x(i - 1)},${half} ${x(i)},${half} ${x(i)},${y(i)}`;
    }
    line += `L${x(rows - 1)},${far}`;
    return { line, area: `${line}L0,${far}L0,${touch}Z` };
  }, [s, deepest, side]);
  if (path === undefined) return null;
  return (
    <svg
      className="pointer-events-none absolute inset-0 size-full"
      viewBox={`0 0 100 ${s.length}`}
      preserveAspectRatio="none"
      aria-hidden="true"
    >
      <path
        d={path.area}
        className={side === "bids" ? "fill-up/[0.08]" : "fill-down/[0.08]"}
      />
      <path
        d={path.line}
        fill="none"
        strokeWidth={1}
        vectorEffect="non-scaling-stroke"
        className={side === "bids" ? "stroke-up/35" : "stroke-down/35"}
      />
    </svg>
  );
});

interface RowProps {
  side: Side;
  i: number;
  price: number;
  size: number;
  shares: number;
  usd: number;
  /** the level's size against the biggest shown */
  share: number;
  places: number;
  unit: Unit;
  wall: boolean;
  lit: boolean;
  /** per-venue sizes, when the ladder is several venues' */
  parts: Level["parts"] | undefined;
  legs: readonly Leg[];
  onEnter: (side: Side, i: number, rect: DOMRect) => void;
  onLeave: () => void;
  onPick: (side: Side, price: number) => void;
}

const k_plain = [
  "side",
  "i",
  "price",
  "size",
  "shares",
  "usd",
  "share",
  "places",
  "unit",
  "wall",
  "lit",
  "legs",
] as const;

// a frame rebuilds every level object, so the parts are compared by what they hold: a row whose numbers stood still
// is not drawn again
function sameRow(a: RowProps, b: RowProps): boolean {
  for (const k of k_plain) if (a[k] !== b[k]) return false;
  if ((a.parts === undefined) !== (b.parts === undefined)) return false;
  if (a.parts !== undefined && b.parts !== undefined) {
    for (const leg of a.legs)
      if (a.parts[leg.venue] !== b.parts[leg.venue]) return false;
  }
  return true;
}

const Row = memo(function Row({
  side,
  i,
  price,
  size,
  shares,
  usd,
  share,
  places,
  unit,
  wall,
  lit,
  parts,
  legs,
  onEnter,
  onLeave,
  onPick,
}: RowProps) {
  const up = side === "bids";
  return (
    <div
      role="button"
      tabIndex={-1}
      onPointerEnter={(e) =>
        onEnter(side, i, e.currentTarget.getBoundingClientRect())
      }
      onPointerLeave={onLeave}
      onClick={() => onPick(side, price)}
      className={cn(
        "relative grid h-[22px] shrink-0 cursor-pointer grid-cols-[1.2fr_1fr_1fr] items-center px-3 text-xs transition-colors duration-100",
        lit ? (up ? "bg-up/[0.07]" : "bg-down/[0.07]") : "hover:bg-hover/50",
      )}
    >
      {wall && (
        <span
          className={cn(
            "absolute inset-y-px left-0 w-0.5",
            up ? "bg-up" : "bg-down",
          )}
        />
      )}
      <span
        className={cn(
          "absolute bottom-0 left-0 flex h-0.5 w-full origin-left transition-transform duration-300 ease-out",
          parts === undefined && (up ? "bg-up/60" : "bg-down/60"),
        )}
        style={{ transform: `scaleX(${share})` }}
      >
        {parts !== undefined &&
          legs.map((leg) =>
            parts[leg.venue] === undefined ? null : (
              <span
                key={leg.venue}
                className="h-full opacity-70"
                style={{
                  width: `${((parts[leg.venue] ?? 0) / size) * 100}%`,
                  background: venueColor(leg.venue),
                }}
              />
            ),
          )}
      </span>
      <span
        className={cn(
          "mono relative flex items-center gap-1",
          up ? "text-up" : "text-down",
          i === 0 && "font-semibold",
        )}
      >
        {px(price, places)}
        {parts !== undefined &&
          legs.map((leg) =>
            parts[leg.venue] === undefined ? null : (
              <span
                key={leg.venue}
                className="size-1 rounded-full"
                style={{ background: venueColor(leg.venue) }}
              />
            ),
          )}
      </span>
      <span
        className={cn(
          "mono relative text-right",
          wall ? "font-medium text-fg" : "text-fg-mid",
        )}
      >
        <Flash value={size}>
          {unit === "base" ? qty(size) : compact(size * price, true)}
        </Flash>
      </span>
      <span
        className={cn(
          "mono relative text-right",
          lit ? "text-fg" : "text-fg-dim",
        )}
      >
        {unit === "base" ? qty(shares) : compact(usd, true)}
      </span>
    </div>
  );
}, sameRow);

const k_views: ReadonlyArray<readonly [View, string, string, string]> = [
  ["both", "Both sides", "fill-down", "fill-up"],
  ["bids", "Bids", "fill-up", "fill-up/40"],
  ["asks", "Asks", "fill-down/40", "fill-down"],
];

export function BookWidget() {
  const { pair, bookVenue, setDraft } = useTerminal();
  const [pick, setPick] = useState<number | undefined>(undefined);
  const [view, setViewState] = useState<View>(() => saved().view ?? "both");
  const [unit, setUnitState] = useState<Unit>(() => saved().unit ?? "base");
  const setView = (v: View) => (setViewState(v), save({ view: v, unit }));
  const setUnit = (u: Unit) => (setUnitState(u), save({ view, unit: u }));
  const book = useBook(bookVenue, pick);
  const [hover, setHover] = useState<{
    side: Side;
    i: number;
    rect: DOMRect;
  } | null>(null);
  const askSums = useMemo(() => sums(book.asks), [book.asks]);
  const bidSums = useMemo(() => sums(book.bids), [book.bids]);
  const askWalls = useMemo(() => wallsOf(book.asks), [book.asks]);
  const bidWalls = useMemo(() => wallsOf(book.bids), [book.bids]);
  const biggest = useMemo(
    () =>
      Math.max(
        1e-12,
        ...book.asks.map((l) => l.size),
        ...book.bids.map((l) => l.size),
      ),
    [book.asks, book.bids],
  );
  const asksHeld = askSums.at(-1);
  const bidsHeld = bidSums.at(-1);
  const deepest = Math.max(
    (view === "bids" ? 0 : asksHeld?.usd) ?? 0,
    (view === "asks" ? 0 : bidsHeld?.usd) ?? 0,
    1,
  );
  const spread =
    book.ask !== undefined && book.bid !== undefined
      ? book.ask - book.bid
      : undefined;
  const mid =
    book.ask !== undefined && book.bid !== undefined
      ? (book.ask + book.bid) / 2
      : undefined;
  const held = (bidsHeld?.shares ?? 0) + (asksHeld?.shares ?? 0);
  const bidShare = held > 0 ? (bidsHeld?.shares ?? 0) / held : undefined;

  // the provider hands a new setDraft every render; the rows keep one handler so a frame does not redraw them all
  const draft = useRef(setDraft);
  draft.current = setDraft;
  const onPick = useCallback(
    (side: Side, price: number) =>
      draft.current({ action: side === "asks" ? "buy" : "sell", price }),
    [],
  );
  const onEnter = useCallback(
    (side: Side, i: number, rect: DOMRect) => setHover({ side, i, rect }),
    [],
  );
  const onLeave = useCallback(() => setHover(null), []);

  if (pair === undefined) return <Empty title="Pick a market" />;

  const several = book.legs.length > 1;
  const rows = (levels: Level[], s: Sum[], side: Side, walls: Set<number>) =>
    levels.map((l, i) => (
      <Row
        key={l.price}
        side={side}
        i={i}
        price={l.price}
        size={l.size}
        shares={(s[i] as Sum).shares}
        usd={(s[i] as Sum).usd}
        share={l.size / biggest}
        places={book.places}
        unit={unit}
        wall={walls.has(l.price)}
        lit={hover?.side === side && i <= hover.i}
        parts={several ? l.parts : undefined}
        legs={book.legs}
        onEnter={onEnter}
        onLeave={onLeave}
        onPick={onPick}
      />
    ));

  const tip =
    hover === null
      ? undefined
      : (hover.side === "asks" ? askSums : bidSums)[hover.i];
  const levels =
    hover === null
      ? []
      : (hover.side === "asks" ? book.asks : book.bids).slice(0, hover.i + 1);

  return (
    <div className="flex h-full min-h-0 flex-col">
      <div className="flex shrink-0 items-center justify-between gap-1.5 px-2 pt-1.5">
        <span className="flex items-baseline gap-1 pl-1 text-xs">
          <span className="font-medium text-fg">{pair.base}</span>
          <span className="text-[10px] text-fg-dim">
            {bookVenue === "all" ? "combined" : pair.quote}
          </span>
        </span>
        <BookTools />
      </div>
      <div className="flex shrink-0 flex-wrap items-center gap-1.5 px-2 py-1.5">
        <div
          role="tablist"
          className="flex items-center rounded-md bg-canvas/60 p-0.5"
        >
          {k_views.map(([v, label, top, bottom]) => (
            <button
              key={v}
              type="button"
              role="tab"
              aria-label={label}
              aria-selected={v === view}
              onClick={() => setView(v)}
              className={cn(
                "grid size-5 place-items-center rounded-[5px] transition-colors duration-200",
                v === view ? "bg-raised" : "opacity-60 hover:opacity-100",
              )}
            >
              <svg viewBox="0 0 12 12" className="size-3" aria-hidden="true">
                <rect
                  x="1.5"
                  y="1.5"
                  width="9"
                  height="3.75"
                  rx="0.75"
                  className={top}
                />
                <rect
                  x="1.5"
                  y="6.75"
                  width="9"
                  height="3.75"
                  rx="0.75"
                  className={bottom}
                />
              </svg>
            </button>
          ))}
        </div>
        {book.steps.length > 1 && (
          <Seg
            size="xs"
            className="mono"
            value={String(book.step)}
            onChange={(v) => setPick(Number(v))}
            options={book.steps.map((s) => [String(s), stepLabel(s)] as const)}
          />
        )}
        <Seg
          size="xs"
          className="ml-auto"
          value={unit}
          onChange={setUnit}
          options={[
            ["base", pair.base],
            ["usd", "USD"],
          ]}
        />
      </div>
      <div className="col-head grid shrink-0 grid-cols-[1.2fr_1fr_1fr] px-3 pb-1">
        <span>Price</span>
        <span className="text-right">Size</span>
        <span className="text-right">Total</span>
      </div>
      {!book.has ? (
        <Empty title="No book yet" />
      ) : (
        <>
          {view !== "bids" && (
            <div className="flex min-h-0 flex-1 flex-col-reverse overflow-y-auto [scrollbar-width:thin]">
              <div className="relative flex shrink-0 flex-col-reverse">
                <Depth side="asks" sums={askSums} deepest={deepest} />
                {rows(book.asks, askSums, "asks", askWalls)}
              </div>
            </div>
          )}
          <div className="flex h-8 shrink-0 items-center justify-between gap-2 border-y border-line bg-raised/60 px-3">
            <span className="mono text-sm font-semibold text-fg">
              <Flash value={mid}>{px(mid, book.fine)}</Flash>
            </span>
            <span className="mono flex items-baseline gap-2 text-xs">
              <span className="text-fg-mid">{px(spread, book.fine)}</span>
              <span
                className={
                  spread !== undefined && spread < 0 ? "text-up" : "text-fg-dim"
                }
              >
                {spread === undefined || mid === undefined
                  ? ""
                  : bp((spread / mid) * 1e4, 2)}
              </span>
            </span>
          </div>
          {view !== "asks" && (
            <div className="flex min-h-0 flex-1 flex-col overflow-y-auto [scrollbar-width:thin]">
              <div className="relative flex shrink-0 flex-col">
                <Depth side="bids" sums={bidSums} deepest={deepest} />
                {rows(book.bids, bidSums, "bids", bidWalls)}
              </div>
            </div>
          )}
          {bidShare !== undefined && (
            <div className="relative mx-2 my-1.5 flex h-5 shrink-0 items-center justify-between overflow-hidden rounded-sm bg-canvas/60 px-2 text-2xs">
              <span
                className="absolute inset-0 origin-left bg-up/20 transition-transform duration-300 ease-out"
                style={{ transform: `scaleX(${bidShare})` }}
              />
              <span
                className="absolute inset-0 origin-right bg-down/20 transition-transform duration-300 ease-out"
                style={{ transform: `scaleX(${1 - bidShare})` }}
              />
              <span className="absolute inset-y-0 left-1/2 w-px bg-line" />
              <span className="mono relative text-up">
                {(bidShare * 100).toFixed(1)}%
              </span>
              <span className="mono relative text-down">
                {((1 - bidShare) * 100).toFixed(1)}%
              </span>
            </div>
          )}
        </>
      )}
      {hover !== null && tip !== undefined && (
        <BookTip
          rect={hover.rect}
          sum={tip}
          levels={levels}
          side={hover.side}
          places={book.places}
          base={pair.base}
        />
      )}
    </div>
  );
}

/** what taking every level up to the hovered one costs, beside the book like a tooltip card */
function BookTip({
  rect,
  sum,
  levels,
  side,
  places,
  base,
}: {
  rect: DOMRect;
  sum: Sum;
  levels: Level[];
  side: Side;
  places: number;
  base: string;
}) {
  const w = 208;
  const left = rect.left - w - 10 > 8 ? rect.left - w - 10 : rect.right + 10;
  const avg = sum.shares > 0 ? sum.usd / sum.shares : undefined;
  const first = levels[0]?.price;
  const worst = levels.at(-1)?.price;
  const impact =
    avg !== undefined && first !== undefined
      ? (Math.abs(avg - first) / first) * 1e4
      : undefined;
  return createPortal(
    <div
      className="v3 v3-pop pointer-events-none fixed z-50 rounded-sm bg-raised/95 px-3 py-2 text-xs shadow-[0_16px_40px_-8px_rgb(0_0_0/0.85),0_0_0_1px_var(--color-line)] backdrop-blur"
      style={{ top: rect.top + rect.height / 2 - 52, left, width: w }}
    >
      <Corners />
      <div className="flex justify-between gap-3 py-0.5">
        <span className="text-fg-dim">Avg. price</span>
        <span className={cn("mono", side === "asks" ? "text-down" : "text-up")}>
          {px(avg, places + 1)}
        </span>
      </div>
      <div className="flex justify-between gap-3 py-0.5">
        <span className="text-fg-dim">Sum {base}</span>
        <span className="mono text-fg">{qty(sum.shares)}</span>
      </div>
      <div className="flex justify-between gap-3 py-0.5">
        <span className="text-fg-dim">Sum USD</span>
        <span className="mono text-fg">{compact(sum.usd, true)}</span>
      </div>
      <div className="flex justify-between gap-3 py-0.5">
        <span className="text-fg-dim">Impact</span>
        <span className="mono text-fg-mid">{bp(impact, 2)}</span>
      </div>
      <div className="mt-1 border-t border-line pt-1 text-2xs text-fg-dim">
        {levels.length} {levels.length === 1 ? "level" : "levels"} · to{" "}
        {px(worst, places)}
      </div>
    </div>,
    document.body,
  );
}
