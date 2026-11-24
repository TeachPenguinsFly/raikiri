import { useRef, useState, type ReactNode } from "react";
import {
  AreaSeries,
  BarSeries,
  BaselineSeries,
  CandlestickSeries,
  LineSeries,
  LineStyle,
  type IChartApi,
  type ISeriesApi,
  type SeriesType,
  type UTCTimestamp,
} from "lightweight-charts";
import { cn, useDismiss } from "@raikiri/ui";
import { Check, compact, Glyph, type Candle } from "@raikiri/ui/v3";
import { px } from "../lib/market";
import {
  atr,
  bollinger,
  ema,
  heikinAshi,
  macd,
  rsi,
  sma,
  stochRsi,
  vwap,
  type Series,
} from "./indicators";

export const k_up = "#2fd3a6";
export const k_down = "#f2557d";
export const k_accent = "#38bdf8";
const k_up_soft = "rgba(47,211,166,0.28)";
const k_down_soft = "rgba(242,85,125,0.28)";
const k_grey = "#8b8b98";

export type Kind =
  | "bars"
  | "candles"
  | "hollow"
  | "heikin"
  | "line"
  | "area"
  | "baseline";
const k_kinds: ReadonlyArray<readonly [Kind, string, ReactNode]> = [
  [
    "bars",
    "Bars",
    <path key="p" d="M5 3v10M3 5.5h2M5 10.5h2M11 2.5v10M9 4.5h2M11 9h2" />,
  ],
  [
    "candles",
    "Candles",
    <path
      key="p"
      fill="currentColor"
      d="M5 2.5v11M3.75 5h2.5v5.5h-2.5zM11 2.5v11M9.75 4h2.5v4h-2.5z"
    />,
  ],
  [
    "hollow",
    "Hollow candles",
    <path
      key="p"
      d="M5 2.5V5m0 5.5v3M3.75 5h2.5v5.5h-2.5zM11 2.5V4m0 4v5.5M9.75 4h2.5v4h-2.5z"
    />,
  ],
  [
    "heikin",
    "Heikin Ashi",
    <path
      key="p"
      fill="currentColor"
      d="M4 5v8M2.9 7h2.2v4H2.9zM8 3.5v8M6.9 5.5h2.2v4H6.9zM12 2v8M10.9 4h2.2v4h-2.2z"
    />,
  ],
  ["line", "Line", <path key="p" d="m2.5 11 3.5-4 3 2.5 4-5" />],
  [
    "area",
    "Area",
    <g key="p">
      <path
        stroke="none"
        fill="currentColor"
        fillOpacity={0.3}
        d="m2.5 11 3.5-4 3 2.5 4-5v9h-10.5z"
      />
      <path d="m2.5 11 3.5-4 3 2.5 4-5" />
    </g>,
  ],
  [
    "baseline",
    "Baseline",
    <g key="p">
      <path strokeDasharray="1.5 2" d="M2 8.5h12" />
      <path d="m2.5 11 3-6 3.5 6.5 4-7" />
    </g>,
  ],
];

export type Study =
  | "ma"
  | "ema"
  | "bb"
  | "vwap"
  | "vol"
  | "rsi"
  | "macd"
  | "stoch"
  | "atr";
/** in the order they are listed and their panes stack; a zero period drops that average's line */
const k_studies: Record<
  Study,
  { label: string; pane: boolean; params: readonly number[] }
> = {
  ma: { label: "MA", pane: false, params: [7, 25, 99] },
  ema: { label: "EMA", pane: false, params: [7, 25, 99] },
  bb: { label: "BB", pane: false, params: [20, 2] },
  vwap: { label: "VWAP", pane: false, params: [] },
  vol: { label: "Vol", pane: true, params: [20] },
  rsi: { label: "RSI", pane: true, params: [14] },
  macd: { label: "MACD", pane: true, params: [12, 26, 9] },
  stoch: { label: "Stoch RSI", pane: true, params: [14, 14, 3, 3] },
  atr: { label: "ATR", pane: true, params: [14] },
};
const k_order = Object.keys(k_studies) as Study[];
const k_lines: Record<Study, readonly string[]> = {
  ma: ["#f0b90b", "#e879f9", "#60a5fa"],
  ema: ["#fb923c", "#a78bfa", "#22d3ee"],
  bb: ["#94a3b8", "#64748b", "#94a3b8"],
  vwap: ["#e5e7eb"],
  vol: [k_grey, "#f0b90b"],
  rsi: ["#c084fc"],
  macd: ["#60a5fa", "#fb923c", k_grey],
  stoch: ["#60a5fa", "#fb923c"],
  atr: ["#f0b90b"],
};

