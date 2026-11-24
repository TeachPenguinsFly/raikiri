import { useEffect, useMemo, useRef, useState } from "react";
import {
  ColorType,
  createChart,
  CrosshairMode,
  HistogramSeries,
  LineSeries,
  LineStyle,
  type IChartApi,
  type ISeriesApi,
  type LineData,
  type SeriesType,
  type UTCTimestamp,
} from "lightweight-charts";
import { cn, venueLabel } from "@raikiri/ui";
import { candles, Empty, Seg, type Candle } from "@raikiri/ui/v3";
import { api } from "../api/client";
import { tapeOf } from "../api/stream";
import { bp, px, touchOf, venueColor } from "../lib/market";
import {
  addMain,
  fillMain,
  groupsOf,
  KindMenu,
  Legend,
  loadTools,
  saveTools,
  shapeOf,
  StudyMenu,
  type Group,
  type Study,
} from "./chart-tools";
import { useTerminal } from "./state";

// one venue's candles: a minute and up come from the history candled keeps since the stack started, with the bar in
// progress drawn on from the quote stream; under a minute only the stream since the page opened draws them. The volume
// pane counts prints in history and moves of the quote after it

type Interval = "1s" | "5s" | "15s" | "1m" | "5m" | "15m" | "1h" | "4h" | "1d";
const k_interval_s: Record<Interval, number> = {
  "1s": 1,
  "5s": 5,
  "15s": 15,
  "1m": 60,
  "5m": 300,
  "15m": 900,
  "1h": 3_600,
  "4h": 14_400,
  "1d": 86_400,
};
const k_history_ms = 30_000;
const k_none: Candle[] = [];

/** the server's bars for a minute and longer, refreshed as minutes close */
const toBar = (c: {
  t: number;
  o: string;
  h: string;
  l: string;
  c: string;
  n: number;
}): Candle => ({
  time: c.t,
  open: Number(c.o),
  high: Number(c.h),
  low: Number(c.l),
  close: Number(c.c),
  ticks: c.n,
});

/** the server's bars for a minute and longer for each instrument, refreshed as minutes close */
/** a venue's closes as a line that breaks where its feed went quiet, rather than a straight run across the gap; a
 * market without trades for a few bars is ordinary, so only a longer silence breaks it. The library draws a segment in
 * its first point's colour and joins across whitespace, so the point before a gap is made transparent */
function broken(bars: readonly Candle[], seconds: number): LineData[] {
  const limit = Math.max(5 * seconds, 300);
  return bars.map((k, i) => {
    const next = bars[i + 1];
    const point: LineData = { time: k.time as UTCTimestamp, value: k.close };
    return next !== undefined && next.time - k.time > limit
      ? { ...point, color: "transparent" }
      : point;
  });
}

function useHistories(
  ids: readonly string[],
  interval: Interval,
): Map<string, Candle[]> {
  const [bars, setBars] = useState<Map<string, Candle[]>>(new Map());
  const key = ids.join(",");
  useEffect(() => {
    setBars(new Map());
    if (ids.length === 0 || k_interval_s[interval] < 60) return;
    let live = true;
    const load = () =>
      Promise.all(
        ids.map((id) =>
          api.candles(id, interval).then(
            (page) => [id, page.candles.map(toBar)] as const,
            () => [id, [] as Candle[]] as const,
          ),
        ),
      ).then((rows) => live && setBars(new Map(rows)));
    void load();
    const t = setInterval(load, k_history_ms);
    return () => {
      live = false;
      clearInterval(t);
    };
  }, [key, interval]);
  return bars;
}

/** history, then the stream's bars on top: a bar both have keeps history's open and takes the stream's close */
function merged(history: Candle[], stream: Candle[]): Candle[] {
  if (history.length === 0) return stream;
  const out = new Map(history.map((b) => [b.time, { ...b }]));
  for (const b of stream) {
    const h = out.get(b.time);
    if (h === undefined) out.set(b.time, b);
    else
      out.set(b.time, {
        ...h,
        high: Math.max(h.high, b.high),
        low: Math.min(h.low, b.low),
        close: b.close,
      });
  }
  return [...out.values()].sort((a, b) => a.time - b.time);
}
const k_max_candles = 3_000;
const k_redraw_ms = 1_000;
const k_pane_stretch = 0.2;

