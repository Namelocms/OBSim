import {
  CandlestickSeries,
  HistogramSeries,
  createChart,
  type IChartApi,
  type ISeriesApi,
  type UTCTimestamp,
} from 'lightweight-charts';
import type { Bar } from './bars';

/* The price chart -- candles plus a volume histogram, on a real time axis.
 *
 * The axis is genuinely time based: a sim day is 1440 minutes, so it maps 1:1 onto a
 * calendar day and every session boundary lands on the wall clock time it is named for
 * (premarket 04:00, regular 09:30, close 16:00). A skipped overnight is a real gap in the
 * data rather than missing data, and Lightweight Charts simply does not plot the empty
 * span -- which is the behaviour we want.
 */

const COLORS = {
  up: '#26a65b',
  down: '#d64545',
  upFill: 'rgba(38, 166, 91, 0.45)',
  downFill: 'rgba(214, 69, 69, 0.45)',
  grid: '#1e2530',
  text: '#8b97a8',
  background: '#0f141b',
};

export class PriceChart {
  private chart: IChartApi;
  private candles: ISeriesApi<'Candlestick'>;
  private volume: ISeriesApi<'Histogram'>;
  private resizeObserver: ResizeObserver;
  /** Bars already handed to the chart, so an update can be one point rather than all */
  private lastTime: number | null = null;
  private followingEdge = true;

  constructor(private container: HTMLElement) {
    this.chart = createChart(container, {
      layout: {
        background: { color: COLORS.background },
        textColor: COLORS.text,
        fontSize: 11,
        attributionLogo: true,
      },
      grid: {
        vertLines: { color: COLORS.grid },
        horzLines: { color: COLORS.grid },
      },
      rightPriceScale: { borderColor: COLORS.grid, scaleMargins: { top: 0.08, bottom: 0.28 } },
      timeScale: {
        borderColor: COLORS.grid,
        timeVisible: true,
        secondsVisible: true,
        rightOffset: 4,
      },
      crosshair: { mode: 0 },
      autoSize: false,
      width: container.clientWidth,
      height: container.clientHeight,
    });

    this.candles = this.chart.addSeries(CandlestickSeries, {
      upColor: COLORS.up,
      downColor: COLORS.down,
      borderUpColor: COLORS.up,
      borderDownColor: COLORS.down,
      wickUpColor: COLORS.up,
      wickDownColor: COLORS.down,
      priceFormat: { type: 'price', precision: 4, minMove: 0.0001 },
    });

    // Volume shares the pane but on its own invisible scale, pinned to the lower quarter
    this.volume = this.chart.addSeries(HistogramSeries, {
      priceScaleId: 'volume',
      priceFormat: { type: 'volume' },
    });
    this.chart.priceScale('volume').applyOptions({
      scaleMargins: { top: 0.78, bottom: 0 },
      visible: false,
    });

    // Stop auto-scrolling once the user pans away, and resume when they return to the
    // edge. Nothing is more irritating than a chart that yanks itself back while you read.
    this.chart.timeScale().subscribeVisibleLogicalRangeChange(() => {
      const scroll = this.chart.timeScale().scrollPosition();
      this.followingEdge = scroll >= -2;
    });

    this.resizeObserver = new ResizeObserver(() => {
      this.chart.resize(this.container.clientWidth, this.container.clientHeight);
    });
    this.resizeObserver.observe(container);
  }

  /** The candles, for whatever draws on them (chart-overlays.ts) */
  get priceSeries(): ISeriesApi<'Candlestick'> {
    return this.candles;
  }

  /** Replace the whole series. Used for backfill and on a timeframe change. */
  setAll(bars: readonly Bar[]): void {
    this.candles.setData(
      bars.map((b) => ({
        time: b.time as UTCTimestamp,
        open: b.open,
        high: b.high,
        low: b.low,
        close: b.close,
      })),
    );
    this.volume.setData(
      bars.map((b) => ({
        time: b.time as UTCTimestamp,
        value: b.volume,
        color: b.close >= b.open ? COLORS.upFill : COLORS.downFill,
      })),
    );
    const last = bars[bars.length - 1];
    this.lastTime = last ? last.time : null;
    if (bars.length > 0) this.chart.timeScale().scrollToRealTime();
  }

  /** Update or append the newest bar only. The hot path: called on every frame. */
  update(bar: Bar): void {
    // update() on a time earlier than the series' last point throws rather than being
    // ignored, so guard instead of relying on the caller never getting it wrong.
    if (this.lastTime !== null && bar.time < this.lastTime) return;
    this.candles.update({
      time: bar.time as UTCTimestamp,
      open: bar.open,
      high: bar.high,
      low: bar.low,
      close: bar.close,
    });
    this.volume.update({
      time: bar.time as UTCTimestamp,
      value: bar.volume,
      color: bar.close >= bar.open ? COLORS.upFill : COLORS.downFill,
    });
    this.lastTime = bar.time;
    if (this.followingEdge) this.chart.timeScale().scrollToRealTime();
  }

  clear(): void {
    this.candles.setData([]);
    this.volume.setData([]);
    this.lastTime = null;
    this.followingEdge = true;
  }

  destroy(): void {
    this.resizeObserver.disconnect();
    this.chart.remove();
  }
}