export interface Tools {
  kind: Kind;
  on: Study[];
  params: Record<Study, number[]>;
}

const k_key = "raikiri.chart.tools";
const k_max_period = 999;
const defaults = (): Tools => ({
  kind: "candles",
  on: ["vol"],
  params: Object.fromEntries(
    k_order.map((s) => [s, [...k_studies[s].params]]),
  ) as Record<Study, number[]>,
});

export function loadTools(): Tools {
  const out = defaults();
  try {
    const saved = JSON.parse(
      localStorage.getItem(k_key) ?? "{}",
    ) as Partial<Tools>;
    if (k_kinds.some(([k]) => k === saved.kind)) out.kind = saved.kind as Kind;
    if (Array.isArray(saved.on))
      out.on = k_order.filter((s) => saved.on?.includes(s));
    for (const s of k_order) {
      const p: unknown = saved.params?.[s];
      const ok = (v: unknown) =>
        typeof v === "number" && v >= 0 && v <= k_max_period;
      if (Array.isArray(p) && p.length === out.params[s].length && p.every(ok))
        out.params[s] = p as number[];
    }
  } catch {
    // a private window or a damaged entry: the defaults stand
  }
  return out;
}

export function saveTools(tools: Tools) {
  try {
    localStorage.setItem(k_key, JSON.stringify(tools));
  } catch {
    // the choice lasts for this page only
  }
}

/** the price series of a kind, added to the price pane */
export function addMain(
  c: IChartApi,
  kind: Kind,
  places: number,
): ISeriesApi<SeriesType> {
  const base = {
    priceLineStyle: LineStyle.Dotted,
    priceFormat: {
      type: "price" as const,
      precision: places,
      minMove: 1 / 10 ** places,
    },
  };
  const candle = {
    ...base,
    upColor: k_up,
    downColor: k_down,
    borderUpColor: k_up,
    borderDownColor: k_down,
    wickUpColor: k_up,
    wickDownColor: k_down,
  };
  switch (kind) {
    case "bars":
      return c.addSeries(BarSeries, {
        ...base,
        upColor: k_up,
        downColor: k_down,
        thinBars: false,
      });
    case "candles":
    case "heikin":
    case "hollow":
      return c.addSeries(CandlestickSeries, candle);
    case "line":
      return c.addSeries(LineSeries, {
        ...base,
        color: k_accent,
        lineWidth: 2,
      });
    case "area":
      return c.addSeries(AreaSeries, {
        ...base,
        lineColor: k_accent,
        topColor: "rgba(56,189,248,0.32)",
        bottomColor: "rgba(56,189,248,0)",
        lineWidth: 2,
      });
    case "baseline":
      return c.addSeries(BaselineSeries, {
        ...base,
        topLineColor: k_up,
        topFillColor1: k_up_soft,
        topFillColor2: "rgba(47,211,166,0.02)",
        bottomLineColor: k_down,
        bottomFillColor1: "rgba(242,85,125,0.02)",
        bottomFillColor2: k_down_soft,
        lineWidth: 2,
      });
  }
}

/** a hollow candle is coloured by its close against the close before and filled only when it fell within the bar; the
 * baseline sits at the first bar's open, the price the header's change is counted from */
export function fillMain(
  s: ISeriesApi<SeriesType>,
  kind: Kind,
  bars: readonly Candle[],
) {
  const time = (t: number) => t as UTCTimestamp;
  if (kind === "line" || kind === "area" || kind === "baseline") {
    s.setData(bars.map((k) => ({ time: time(k.time), value: k.close })));
    const first = bars[0];
    if (kind === "baseline" && first !== undefined)
      (s as ISeriesApi<"Baseline">).applyOptions({
        baseValue: { type: "price", price: first.open },
      });
    return;
  }
  const src = kind === "heikin" ? heikinAshi(bars) : bars;
  s.setData(
    src.map((k, i) => {
      const bar = {
        time: time(k.time),
        open: k.open,
        high: k.high,
        low: k.low,
        close: k.close,
      };
      if (kind !== "hollow") return bar;
      const color = k.close >= (src[i - 1]?.close ?? k.open) ? k_up : k_down;
      return {
        ...bar,
        color: k.close >= k.open ? "transparent" : color,
        borderColor: color,
        wickColor: color,
      };
    }),
  );
}

export interface Plot {
  key: string;
  label: string;
  color: string;
  values: Series;
  /** drawn as columns, each in the colour this gives it */
  column?: (i: number) => string;
}

