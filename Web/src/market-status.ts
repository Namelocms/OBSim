import './market-status.css';

import { DEFAULT_FEATURES, isEffective } from './features';
import { fmt } from './format';
import type { Features, Frame } from './protocol';

/* Market structure and broker activity, as chips: a label and a value each.
 *
 * Every chip is a pure function from a frame (and the run's switches) to what it shows, or to
 * nothing when it has nothing to say -- no LULD chip while the bands are off, no pause chip
 * while trading. The lists below are the content; StatusStrip is one way to draw them. A
 * redesign can split the lists, reorder them, or draw the same chips as a sidebar, a banner or
 * a tooltip without touching the logic that decides what they say.
 */

export type ChipTone = 'up' | 'down' | 'warn' | 'alert';

export interface ChipView {
  text: string;
  /** Overrides the chip's label, for a chip whose subject changes (which cross is next) */
  label?: string;
  tone?: ChipTone;
  /** The long form, as a tooltip */
  title?: string;
}

export interface StatusChip {
  key: string;
  label: string;
  view: (f: Frame, features: Features) => ChipView | null;
}

/* The engine's knee for the borrow fee curve (Core/include/StockLoan.h, BORROW_KNEE). Above
 * it the fee climbs steeply: hard to borrow. Only used to colour the chip. */
const HARD_TO_BORROW = 0.6;

const tone = (v: number): ChipTone | undefined => (v > 0.00005 ? 'up' : v < -0.00005 ? 'down' : undefined);

/** The market's own state: reference prices, the next cross, bands, pauses, restrictions */
export const MARKET_CHIPS: readonly StatusChip[] = [
  {
    key: 'close', label: 'CLOSE',
    view: (f) => {
      const close = f.market.official.close;
      if (!(close > 0)) return null;
      const change = f.price / close - 1;
      return {
        text: `${fmt.price(close)} ${fmt.change(change)}`,
        tone: tone(change),
        title: 'The last official close, and the change since',
      };
    },
  },
  {
    key: 'open', label: 'OPEN',
    view: (f, features) => {
      // Yesterday's open until the next opening cross replaces it, so only shown after one
      const open = f.market.official.open;
      if (!features.auctions || !(open > 0)) return null;
      if (f.session !== 'REGULAR' && f.session !== 'AFTERHOURS') return null;
      return { text: fmt.price(open), title: "Today's official open, from the opening cross" };
    },
  },
  {
    key: 'cross', label: 'CROSS',
    view: (f) => {
      const a = f.market.auction;
      if (!a.collecting) return null;
      const label = a.cross === 'OPEN' ? 'OPEN CROSS' : 'CLOSE CROSS';
      if (!(a.matched > 0)) {
        return { label, text: `no match, ${fmt.int(a.orders)} orders`, title: 'Indicative: nothing would trade if the cross ran now' };
      }
      const imbalance = a.imbalance > 0 ? ` imb ${fmt.int(a.imbalance)} ${a.side === 'B' ? 'buy' : 'sell'}` : '';
      return {
        label,
        text: `${fmt.price(a.price)} x ${fmt.int(a.matched)}${imbalance}`,
        title: `Indicative price and matched shares if the cross ran now. ${fmt.int(a.orders)} `
          + `on-${a.cross === 'OPEN' ? 'open' : 'close'} orders waiting, plus the book.`,
      };
    },
  },
  {
    key: 'luld', label: 'LULD',
    view: (f) => {
      const l = f.market.luld;
      if (!l.active) return null;
      return {
        text: `${fmt.price(l.lower)} - ${fmt.price(l.upper)}${l.limitState ? ' LIMIT' : ''}`,
        tone: l.limitState ? 'warn' : undefined,
        title: `Price bands around a reference of ${fmt.price(l.reference)}. A limit state that `
          + 'lasts 15 seconds pauses trading for five minutes.',
      };
    },
  },
  {
    key: 'pause', label: 'PAUSED',
    view: (f) => {
      const p = f.market.pause;
      if (!p.paused) return null;
      return {
        text: `reopens in ${fmt.span(p.endsMs - f.simTimeMs)}`,
        tone: 'alert',
        title: 'LULD trading pause. Trading resumes with a reopening cross.',
      };
    },
  },
  {
    key: 'ssr', label: 'SSR',
    view: (f) => {
      const s = f.market.ssr;
      if (!s.active) return null;
      return {
        text: `until ${fmt.clock(s.untilMs)}`,
        tone: 'warn',
        title: 'Short sale restriction (Reg SHO Rule 201): short sales must be priced above the '
          + `best bid. Triggered by a 10% fall below the previous close of ${fmt.price(s.referenceClose)}.`,
      };
    },
  },
  {
    key: 'short', label: 'SHORT INT',
    view: (f, features) => {
      if (!isEffective(features, 'shorting')) return null;
      const l = f.lending;
      const ofFloat = f.shareFloat > 0 ? l.shortInterest / f.shareFloat : 0;
      return {
        text: `${fmt.pct(ofFloat)} of float`,
        title: `${fmt.int(l.shortInterest)} shares sold short and still owed, ${fmt.int(l.borrowed)} of them on loan`,
      };
    },
  },
  {
    key: 'borrow', label: 'BORROW',
    view: (f, features) => {
      if (!isEffective(features, 'shorting')) return null;
      const l = f.lending;
      return {
        text: `${fmt.pct(l.utilisation)} used, ${(l.feeRate * 100).toFixed(2)}%/yr`,
        tone: l.utilisation > HARD_TO_BORROW ? 'warn' : undefined,
        title: `${fmt.int(l.borrowed)} on loan of ${fmt.int(Math.round(l.supply))} lendable. The fee `
          + 'is an annual rate set by how much of the pool is out.',
      };
    },
  },
];

