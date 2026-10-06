import './trading.css';

import { el, setText } from './dom';
import { fmt } from './format';
import type { Frame, OrderMessage, TimeInForce, UserOrderType, UserResult } from './protocol';

/* The order ticket (OrderModelPlan Step 4.2).
 *
 * Everything the broker takes: buy, sell or sell short; market, limit, stop, stop-limit or
 * trailing; every time in force but GTD (which needs a sim-time expiry this ticket has no
 * picker for); extended hours; post-only, hidden, reserve and midpoint peg; an attached
 * bracket or OCO. It only builds the message. Whether the account can carry the order is the
 * broker's to decide, and its answer comes back as a userResult, shown under the button.
 */

type TicketSide = OrderMessage['side'];

const TYPES: [UserOrderType, string][] = [
  ['market', 'Market'], ['limit', 'Limit'], ['stop', 'Stop'], ['stopLimit', 'Stop limit'], ['trailingStop', 'Trailing stop'],
];
const TIFS: [TimeInForce, string][] = [
  ['DAY', 'Day'], ['GTC', 'Good till cancelled'], ['IOC', 'Immediate or cancel'], ['FOK', 'Fill or kill'],
  ['OPG', 'On the open'], ['CLS', 'On the close'],
];

export class OrderTicket {
  readonly root = el('div', 'panel tr-panel order-ticket');
  private side: TicketSide = 'buy';
  private sideButtons = new Map<TicketSide, HTMLButtonElement>();
  private type = el('select', 'tr-input');
  private qty = el('input', 'tr-input');
  private limit = el('input', 'tr-input');
  private stop = el('input', 'tr-input');
  private trail = el('input', 'tr-input');
  private trailUnit = el('select', 'tr-input tr-narrow');
  private tif = el('select', 'tr-input');
  private extended = el('input');
  private postOnly = el('input');
  private hidden = el('input');
  private display = el('input', 'tr-input');
  private peg = el('input');
  private attach = el('select', 'tr-input');
  private takeProfit = el('input', 'tr-input');
  private stopLoss = el('input', 'tr-input');
  private ocoStop = el('input', 'tr-input');
  private ocoLimit = el('input', 'tr-input');
  private rows = new Map<string, HTMLElement>();
  private estimate = el('div', 'tr-estimate');
  private submit = el('button', 'btn btn-primary tr-submit', 'Place order');
  private status = el('div', 'tr-status');
  private lastPrice = 0;
  private pending = new Map<string, string>();
  private counter = 0;

