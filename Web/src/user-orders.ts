import './trading.css';

import { el } from './dom';
import { fmt } from './format';
import type { ClientMessage, Frame, UserFill, UserOrder, UserResult } from './protocol';

/* The user's working orders, with cancel and replace, and their recent fills
 * (OrderModelPlan Step 4.2). A replace is for a working limit order's price and size, as at
 * an exchange; a held stop is cancelled and placed again instead.
 */

const FILLS_KEPT = 60;

const describe = (o: UserOrder): string => {
  const side = o.side === 'B' ? 'Buy' : o.short ? 'Short' : 'Sell';
  const what = o.type === 'market' ? 'MKT'
    : o.type === 'limit' ? `LMT ${fmt.price(o.price)}`
      : o.type === 'stop' ? `STP ${fmt.price(o.stopPrice)}`
        : o.type === 'stopLimit' ? `STP ${fmt.price(o.stopPrice)} LMT ${fmt.price(o.price)}`
          : o.trailPercent > 0 ? `TRAIL ${o.trailPercent.toFixed(1)}% @ ${fmt.price(o.stopPrice)}` : `TRAIL $${o.trailAmount} @ ${fmt.price(o.stopPrice)}`;
  const flags = [o.hidden ? 'hidden' : '', o.displayQty > 0 ? `shows ${o.displayQty}` : '', o.midpointPeg ? 'peg' : '',
    o.extendedHours ? 'ext' : '', o.group ? o.group : ''].filter((x) => x).join(' ');
  return `${side} ${fmt.int(o.qty)}${o.qty !== o.entered ? `/${fmt.int(o.entered)}` : ''} ${what} ${o.tif}${flags ? ` - ${flags}` : ''}`;
};

export class UserOrders {
  readonly root = el('div', 'panel tr-panel user-orders');
  private ordersBody = el('div', 'tr-orders');
  private fillsBody = el('div', 'tr-fills');
  private note = el('div', 'tr-status');
  private lastKey = '';
  private fills: UserFill[] = [];
  private editing: string | null = null;
  private counter = 0;

  constructor(private send: (message: ClientMessage) => void) {
    const head = el('div', 'panel-head');
    head.append(el('span', 'panel-title', 'Orders and fills'));
    this.root.append(head, this.ordersBody, this.note, el('div', 'tr-subhead', 'Fills'), this.fillsBody);
  }

  private requestId(): string {
    this.counter += 1;
    return `orders-${this.counter}`;
  }

  private renderOrders(orders: UserOrder[]): void {
    if (orders.length === 0) { this.ordersBody.replaceChildren(el('div', 'tr-empty', 'No working orders')); return; }
    this.ordersBody.replaceChildren(...orders.map((o) => {
      const row = el('div', `tr-order tr-order-${o.state}`);
      row.append(el('span', 'tr-order-text', describe(o)), el('span', 'tr-order-state', o.state));
      if (this.editing === o.id && o.type === 'limit' && o.state === 'working') {
        const price = el('input', 'tr-input tr-narrow');
        price.type = 'number'; price.step = '0.01'; price.value = String(o.price);
        const qty = el('input', 'tr-input tr-narrow');
        qty.type = 'number'; qty.step = '1'; qty.value = String(o.qty);
        const go = el('button', 'btn btn-narrow', 'Send');
        go.addEventListener('click', () => {
          this.editing = null;
          this.send({ type: 'replace', orderId: o.id, limitPrice: Number(price.value), qty: Math.floor(Number(qty.value)), id: this.requestId() });
        });
        row.append(price, qty, go);
      }
      else if (o.type === 'limit' && o.state === 'working') {
        const edit = el('button', 'btn btn-narrow', 'Edit');
        edit.addEventListener('click', () => { this.editing = o.id; this.lastKey = ''; });
        row.append(edit);
      }
      const cancel = el('button', 'btn btn-narrow', 'Cancel');
      cancel.addEventListener('click', () => this.send({ type: 'cancel', orderId: o.id, id: this.requestId() }));
      row.append(cancel);
      return row;
    }));
  }

  private renderFills(): void {
    if (this.fills.length === 0) { this.fillsBody.replaceChildren(el('div', 'tr-empty', 'No fills yet')); return; }
    this.fillsBody.replaceChildren(...this.fills.slice().reverse().map((x) => {
      const row = el('div', `tr-fill ${x.side === 'B' ? 'up' : 'down'}`);
      row.append(
        el('span', 'tr-fill-time', fmt.clock(x.simTimeMs)),
        el('span', '', `${x.side === 'B' ? 'Bought' : x.short ? 'Shorted' : 'Sold'} ${fmt.int(x.qty)} @ ${fmt.price(x.price)}`),
        el('span', 'tr-fill-fee', `${x.fee !== 0 ? `fee ${fmt.money(x.fee)}` : ''} ${x.liquidity}`),
      );
      return row;
    }));
  }

  /** A cancel or replace this panel sent was refused: say why */
  handleResult(r: UserResult): void {
    if (!r.echo || !r.echo.startsWith('orders-')) return;
    this.note.textContent = r.accepted ? '' : `${r.command} refused: ${r.reason ?? ''}`;
    this.note.className = `tr-status${r.accepted ? '' : ' tr-refused'}`;
  }

  update(f: Frame): void {
    const u = f.user;
    if (!u) return;
    // Rebuilt only when the orders actually changed, so an edit box keeps its focus
    const key = JSON.stringify(u.orders) + (this.editing ?? '');
    if (key !== this.lastKey) {
      this.lastKey = key;
      if (this.editing && !u.orders.some((o) => o.id === this.editing)) this.editing = null;
      this.renderOrders(u.orders);
    }
    if (u.fills.length > 0) {
      this.fills.push(...u.fills);
      if (this.fills.length > FILLS_KEPT) this.fills.splice(0, this.fills.length - FILLS_KEPT);
      this.renderFills();
    }
  }

  clear(): void {
    this.fills = [];
    this.lastKey = '';
    this.editing = null;
    this.note.textContent = '';
    this.renderFills();
  }
}