/** What the brokers have taken and done: fees, interest, margin calls, stops, recalls */
export const BROKER_CHIPS: readonly StatusChip[] = [
  {
    key: 'fees', label: 'FEES',
    view: (f, features) => {
      if (!features.fees) return null;
      const h = f.house;
      return {
        text: fmt.money(h.commissions + h.exchangeFees + h.regulatoryFees),
        title: `Commissions ${fmt.money(h.commissions)}, exchange fees net of rebates `
          + `${fmt.money(h.exchangeFees)}, SEC and FINRA fees ${fmt.money(h.regulatoryFees)}`,
      };
    },
  },
  {
    key: 'interest', label: 'INTEREST',
    view: (f, features) => {
      if (!features.margin) return null;
      const h = f.house;
      return {
        text: fmt.money(h.marginInterest + h.borrowFees),
        title: `Margin interest ${fmt.money(h.marginInterest)}, borrow fees ${fmt.money(h.borrowFees)}`,
      };
    },
  },
  {
    key: 'calls', label: 'MARGIN CALLS',
    view: (f, features) => {
      if (!features.margin) return null;
      const b = f.broker;
      return {
        text: fmt.int(b.marginCalls) + (b.writeOffs > 0 ? `, ${fmt.int(b.writeOffs)} written off` : ''),
        tone: b.writeOffs > 0 ? 'down' : undefined,
        title: `Accounts liquidated below maintenance. Losses the brokers absorbed: ${fmt.money(f.house.brokerLosses)}`,
      };
    },
  },
  {
    key: 'stops', label: 'STOPS',
    view: (f, features) => {
      const b = f.broker;
      if (!features.agentBrackets && b.stopsTriggered === 0) return null;
      const cascades = b.cascades > 0 ? `, ${fmt.int(b.cascades)} cascades (max ${fmt.int(b.deepestCascade)})` : '';
      return {
        text: `${fmt.int(b.stopsTriggered)} fired${cascades}`,
        title: 'Stops triggered by the last trade. A cascade is a stop whose fill triggered more.',
      };
    },
  },
  {
    key: 'brackets', label: 'BRACKETS',
    view: (f, features) => (features.agentBrackets
      ? { text: fmt.int(f.broker.brackets), title: 'Entries placed with a protective stop-loss and take-profit' }
      : null),
  },
  {
    key: 'recalls', label: 'RECALLS',
    view: (f, features) => {
      if (!isEffective(features, 'shorting')) return null;
      const b = f.broker;
      return {
        text: `${fmt.int(b.recalledShares)} sh` + (b.buyIns > 0 ? `, ${fmt.int(b.buyIns)} buy-ins` : ''),
        title: 'Shares lenders called back, and the forced buys that covered recalls and failed deliveries',
      };
    },
  },
  {
    key: 'pauses', label: 'PAUSES',
    view: (f, features) => (features.luld
      ? { text: fmt.int(f.broker.tradingPauses), title: 'LULD trading pauses this run' }
      : null),
  },
  {
    key: 'ssrDays', label: 'SSR TRIGGERS',
    view: (f, features) => (isEffective(features, 'shorting')
      ? { text: fmt.int(f.broker.ssrTriggers), title: 'Times the short sale restriction was triggered this run' }
      : null),
  },
];

