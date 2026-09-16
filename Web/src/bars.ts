import type { PrintTuple } from './protocol';

/* Step 3.1 -- turning a trade stream into bars, on the client.
 *
 * The engine sends prints and nothing else: no candles, no timeframes, no indicators. That
 * is the whole design. It means any timeframe is available without asking the server, and
 * it means the tick-count bars the terminal UI has (10t/50t/100t...) can be added later as
 * another Bucketer rather than as an engine change.
 *
 * Which is why bucketing is an interface rather than a division. SimTimeBucketer is the
 * only implementation today; TickCountBucketer is the deferred feature, and it is a dozen
 * lines here instead of a protocol revision.
 */

export interface Bar {
  /** Bar start, in epoch seconds. Lightweight Charts wants seconds and strict ascent. */
  time: number;
  open: number;
  high: number;
  low: number;
  close: number;
  volume: number;
  /** Trades in this bar, not shares -- useful for spotting thin bars at a glance */
  trades: number;
  /** Share of volume where a buyer crossed the spread, 0..1 */
  buyShare: number;
}

export interface Bucketer {
  /** A stable key for the bar this print belongs to, ascending with time. */
  keyFor(print: PrintTuple, index: number): number;
  /** The bar's `time`, in epoch seconds, for a given key. */
  timeFor(key: number, firstPrint: PrintTuple): number;
  readonly label: string;
}

/** Fixed spans of SIM time. The default, and the only one wired up today. */
export class SimTimeBucketer implements Bucketer {
  constructor(public readonly intervalSec: number, public readonly label: string) {}
  keyFor(print: PrintTuple): number {
    return Math.floor(print[0] / this.intervalSec);
  }
  timeFor(key: number): number {
    return key * this.intervalSec;
  }
}

/* Fixed counts of trades, the terminal UI's model. NOT wired to the UI yet -- it is here
 * because the plan defers tick bars rather than dropping them, and leaving the seam
 * visible is cheaper than rediscovering it.
 *
 * Note the honest wart: a bar's `time` is then the time of its first print, so bars are
 * unevenly spaced on a time axis. That is inherent to tick bars on a time-based chart and
 * is the reason they are deferred rather than shipped alongside. */
export class TickCountBucketer implements Bucketer {
  constructor(public readonly perBar: number, public readonly label: string) {}
  keyFor(_print: PrintTuple, index: number): number {
    return Math.floor(index / this.perBar);
  }
  timeFor(_key: number, firstPrint: PrintTuple): number {
    return firstPrint[0];
  }
}

export const TIMEFRAMES: SimTimeBucketer[] = [
  new SimTimeBucketer(1, '1s'),
  new SimTimeBucketer(5, '5s'),
  new SimTimeBucketer(15, '15s'),
  new SimTimeBucketer(60, '1m'),
  new SimTimeBucketer(300, '5m'),
  new SimTimeBucketer(900, '15m'),
  new SimTimeBucketer(3600, '1h'),
];

export class BarSeries {
  private bars: Bar[] = [];
  private currentKey: number | null = null;
  /** Running index across every print ever folded in, for count-based bucketers */
  private printIndex = 0;
  /** Buy-side share volume of the bar in progress, to derive buyShare on close */
  private buyVolume = 0;

  constructor(private bucketer: Bucketer, private maxBars = 5000) {}

  get all(): readonly Bar[] {
    return this.bars;
  }

  get last(): Bar | undefined {
    return this.bars[this.bars.length - 1];
  }

  get label(): string {
    return this.bucketer.label;
  }

  /** Throw everything away and rebuild from a full print stream. */
  reset(bucketer: Bucketer, prints: readonly PrintTuple[] = []): void {
    this.bucketer = bucketer;
    this.bars = [];
    this.currentKey = null;
    this.printIndex = 0;
    this.buyVolume = 0;
    this.add(prints);
  }

  /** Fold prints in. Returns true when a NEW bar was started, which tells the chart
   *  whether it can update just the last bar or has to append. */
  add(prints: readonly PrintTuple[]): boolean {
    let openedBar = false;
    for (const print of prints) {
      const key = this.bucketer.keyFor(print, this.printIndex);
      this.printIndex += 1;
      const [, price, volume, side] = print;

      if (this.currentKey === null || key !== this.currentKey) {
        // Guard against a print arriving for a bar we have already closed. Frames are
        // ordered and the server sends prints oldest first, so this should not happen --
        // but folding an out-of-order print into the previous bar would corrupt the
        // series silently, and dropping it is visibly harmless.
        if (this.currentKey !== null && key < this.currentKey) continue;

        this.currentKey = key;
        this.buyVolume = side === 'B' ? volume : 0;
        this.bars.push({
          time: this.bucketer.timeFor(key, print),
          open: price,
          high: price,
          low: price,
          close: price,
          volume,
          trades: 1,
          buyShare: side === 'B' ? 1 : 0,
        });
        openedBar = true;
        if (this.bars.length > this.maxBars) this.bars.shift();
        continue;
      }

      const bar = this.bars[this.bars.length - 1];
      if (!bar) continue;
      bar.close = price;
      if (price > bar.high) bar.high = price;
      if (price < bar.low) bar.low = price;
      bar.volume += volume;
      bar.trades += 1;
      if (side === 'B') this.buyVolume += volume;
      bar.buyShare = bar.volume > 0 ? this.buyVolume / bar.volume : 0;
    }
    return openedBar;
  }
}