export interface Group {
  study: Study;
  title: string;
  pane: boolean;
  format: "price" | "volume" | "fixed";
  levels?: readonly number[];
  plots: Plot[];
}

export function groupsOf(bars: readonly Candle[], tools: Tools): Group[] {
  const closes = bars.map((b) => b.close);
  return k_order
    .filter((s) => tools.on.includes(s))
    .map((study) => {
      const { label, pane } = k_studies[study];
      const raw = tools.params[study];
      const p = raw.map((v) => Math.max(1, Math.round(v)));
      const n = (i: number) => p[i] ?? 1;
      const color = (i: number) => k_lines[study][i] ?? k_grey;
      const line = (i: number, values: Series, name = ""): Plot => ({
        key: `${study}${i}`,
        label: name,
        color: color(i),
        values,
      });
      const group = (
        plots: Plot[],
        format: Group["format"],
        title = label,
        levels?: readonly number[],
      ): Group => ({
        study,
        title,
        pane,
        format,
        plots,
        ...(levels === undefined ? {} : { levels }),
      });
      const titled = `${label} ${p.join(" ")}`;
      switch (study) {
        case "ma":
        case "ema": {
          const f = study === "ma" ? sma : ema;
          return group(
            raw.flatMap((v, i) =>
              v >= 1 ? [line(i, f(closes, n(i)), String(n(i)))] : [],
            ),
            "price",
          );
        }
        case "bb": {
          const b = bollinger(closes, n(0), raw[1] ?? 2);
          return group(
            [line(0, b.upper), line(1, b.mid), line(2, b.lower)],
            "price",
            `${label} ${n(0)} ${raw[1] ?? 2}`,
          );
        }
        case "vwap":
          return group([line(0, vwap(bars))], "price");
        case "vol": {
          const ticks = bars.map((b) => b.ticks);
          const column = (i: number) => {
            const b = bars[i];
            return b !== undefined && b.close < b.open
              ? k_down_soft
              : k_up_soft;
          };
          return group(
            [
              { ...line(0, ticks), column },
              line(1, sma(ticks, n(0)), `MA ${n(0)}`),
            ],
            "volume",
          );
        }
        case "rsi":
          return group([line(0, rsi(closes, n(0)))], "fixed", titled, [30, 70]);
        case "macd": {
          const m = macd(closes, n(0), n(1), n(2));
          const column = (i: number) =>
            (m.hist[i] ?? 0) < 0 ? k_down_soft : k_up_soft;
          return group(
            [
              { ...line(2, m.hist), column },
              line(0, m.line),
              line(1, m.signal),
            ],
            "price",
            titled,
          );
        }
        case "stoch": {
          const s = stochRsi(closes, n(0), n(1), n(2), n(3));
          return group([line(0, s.k), line(1, s.d)], "fixed", titled, [20, 80]);
        }
        case "atr":
          return group([line(0, atr(bars, n(0)))], "price", titled);
      }
    });
}

/** what decides which series exist; periods change the data alone */
export const shapeOf = (groups: readonly Group[]) =>
  groups
    .map((g) => `${g.study}:${g.plots.map((p) => p.key).join(",")}`)
    .join(" ");

export function Legend({
  group,
  at,
  places,
  onClose,
}: {
  group: Group;
  at: number;
  places: number;
  onClose: () => void;
}) {
  const text = (v: number | null | undefined) =>
    v === null || v === undefined
      ? "—"
      : group.format === "price"
        ? px(v, places)
        : group.format === "volume"
          ? compact(v)
          : v.toFixed(2);
  return (
    <span className="group/legend pointer-events-auto flex items-center gap-1.5 text-2xs text-fg-dim">
      {group.title}
      {group.plots.map((p) => (
        <span key={p.key} className="mono" style={{ color: p.color }}>
          {p.label !== "" && `${p.label} `}
          {text(p.values[at])}
        </span>
      ))}
      <button
        type="button"
        aria-label={`remove ${group.title}`}
        onClick={onClose}
        className="grid size-4 cursor-pointer place-items-center rounded-sm text-fg-dim opacity-50 transition-opacity group-hover/legend:opacity-100 hover:bg-raised hover:text-fg"
      >
        <Glyph name="close" className="size-3" />
      </button>
    </span>
  );
}