export interface StripOptions {
  /** Drawn as a titled panel with the chips in a grid, rather than a bare row */
  title?: string;
}

/* One way to draw chips: a row, or with a title, a small panel. Hides itself when no chip
 * has anything to show, so a run with every switch off looks as it always did. */
export class StatusStrip {
  readonly root = document.createElement('div');
  private body: HTMLElement;
  private features: Features = { ...DEFAULT_FEATURES };
  private nodes: { chip: StatusChip; root: HTMLElement; label: HTMLElement; value: HTMLElement; last: string }[] = [];

  constructor(chips: readonly StatusChip[], options: StripOptions = {}) {
    if (options.title !== undefined) {
      this.root.className = 'status-panel hidden';
      const head = document.createElement('div');
      head.className = 'ms-head';
      head.textContent = options.title;
      this.body = document.createElement('div');
      this.body.className = 'ms-grid';
      this.root.append(head, this.body);
    } else {
      this.root.className = 'status-strip hidden';
      this.body = this.root;
    }

    for (const chip of chips) {
      const root = document.createElement('div');
      root.className = 'ms-chip';
      root.hidden = true;
      const label = document.createElement('span');
      label.className = 'ms-label';
      label.textContent = chip.label;
      const value = document.createElement('span');
      value.className = 'ms-value';
      root.append(label, value);
      this.body.append(root);
      this.nodes.push({ chip, root, label, value, last: '' });
    }
  }

  /** The run's switches, from hello: what a chip may assume is meaningful */
  setFeatures(features: Features): void {
    this.features = features;
  }

  update(f: Frame): void {
    let shown = 0;
    for (const n of this.nodes) {
      const view = n.chip.view(f, this.features);
      if (!view) {
        if (!n.root.hidden) n.root.hidden = true;
        continue;
      }
      shown += 1;
      if (n.root.hidden) n.root.hidden = false;
      // One string compare instead of four DOM reads: most frames change nothing here
      const key = `${view.label ?? ''}|${view.text}|${view.tone ?? ''}|${view.title ?? ''}`;
      if (key === n.last) continue;
      n.last = key;
      n.label.textContent = view.label ?? n.chip.label;
      n.value.textContent = view.text;
      n.root.className = `ms-chip${view.tone ? ` ms-${view.tone}` : ''}`;
      // The value leads the tooltip: a narrow cell can cut it short
      n.root.title = view.title ? `${view.text}\n${view.title}` : view.text;
    }
    this.root.classList.toggle('hidden', shown === 0);
  }

  clear(): void {
    for (const n of this.nodes) {
      n.root.hidden = true;
      n.last = '';
    }
    this.root.classList.add('hidden');
  }
}