export function ChartWidget() {
  const { pair, leg, quotes, hiddenLines: hidden, toggleLine } = useTerminal();
  const [interval, setSpan] = useState<Interval>("1m");
  const [tools, setTools] = useState(loadTools);
  const kind = tools.kind;
  useEffect(() => saveTools(tools), [tools]);

  const box = useRef<HTMLDivElement>(null);
  const chart = useRef<IChartApi | null>(null);
  const main = useRef<ISeriesApi<SeriesType> | null>(null);
  const studies = useRef(new Map<string, ISeriesApi<"Line" | "Histogram">>());
  const others = useRef(new Map<string, ISeriesApi<"Line">>());
  const [tops, setTops] = useState<number[]>([]);
  const [hover, setHover] = useState<Candle | null>(null);
  const [tick, setTick] = useState(0);
  const places = leg?.places ?? 2;

  // the tape grows between renders; a slow clock redraws from it so a quiet market still closes its candles
  useEffect(() => {
    const t = setInterval(() => setTick((x) => x + 1), k_redraw_ms);
    return () => clearInterval(t);
  }, []);
  const id = leg?.id ?? "";
  const seconds = k_interval_s[interval];
  const live = quotes.get(id);
  const legIds = useMemo(() => (pair?.legs ?? []).map((l) => l.id), [pair]);
  const histories = useHistories(legIds, interval);
  const history = histories.get(id) ?? k_none;
  const bars = useMemo(
    () =>
      merged(
        history,
        id === ""
          ? []
          : candles(tapeOf(id), seconds, Date.now(), k_max_candles),
      ),
    [history, id, seconds, tick, live],
  );
  const rest = useMemo(
    () => (pair?.legs ?? []).filter((l) => l.id !== id && !hidden.has(l.venue)),
    [pair, id, hidden],
  );

  useEffect(() => {
    const el = box.current;
    if (el === null) return;
    const c = createChart(el, {
      autoSize: true,
      layout: {
        background: { type: ColorType.Solid, color: "transparent" },
        textColor: "#6b6b78",
        fontFamily: "'Inter', system-ui, sans-serif",
        fontSize: 10,
        attributionLogo: true,
        panes: {
          separatorColor: "rgba(255,255,255,0.05)",
          separatorHoverColor: "rgba(56,189,248,0.25)",
          enableResize: true,
        },
      },
      grid: {
        vertLines: { color: "rgba(255,255,255,0.025)" },
        horzLines: { color: "rgba(255,255,255,0.035)" },
      },
      rightPriceScale: {
        borderVisible: false,
        scaleMargins: { top: 0.16, bottom: 0.06 },
      },
      timeScale: {
        borderVisible: false,
        timeVisible: true,
        secondsVisible: true,
        rightOffset: 6,
        barSpacing: 8,
      },
      crosshair: {
        mode: CrosshairMode.Normal,
        vertLine: {
          color: "rgba(56,189,248,0.35)",
          width: 1,
          style: LineStyle.Dashed,
          labelBackgroundColor: "#0b3a52",
        },
        horzLine: {
          color: "rgba(56,189,248,0.35)",
          width: 1,
          style: LineStyle.Dashed,
          labelBackgroundColor: "#0b3a52",
        },
      },
    });
    chart.current = c;
    const lines = others.current;
    const drawn = studies.current;
    return () => {
      c.remove();
      chart.current = null;
      main.current = null;
      lines.clear();
      drawn.clear();
    };
  }, []);

  const barsRef = useRef<Candle[]>([]);
  barsRef.current = bars;
  const groups = useMemo(() => groupsOf(bars, tools), [bars, tools]);
  const groupsRef = useRef<Group[]>([]);
  groupsRef.current = groups;
  const shape = shapeOf(groups);

  useEffect(() => {
    const c = chart.current;
    if (c === null) return;
    // the new series goes in before the old leaves, or the emptied price pane closes and the first pane under it takes
    // its place
    const old = main.current;
    main.current = addMain(c, kind, places);
    if (old !== null) c.removeSeries(old);
    const onMove = (
      p: Parameters<Parameters<IChartApi["subscribeCrosshairMove"]>[0]>[0],
    ) => {
      if (p.time === undefined) return setHover(null);
      setHover(barsRef.current.find((x) => x.time === p.time) ?? null);
    };
    c.subscribeCrosshairMove(onMove);
    return () => c.unsubscribeCrosshairMove(onMove);
  }, [kind, places]);

  useEffect(() => {
    if (main.current !== null) fillMain(main.current, kind, bars);
  }, [bars, kind, places]);

  // the indicators' series are rebuilt only when the set of them changes: panes are numbered by position, so a pane
  // closing in the middle would otherwise leave the ones after it pointing one too far
  useEffect(() => {
    const c = chart.current;
    if (c === null) return;
    const made = studies.current;
    const price = {
      type: "price" as const,
      precision: places,
      minMove: 1 / 10 ** places,
    };
    let pane = 0;
    for (const g of groupsRef.current) {
      const at = g.pane ? ++pane : 0;
      const priceFormat =
        g.format === "price"
          ? price
          : g.format === "volume"
            ? { type: "volume" as const }
            : { type: "price" as const, precision: 2, minMove: 0.01 };
      const quiet = {
        priceFormat,
        priceLineVisible: false,
        lastValueVisible: g.pane,
        crosshairMarkerVisible: false,
      };
      g.plots.forEach((p, i) => {
        const s =
          p.column !== undefined
            ? c.addSeries(
                HistogramSeries,
                { ...quiet, lastValueVisible: false },
                at,
              )
            : c.addSeries(
                LineSeries,
                { ...quiet, color: p.color, lineWidth: 1 },
                at,
              );
        if (g.pane && i === 0) {
          s.priceScale().applyOptions({
            scaleMargins: {
              top: 0.24,
              bottom: g.format === "volume" ? 0 : 0.08,
            },
          });
          for (const level of g.levels ?? [])
            s.createPriceLine({
              price: level,
              color: "rgba(255,255,255,0.14)",
              lineWidth: 1,
              lineStyle: LineStyle.Dashed,
              axisLabelVisible: false,
            });
        }
        made.set(p.key, s);
      });
    }
    c.panes().forEach((p, i) =>
      p.setStretchFactor(i === 0 ? 1 : k_pane_stretch),
    );
    return () => {
      // on the way out the chart is already gone, and its series with it
      if (chart.current === c) for (const s of made.values()) c.removeSeries(s);
      made.clear();
    };
  }, [shape, places]);

  useEffect(() => {
    const c = chart.current;
    if (c === null) return;
    for (const g of groups)
      for (const p of g.plots)
        studies.current.get(p.key)?.setData(
          p.values.flatMap((value, i) => {
            const time = bars[i]?.time;
            if (value === null || time === undefined) return [];
            const point = { time: time as UTCTimestamp, value };
            return [
              p.column === undefined ? point : { ...point, color: p.column(i) },
            ];
          }),
        );
    // where each pane starts, for the legends laid over them; a pane dragged taller is picked up on the next redraw
    const next: number[] = [];
    let y = 0;
    c.panes().forEach((pane) => {
      next.push(y);
      y += pane.getHeight() + 1;
    });
    setTops((was) =>
      was.length === next.length && was.every((v, i) => v === next[i])
        ? was
        : next,
    );
  }, [groups, shape, places]);

  // the other venues' mids as lines over the candles, where they sit apart shows at a glance
  useEffect(() => {
    const c = chart.current;
    if (c === null) return;
    const keep = new Set(rest.map((l) => l.id));
    for (const [lid, s] of others.current) {
      if (keep.has(lid)) continue;
      c.removeSeries(s);
      others.current.delete(lid);
    }
    for (const l of rest) {
      let s = others.current.get(l.id);
      if (s === undefined) {
        s = c.addSeries(LineSeries, {
          color: venueColor(l.venue),
          lineWidth: 2,
          priceLineVisible: false,
          lastValueVisible: true,
          crosshairMarkerVisible: false,
          priceFormat: {
            type: "price",
            precision: places,
            minMove: 1 / 10 ** places,
          },
        });
        others.current.set(l.id, s);
      }
      s.setData(
        broken(
          merged(
            histories.get(l.id) ?? k_none,
            candles(tapeOf(l.id), seconds, Date.now(), k_max_candles),
          ),
          seconds,
        ),
      );
    }
  }, [rest, seconds, tick, places, histories]);

  const zoom = (k: number) => {
    const ts = chart.current?.timeScale();
    const r = ts?.getVisibleLogicalRange();
    if (ts === undefined || r === null || r === undefined) return;
    ts.setVisibleLogicalRange({ from: r.to - (r.to - r.from) * k, to: r.to });
  };

  const hovered =
    hover === null ? -1 : bars.findIndex((b) => b.time === hover.time);
  const at = hovered === -1 ? bars.length - 1 : hovered;
  const drop = (study: Study) =>
    setTools((t) => ({ ...t, on: t.on.filter((s) => s !== study) }));
  const overlays = groups.filter((g) => !g.pane);
  const under = groups.filter((g) => g.pane);

  const shown = hover ?? bars.at(-1);
  const firstOpen = bars[0]?.open;
  const change =
    shown !== undefined && firstOpen !== undefined
      ? shown.close - firstOpen
      : undefined;
  const touch =
    leg === undefined ? undefined : touchOf(quotes.get(leg.id), leg);

  return (
    <div className="flex h-full min-h-0 flex-col">
      <div className="flex shrink-0 flex-wrap items-center gap-1.5 px-2 py-1.5">
        <Seg
          size="xs"
          value={interval}
          onChange={setSpan}
          options={(Object.keys(k_interval_s) as Interval[]).map(
            (i) => [i, i] as const,
          )}
        />
        <KindMenu
          value={kind}
          onChange={(next) => setTools((t) => ({ ...t, kind: next }))}
        />
        <StudyMenu tools={tools} onChange={setTools} />
        <span className="flex-1" />
        <div className="flex items-center gap-0.5">
          {(
            [
              ["−", "zoom out", () => zoom(1.25)],
              ["+", "zoom in", () => zoom(0.8)],
              [
                "⟲",
                "show everything",
                () => chart.current?.timeScale().fitContent(),
              ],
            ] as const
          ).map(([text, label, run]) => (
            <button
              key={label}
              type="button"
              aria-label={label}
              title={label}
              onClick={run}
              className="grid size-6 cursor-pointer place-items-center rounded-md text-xs text-fg-dim transition-colors hover:bg-raised hover:text-fg"
            >
              {text}
            </button>
          ))}
        </div>
      </div>
      <div className="relative min-h-0 flex-1">
        <div className="pointer-events-none absolute top-1 left-3 z-10 flex flex-col gap-0.5">
          <span className="truncate text-2xs text-fg-dim">
            {pair === undefined || leg === undefined
              ? ""
              : `${venueLabel(leg.venue)} · ${leg.exchange} · ${pair.market === "perp" ? "perpetual" : "spot"}`}
          </span>
          <span className="flex items-baseline gap-2">
            <span className="text-xs font-medium text-fg">{pair?.base}</span>
            <span className="mono text-lg font-semibold text-fg">
              {px(shown?.close, places)}
            </span>
            {change !== undefined && firstOpen !== undefined && (
              <span
                className={cn(
                  "mono text-xs",
                  change > 0
                    ? "text-up"
                    : change < 0
                      ? "text-down"
                      : "text-fg-dim",
                )}
              >
                {change > 0 ? "+" : change < 0 ? "−" : ""}
                {px(Math.abs(change), places)} ·{" "}
                {bp((change / firstOpen) * 1e4)}
              </span>
            )}
          </span>
          {shown !== undefined && (
            <span className="mono flex gap-2 text-xs text-fg-dim">
              {(["open", "high", "low", "close"] as const).map((f) => (
                <span key={f}>
                  {f[0]?.toUpperCase()}{" "}
                  <span className="text-fg-mid">{px(shown[f], places)}</span>
                </span>
              ))}
              <span>
                moves <span className="text-fg-mid">{shown.ticks}</span>
              </span>
              {touch !== undefined && (
                <span>
                  spread{" "}
                  <span className="text-fg-mid">
                    {bp(((touch.ask - touch.bid) / touch.mid) * 1e4, 2)}
                  </span>
                </span>
              )}
            </span>
          )}
          {overlays.length > 0 && (
            <span className="flex flex-wrap gap-x-3 gap-y-0.5">
              {overlays.map((g) => (
                <Legend
                  key={g.study}
                  group={g}
                  at={at}
                  places={places}
                  onClose={() => drop(g.study)}
                />
              ))}
            </span>
          )}
          {rest.length > 0 && (
            <span className="pointer-events-auto mt-1 flex flex-wrap gap-x-3 gap-y-0.5 text-xs">
              {rest.map((l) => (
                <button
                  key={l.venue}
                  type="button"
                  title="Hide this line"
                  onClick={() => toggleLine(l.venue)}
                  className="flex items-center gap-1.5 text-fg-mid hover:text-fg"
                >
                  <span
                    className="h-0.5 w-3 rounded-full"
                    style={{ background: venueColor(l.venue) }}
                  />
                  {venueLabel(l.venue)}
                  <span className="mono text-fg-dim">
                    {px(touchOf(quotes.get(l.id), l)?.mid, l.places)}
                  </span>
                </button>
              ))}
            </span>
          )}
        </div>
        {under.map((g, i) => {
          const top = tops[i + 1];
          return (
            top !== undefined && (
              <div
                key={g.study}
                className="pointer-events-none absolute left-3 z-10"
                style={{ top: top + 3 }}
              >
                <Legend
                  group={g}
                  at={at}
                  places={places}
                  onClose={() => drop(g.study)}
                />
              </div>
            )
          );
        })}
        <div ref={box} className="absolute inset-0" />
        {pair === undefined && (
          <div className="absolute inset-0 grid place-items-center bg-panel">
            <Empty title="Pick a market" />
          </div>
        )}
        {pair !== undefined && bars.length === 0 && (
          <div className="absolute inset-0 grid place-items-center">
            <Empty title="Waiting for the first quotes" />
          </div>
        )}
      </div>
    </div>
  );
}
