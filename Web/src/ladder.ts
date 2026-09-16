import type { BookLevelTuple } from './protocol';

/* The depth ladder.
 *
 * Hand drawn on a canvas because no charting library provides one -- a ladder is a table
 * with a bar behind each row, and every library that could draw it would have to be fought
 * to stop it drawing a chart instead.
 *
 * The levels arriving here are aggregated by PRICE, not by order: the server walks the
 * book and sums every order resting at a level. The terminal UI shows the best N orders
 * instead, so several agents quoting one price eat its visible depth. This is the one
 * place that difference is obvious.
 */

const COLORS = {
  bid: '#26a65b',
  ask: '#d64545',
  bidFill: 'rgba(38, 166, 91, 0.18)',
  askFill: 'rgba(214, 69, 69, 0.18)',
  text: '#c8d2e0',
  muted: '#6b7787',
  spread: '#e0a03c',
  background: '#0f141b',
};

export interface LadderData {
  bids: BookLevelTuple[];
  asks: BookLevelTuple[];
  spread: number;
  price: number;
}

export class DepthLadder {
  private canvas: HTMLCanvasElement;
  private ctx: CanvasRenderingContext2D;
  private resizeObserver: ResizeObserver;
  private data: LadderData = { bids: [], asks: [], spread: 0, price: 0 };

  constructor(private container: HTMLElement) {
    this.canvas = document.createElement('canvas');
    this.canvas.className = 'ladder-canvas';
    container.appendChild(this.canvas);
    const ctx = this.canvas.getContext('2d');
    if (!ctx) throw new Error('2d canvas context unavailable');
    this.ctx = ctx;

    this.resizeObserver = new ResizeObserver(() => this.resize());
    this.resizeObserver.observe(container);
    this.resize();
  }

  private resize(): void {
    // Back the canvas at device resolution, or text is soft on any HiDPI screen
    const dpr = window.devicePixelRatio || 1;
    const w = this.container.clientWidth;
    const h = this.container.clientHeight;
    this.canvas.width = Math.max(1, Math.floor(w * dpr));
    this.canvas.height = Math.max(1, Math.floor(h * dpr));
    this.canvas.style.width = `${w}px`;
    this.canvas.style.height = `${h}px`;
    this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    this.draw();
  }

  set(data: LadderData): void {
    this.data = data;
    this.draw();
  }

  private draw(): void {
    const ctx = this.ctx;
    const w = this.container.clientWidth;
    const h = this.container.clientHeight;
    if (w <= 0 || h <= 0) return;

    ctx.fillStyle = COLORS.background;
    ctx.fillRect(0, 0, w, h);

    const { bids, asks } = this.data;
    const rowH = 18;
    const spreadH = 26;
    const headerH = 20;

    // Asks descend to the touch, bids descend from it -- the conventional ladder, best
    // prices meeting in the middle at the spread.
    const available = h - headerH - spreadH;
    const perSide = Math.max(1, Math.floor(available / 2 / rowH));
    const shownAsks = asks.slice(0, perSide).reverse();
    const shownBids = bids.slice(0, perSide);

    let maxVol = 1;
    for (const [, v] of shownAsks) maxVol = Math.max(maxVol, v);
    for (const [, v] of shownBids) maxVol = Math.max(maxVol, v);

    ctx.font = '11px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace';
    ctx.textBaseline = 'middle';

    // Header
    ctx.fillStyle = COLORS.muted;
    ctx.textAlign = 'left';
    ctx.fillText('PRICE', 8, headerH / 2);
    ctx.textAlign = 'right';
    ctx.fillText('SIZE', w - 52, headerH / 2);
    ctx.fillText('ORD', w - 10, headerH / 2);

    const drawRow = (
      level: BookLevelTuple,
      y: number,
      stroke: string,
      fill: string,
    ): void => {
      const [price, volume, orders] = level;
      // Bar drawn from the right, behind the text, so depth reads as a shape without
      // competing with the numbers
      const barW = (volume / maxVol) * (w * 0.62);
      ctx.fillStyle = fill;
      ctx.fillRect(w - barW, y, barW, rowH - 2);

      ctx.fillStyle = stroke;
      ctx.textAlign = 'left';
      ctx.fillText(price.toFixed(4), 8, y + rowH / 2);
      ctx.fillStyle = COLORS.text;
      ctx.textAlign = 'right';
      ctx.fillText(String(volume), w - 52, y + rowH / 2);
      ctx.fillStyle = COLORS.muted;
      ctx.fillText(String(orders), w - 10, y + rowH / 2);
    };

    let y = headerH;
    // Pad so the touch always sits at the spread row even with a thin book
    y += (perSide - shownAsks.length) * rowH;
    for (const level of shownAsks) {
      drawRow(level, y, COLORS.ask, COLORS.askFill);
      y += rowH;
    }

    // Spread band
    ctx.fillStyle = 'rgba(224, 160, 60, 0.10)';
    ctx.fillRect(0, y, w, spreadH);
    ctx.fillStyle = COLORS.spread;
    ctx.textAlign = 'left';
    const spreadText = this.data.spread > 0 ? this.data.spread.toFixed(4) : '--';
    ctx.fillText(`SPREAD ${spreadText}`, 8, y + spreadH / 2);
    ctx.textAlign = 'right';
    ctx.fillStyle = COLORS.text;
    ctx.fillText(this.data.price.toFixed(4), w - 10, y + spreadH / 2);
    y += spreadH;

    for (const level of shownBids) {
      drawRow(level, y, COLORS.bid, COLORS.bidFill);
      y += rowH;
    }

    if (shownAsks.length === 0 && shownBids.length === 0) {
      ctx.fillStyle = COLORS.muted;
      ctx.textAlign = 'center';
      ctx.fillText('no resting orders', w / 2, h / 2);
    }
  }

  destroy(): void {
    this.resizeObserver.disconnect();
    this.canvas.remove();
  }
}