  constructor(private send: (order: OrderMessage) => void) {
    const head = el('div', 'panel-head');
    head.append(el('span', 'panel-title', 'Order ticket'));
    const body = el('div', 'tr-body');
    this.root.append(head, body);

    const sides = el('div', 'tr-sides');
    for (const [value, label] of [['buy', 'Buy'], ['sell', 'Sell'], ['sellShort', 'Sell short']] as const) {
      const b = el('button', `btn tr-side tr-side-${value}`, label);
      b.type = 'button';
      b.addEventListener('click', () => { this.side = value; this.refresh(); });
      this.sideButtons.set(value, b);
      sides.append(b);
    }
    body.append(sides);

    for (const [v, l] of TYPES) { const o = el('option', '', l); o.value = v; this.type.append(o); }
    for (const [v, l] of TIFS) { const o = el('option', '', l); o.value = v; this.tif.append(o); }
    for (const [v, l] of [['percent', '%'], ['amount', '$']]) { const o = el('option', '', l); o.value = v!; this.trailUnit.append(o); }
    for (const [v, l] of [['none', 'Nothing'], ['bracket', 'Bracket: take-profit and stop-loss'], ['oco', 'OCO: a stop beside the limit']]) {
      const o = el('option', '', l); o.value = v!; this.attach.append(o);
    }
    for (const input of [this.qty, this.limit, this.stop, this.trail, this.display, this.takeProfit, this.stopLoss, this.ocoStop, this.ocoLimit]) {
      input.type = 'number';
      input.min = '0';
    }
    this.qty.step = '1';
    this.qty.value = '100';
    for (const input of [this.limit, this.stop, this.takeProfit, this.stopLoss, this.ocoStop, this.ocoLimit, this.trail]) { input.step = '0.01'; }
    this.display.step = '1';
    for (const box of [this.extended, this.postOnly, this.hidden, this.peg]) { box.type = 'checkbox'; }

    body.append(
      this.row('type', 'Type', this.type),
      this.row('qty', 'Shares', this.qty),
      this.row('limit', 'Limit price', this.limit),
      this.row('stop', 'Stop price', this.stop),
      this.row('trail', 'Trail by', this.trail, this.trailUnit),
      this.row('tif', 'Time in force', this.tif),
      this.check('extended', 'Extended hours (04:00-20:00)', this.extended),
    );

    const advanced = el('details', 'tr-advanced');
    advanced.append(el('summary', '', 'Flags and attachments'));
    advanced.append(
      this.check('postOnly', 'Post-only: never take liquidity', this.postOnly),
      this.check('hidden', 'Hidden: not displayed', this.hidden),
      this.row('display', 'Show only', this.display),
      this.check('peg', 'Midpoint peg', this.peg),
      this.row('attach', 'Attach', this.attach),
      this.row('takeProfit', 'Take-profit', this.takeProfit),
      this.row('stopLoss', 'Stop-loss', this.stopLoss),
      this.row('ocoStop', 'OCO stop', this.ocoStop),
      this.row('ocoLimit', 'OCO stop limit', this.ocoLimit),
    );
    body.append(advanced, this.estimate, this.submit, this.status);

    for (const control of [this.type, this.attach, this.qty, this.limit, this.trailUnit]) {
      control.addEventListener('input', () => this.refresh());
    }
    this.submit.addEventListener('click', () => this.place());
    this.refresh();
  }

  private row(key: string, label: string, ...controls: HTMLElement[]): HTMLElement {
    const r = el('label', 'tr-row');
    r.append(el('span', 'tr-label', label), ...controls);
    this.rows.set(key, r);
    return r;
  }

  private check(key: string, label: string, box: HTMLInputElement): HTMLElement {
    const r = el('label', 'tr-check');
    box.setAttribute('aria-label', label);
    r.append(box, el('span', '', label));
    this.rows.set(key, r);
    return r;
  }

  private show(key: string, visible: boolean): void {
    const r = this.rows.get(key);
    if (r) r.hidden = !visible;
  }

  /** Show only the fields this kind of order uses */
  private refresh(): void {
    for (const [side, b] of this.sideButtons) b.classList.toggle('active', side === this.side);
    const t = this.type.value as UserOrderType;
    const limit = t === 'limit' || t === 'stopLimit';
    this.show('limit', limit);
    this.show('stop', t === 'stop' || t === 'stopLimit');
    this.show('trail', t === 'trailingStop');
    const attach = this.attach.value;
    this.show('takeProfit', attach === 'bracket');
    this.show('stopLoss', attach === 'bracket');
    this.show('ocoStop', attach === 'oco');
    this.show('ocoLimit', attach === 'oco');
    if (limit && this.limit.value === '' && this.lastPrice > 0) this.limit.value = this.lastPrice.toFixed(this.lastPrice < 1 ? 4 : 2);

    const qty = Math.floor(Number(this.qty.value));
    const price = limit ? Number(this.limit.value) : this.lastPrice;
    setText(this.estimate, qty > 0 && price > 0
      ? `About ${fmt.money(qty * price)} at ${limit ? 'the limit' : 'the last price'}`
      : '');
    this.submit.className = `btn btn-primary tr-submit tr-submit-${this.side}`;
    this.submit.textContent = this.side === 'buy' ? 'Place buy order' : this.side === 'sell' ? 'Place sell order' : 'Place short sale';
  }

