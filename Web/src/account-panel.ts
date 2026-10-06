import './trading.css';

import { el, setText } from './dom';
import { fmt } from './format';
import type { Frame } from './protocol';

/* The user's account: balances, the position and its P&L, and margin standing
 * (OrderModelPlan Step 4.2). Money is shown in this market's dollars; when the account is
 * scaled, the real-world equivalent rides along so the size still reads as what was entered.
 */

const FIELDS: [string, string][] = [
  ['equity', 'Equity'], ['cash', 'Cash'], ['buyingPower', 'Buying power'], ['debit', 'Margin loan'],
  ['position', 'Position'], ['average', 'Average cost'], ['unrealized', 'Unrealized P&L'],
  ['realized', 'Realized P&L'], ['fees', 'Fees paid'], ['shorts', 'Short / borrowed'],
];

const signed = (v: number) => `${v >= 0 ? '+' : '-'}${fmt.money(Math.abs(v))}`;

export class AccountPanel {
  readonly root = el('div', 'panel tr-panel account-panel');
  private title = el('span', 'panel-title', 'Account');
  private badge = el('span', 'tr-badge');
  private values = new Map<string, HTMLElement>();
  private realWorld = el('div', 'tr-realworld');

  constructor() {
    const head = el('div', 'panel-head');
    head.append(this.title, this.badge);
    const grid = el('div', 'tr-grid');
    for (const [key, label] of FIELDS) {
      const cell = el('div', 'tr-cell');
      const value = el('span', 'tr-value', '--');
      cell.append(el('span', 'tr-label', label), value);
      this.values.set(key, value);
      grid.append(cell);
    }
    this.root.append(head, grid, this.realWorld);
  }

  private set(key: string, text: string, tone?: 'up' | 'down'): void {
    const node = this.values.get(key);
    if (!node) return;
    setText(node, text);
    const cls = `tr-value${tone ? ` ${tone}` : ''}`;
    if (node.className !== cls) node.className = cls;
  }

  update(f: Frame): void {
    const u = f.user;
    if (!u) return;
    setText(this.title, `Account - ${u.broker}`);
    setText(this.badge, u.violation ? 'BELOW MAINTENANCE' : u.margin ? 'MARGIN' : 'CASH');
    this.badge.className = `tr-badge${u.violation ? ' tr-badge-alert' : u.margin ? ' tr-badge-margin' : ''}`;
    const tone = (v: number) => (v > 0.005 ? 'up' : v < -0.005 ? 'down' : undefined);
    this.set('equity', fmt.money(u.equity));
    this.set('cash', fmt.money(u.cash + u.escrow));
    this.set('buyingPower', fmt.money(u.buyingPower));
    this.set('debit', u.debit > 0 ? fmt.money(u.debit) : '--');
    this.set('position', u.position === 0 ? 'flat' : `${u.position > 0 ? 'long' : 'short'} ${fmt.int(Math.abs(u.position))}`,
      u.position > 0 ? 'up' : u.position < 0 ? 'down' : undefined);
    this.set('average', u.position !== 0 ? `$${fmt.price(u.averageCost)}` : '--');
    this.set('unrealized', u.position !== 0 ? signed(u.unrealizedPnl) : '--', tone(u.unrealizedPnl));
    this.set('realized', signed(u.realizedPnl), tone(u.realizedPnl));
    this.set('fees', fmt.money(u.feesPaid));
    this.set('shorts', u.short > 0 ? `${fmt.int(u.short)} / ${fmt.int(u.borrowed)}` : '--');
    setText(this.realWorld, u.scaled && u.moneyScale > 0
      ? `Scaled account: $1 here is $${fmt.int(Math.round(1 / u.moneyScale))} in the real world, so equity is about ${fmt.money(u.equity / u.moneyScale)}`
      : 'Dollars as entered');
  }
}
