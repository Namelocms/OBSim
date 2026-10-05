import {
  LineStyle,
  createSeriesMarkers,
  type IPriceLine,
  type ISeriesApi,
  type ISeriesMarkersPluginApi,
  type SeriesMarker,
  type SeriesType,
  type Time,
  type UTCTimestamp,
} from 'lightweight-charts';
import type { Bucketer } from './bars';
import type { Frame, PrintKind, PrintTuple } from './protocol';

/* What the chart draws over the candles: the LULD bands and the last close as price lines,
 * and a marker on each bar where a cross printed.
 *
 * Attached to a series from outside, so the chart itself stays a chart. Everything here comes
 * from the frame or the print stream; nothing is inferred.
 */

type CrossKind = Exclude<PrintKind, 'T'>;

const CROSS_STYLE: Record<CrossKind, { text: string; color: string }> = {
  O: { text: 'OPEN', color: '#4b9fd5' },
  C: { text: 'CLOSE', color: '#e0a03c' },
  R: { text: 'REOPEN', color: '#c07ad6' },
};

const LINE_COLORS = {
  band: 'rgba(224, 160, 60, 0.85)',
  limit: '#d64545',
  close: '#6b7787',
};

interface LineSpec { price: number; color: string; title: string; style: LineStyle }

export class ChartOverlays {
  private markers: ISeriesMarkersPluginApi<Time>;
  /** One per bar and kind: a cross prints once per fill, but it is one event */
  private crosses = new Map<string, { time: number; kind: CrossKind; shares: number }>();
  private lines = new Map<string, { line: IPriceLine; spec: string }>();

  constructor(private series: ISeriesApi<SeriesType>, private bucketer: Bucketer) {
    this.markers = createSeriesMarkers(series, []);
  }

  /** The bars changed width. Markers sit on bars, so they are rebuilt from the next backfill. */
  setBucketer(bucketer: Bucketer): void {
    this.bucketer = bucketer;
    this.clearMarkers();
  }

  /** Replace the markers from a whole print history */
  reset(prints: readonly PrintTuple[]): void {
    this.crosses.clear();
    this.add(prints);
  }

  add(prints: readonly PrintTuple[]): void {
    let changed = false;
    for (const print of prints) {
      const kind = print[4];
      if (kind === undefined || kind === 'T') continue;
      // The bar this print falls in. Index-free: the tick bucketer is not wired to markers.
      const time = this.bucketer.timeFor(this.bucketer.keyFor(print, 0), print);
      const key = `${time}|${kind}`;
      const entry = this.crosses.get(key);
      if (entry) entry.shares += print[2];
      else this.crosses.set(key, { time, kind, shares: print[2] });
      changed = true;
    }
    if (changed) this.drawMarkers();
  }

  private drawMarkers(): void {
    const list: SeriesMarker<Time>[] = [...this.crosses.values()]
      .sort((a, b) => a.time - b.time)
      .map((c) => ({
        time: c.time as UTCTimestamp,
        position: 'aboveBar',
        shape: 'circle',
        color: CROSS_STYLE[c.kind].color,
        text: CROSS_STYLE[c.kind].text,
        size: 1,
      }));
    this.markers.setMarkers(list);
  }

  update(f: Frame): void {
    const l = f.market.luld;
    const bandColor = l.limitState ? LINE_COLORS.limit : LINE_COLORS.band;
    this.setLine('upper', l.active ? { price: l.upper, color: bandColor, title: 'LULD', style: LineStyle.Dashed } : null);
    this.setLine('lower', l.active ? { price: l.lower, color: bandColor, title: 'LULD', style: LineStyle.Dashed } : null);
    const close = f.market.official.close;
    this.setLine('close', close > 0 ? { price: close, color: LINE_COLORS.close, title: 'CLOSE', style: LineStyle.Dotted } : null);
  }

  /** Create, move or remove one line, touching the chart only when something changed */
  private setLine(key: string, spec: LineSpec | null): void {
    const existing = this.lines.get(key);
    if (!spec) {
      if (existing) {
        this.series.removePriceLine(existing.line);
        this.lines.delete(key);
      }
      return;
    }
    const id = `${spec.price}|${spec.color}|${spec.title}|${spec.style}`;
    if (existing) {
      if (existing.spec === id) return;
      existing.line.applyOptions({ price: spec.price, color: spec.color, title: spec.title, lineStyle: spec.style });
      existing.spec = id;
      return;
    }
    const line = this.series.createPriceLine({
      price: spec.price, color: spec.color, title: spec.title, lineStyle: spec.style,
      lineWidth: 1, axisLabelVisible: true,
    });
    this.lines.set(key, { line, spec: id });
  }

  clearMarkers(): void {
    this.crosses.clear();
    this.markers.setMarkers([]);
  }

  /** Everything off, for a new run */
  clear(): void {
    this.clearMarkers();
    for (const { line } of this.lines.values()) this.series.removePriceLine(line);
    this.lines.clear();
  }
}