  /** Build the message from the fields. Any field it has to refuse, it says which. */
  private build(): OrderMessage | string {
    const orderType = this.type.value as UserOrderType;
    const qty = Number(this.qty.value);
    if (!Number.isInteger(qty) || qty <= 0) return 'Shares must be a whole number above zero';
    const order: OrderMessage = { type: 'order', side: this.side, orderType, qty, tif: this.tif.value as TimeInForce };
    const price = (input: HTMLInputElement, name: string): number | string => {
      const v = Number(input.value);
      return input.value !== '' && Number.isFinite(v) && v > 0 ? v : `${name} must be a price above zero`;
    };
    if (orderType === 'limit' || orderType === 'stopLimit') {
      const v = price(this.limit, 'Limit price'); if (typeof v === 'string') return v; order.limitPrice = v;
    }
    if (orderType === 'stop' || orderType === 'stopLimit') {
      const v = price(this.stop, 'Stop price'); if (typeof v === 'string') return v; order.stopPrice = v;
    }
    if (orderType === 'trailingStop') {
      const v = price(this.trail, 'Trail'); if (typeof v === 'string') return v;
      if (this.trailUnit.value === 'percent') order.trailPercent = v; else order.trailAmount = v;
    }
    if (this.extended.checked) order.extendedHours = true;
    if (this.postOnly.checked) order.postOnly = true;
    if (this.hidden.checked) order.hidden = true;
    if (this.peg.checked) order.midpointPeg = true;
    const display = Number(this.display.value);
    if (this.display.value !== '' && display > 0) order.displayQty = Math.floor(display);
    if (this.attach.value === 'bracket') {
      const tp = price(this.takeProfit, 'Take-profit'); if (typeof tp === 'string') return tp;
      const sl = price(this.stopLoss, 'Stop-loss'); if (typeof sl === 'string') return sl;
      order.bracket = { takeProfit: tp, stopLoss: sl };
    }
    if (this.attach.value === 'oco') {
      const sp = price(this.ocoStop, 'OCO stop'); if (typeof sp === 'string') return sp;
      order.oco = { stopPrice: sp };
      if (this.ocoLimit.value !== '') {
        const lp = price(this.ocoLimit, 'OCO stop limit'); if (typeof lp === 'string') return lp;
        order.oco.stopLimitPrice = lp;
      }
    }
    return order;
  }

  private place(): void {
    const built = this.build();
    if (typeof built === 'string') { this.setStatus(built, 'tr-refused'); return; }
    this.counter += 1;
    const id = `ticket-${Date.now().toString(36)}-${this.counter}`;
    built.id = id;
    this.pending.set(id, `${built.side === 'sellShort' ? 'Short' : built.side === 'buy' ? 'Buy' : 'Sell'} ${fmt.int(built.qty)}`);
    this.setStatus('Sent...', 'tr-pending');
    this.send(built);
  }

  private setStatus(text: string, cls: string): void {
    this.status.textContent = text;
    this.status.className = `tr-status ${cls}`;
  }

  /** The broker's answer to an order this ticket sent */
  handleResult(r: UserResult): void {
    if (!r.echo || !this.pending.has(r.echo)) return;
    const what = this.pending.get(r.echo) ?? '';
    this.pending.delete(r.echo);
    if (r.accepted) this.setStatus(`${what}: accepted${r.orderId ? ` (${r.orderId})` : ''}`, 'tr-accepted');
    else this.setStatus(`${what}: refused, ${r.reason ?? 'no reason given'}`, 'tr-refused');
  }

  /** A protocol error for one of this ticket's orders: malformed before the broker saw it */
  handleError(echo: string | undefined, message: string): boolean {
    if (!echo || !this.pending.has(echo)) return false;
    this.pending.delete(echo);
    this.setStatus(`Not sent: ${message}`, 'tr-refused');
    return true;
  }

  update(f: Frame): void {
    if (f.price > 0 && f.price !== this.lastPrice) {
      this.lastPrice = f.price;
      this.refresh();
    }
  }
}