function Drop({
  label,
  title,
  width,
  children,
}: {
  label: ReactNode;
  title: string;
  width: number;
  children: (close: () => void) => ReactNode;
}) {
  const [open, setOpen] = useState(false);
  const box = useRef<HTMLDivElement>(null);
  useDismiss(box, open, setOpen);
  return (
    <div ref={box} className="relative">
      <button
        type="button"
        aria-expanded={open}
        aria-label={title}
        title={title}
        onClick={() => setOpen((o) => !o)}
        className={cn(
          "flex h-7 cursor-pointer items-center gap-1 rounded-md bg-canvas/60 pr-1 pl-2 text-2xs text-fg-mid transition-colors hover:text-fg",
          open && "text-fg",
        )}
      >
        {label}
        <Glyph
          name="down"
          className={cn(
            "size-3 text-fg-dim transition-transform",
            open && "rotate-180",
          )}
        />
      </button>
      {open && (
        <div
          style={{ width }}
          className="v3-pop absolute top-8 left-0 z-50 rounded-md bg-raised p-1 shadow-[0_0_0_1px_var(--color-line),0_20px_50px_-10px_rgb(0_0_0/0.9)]"
        >
          {children(() => setOpen(false))}
        </div>
      )}
    </div>
  );
}

const KindIcon = ({ kind }: { kind: Kind }) => (
  <svg
    viewBox="0 0 16 16"
    fill="none"
    stroke="currentColor"
    strokeWidth={1.25}
    strokeLinecap="round"
    strokeLinejoin="round"
    className="size-3.5 shrink-0"
    aria-hidden="true"
  >
    {k_kinds.find(([k]) => k === kind)?.[2]}
  </svg>
);

export function KindMenu({
  value,
  onChange,
}: {
  value: Kind;
  onChange: (kind: Kind) => void;
}) {
  return (
    <Drop title="chart type" width={156} label={<KindIcon kind={value} />}>
      {(close) =>
        k_kinds.map(([kind, name]) => (
          <button
            key={kind}
            type="button"
            onClick={() => {
              onChange(kind);
              close();
            }}
            className={cn(
              "flex h-7 w-full cursor-pointer items-center gap-2 rounded-sm px-2 text-left text-xs transition-colors",
              kind === value
                ? "bg-hover text-fg"
                : "text-fg-mid hover:bg-hover hover:text-fg",
            )}
          >
            <KindIcon kind={kind} />
            {name}
          </button>
        ))
      }
    </Drop>
  );
}

function Param({
  value,
  whole,
  onChange,
}: {
  value: number;
  whole: boolean;
  onChange: (v: number) => void;
}) {
  const [text, setText] = useState(String(value));
  return (
    <input
      value={text}
      inputMode={whole ? "numeric" : "decimal"}
      aria-label="period"
      onChange={(e) => {
        const next = e.target.value
          .replace(whole ? /\D/g : /[^\d.]/g, "")
          .slice(0, 4);
        setText(next);
        const v = next === "" ? 0 : Number(next);
        if (Number.isFinite(v) && v <= k_max_period) onChange(v);
      }}
      className="mono h-5 w-8 rounded-sm bg-canvas/60 text-center text-2xs text-fg outline-none focus:bg-canvas"
    />
  );
}

export function StudyMenu({
  tools,
  onChange,
}: {
  tools: Tools;
  onChange: (tools: Tools) => void;
}) {
  const toggle = (study: Study, on: boolean) =>
    onChange({
      ...tools,
      on: k_order.filter((s) => (s === study ? on : tools.on.includes(s))),
    });
  const set = (study: Study, i: number, v: number) =>
    onChange({
      ...tools,
      params: {
        ...tools.params,
        [study]: tools.params[study].map((x, j) => (j === i ? v : x)),
      },
    });
  return (
    <Drop
      title="indicators"
      width={236}
      label={
        <>
          Indicators
          {tools.on.length > 0 && (
            <span className="mono text-accent">{tools.on.length}</span>
          )}
        </>
      }
    >
      {() =>
        k_order.map((study, row) => (
          <div
            key={study}
            className={cn(
              "flex h-7 items-center gap-1 px-2",
              k_studies[study].pane &&
                k_studies[k_order[row - 1] ?? study].pane === false &&
                "mt-1 border-t border-line pt-1 h-8",
            )}
          >
            <span className="min-w-0 flex-1">
              <Check
                checked={tools.on.includes(study)}
                onChange={(on) => toggle(study, on)}
              >
                {study === "bb"
                  ? "Bollinger Bands"
                  : study === "vol"
                    ? "Volume"
                    : k_studies[study].label}
              </Check>
            </span>
            {tools.params[study].map((v, i) => (
              <Param
                key={i}
                value={v}
                whole={!(study === "bb" && i === 1)}
                onChange={(x) => set(study, i, x)}
              />
            ))}
          </div>
        ))
      }
    </Drop>
  );
}
