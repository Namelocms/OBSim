import './account-settings.css';

import { el } from './dom';
import { fmt } from './format';
import type { BrokerPreset, UserAccountParams } from './protocol';

/* The user's account, as a section of the reset dialog (OrderModelPlan Step 4.2).
 *
 * Whether there is one, how big it is, whether its money is scaled to the market or taken as
 * entered, and which broker prices it. The presets come from the server's hello, so a broker
 * added to the data file appears here with no client change.
 */

const DEFAULT_USER: UserAccountParams = { enabled: true, cash: 25000, scaled: true, preset: 'generic-retail' };

/** A one-line summary of a preset's pricing, for the dropdown's caption */
export function presetSummary(p: BrokerPreset): string {
  const c = p.commission;
  const commission = c.perShare > 0
    ? `$${c.perShare} a share${c.min > 0 ? `, $${c.min.toFixed(2)} min` : ''}`
    : c.perOrder > 0 ? `$${c.perOrder.toFixed(2)} an order` : '$0 commission';
  const m = p.margin;
  const first = m.tiers && m.tiers.length > 0 ? m.tiers[0]!.apr : m.apr ?? 0;
  const margin = m.tiers && m.tiers.length > 1
    ? `margin ${(first * 100).toFixed(2)}% under $${fmt.int(m.tiers[1]!.from)}`
    : `margin ${(first * 100).toFixed(2)}%`;
  const extras = [
    p.exchangeFees.passThrough ? 'exchange fees passed through' : '',
    m.interestFree ? `first $${fmt.int(m.interestFree)} interest-free` : '',
    p.regulatory.catPerShare > 0 ? 'CAT fee' : '',
  ].filter((x) => x);
  return [commission, margin, ...extras].join(', ');
}

export class AccountSettings {
  readonly root = el('section', 'account-settings');
  private enabled = el('input');
  private cash = el('input', 'as-input');
  private scaled = el('input');
  private absolute = el('input');
  private preset = el('select', 'as-input');
  private caption = el('div', 'as-caption');
  private presets: BrokerPreset[] = [];

  constructor() {
    const head = el('div', 'as-head');
    head.append(el('span', 'as-title', 'Your account'));
    const enabledRow = el('label', 'as-toggle');
    this.enabled.type = 'checkbox';
    this.enabled.setAttribute('aria-label', 'Trade in this run');
    enabledRow.append(this.enabled, el('span', '', 'trade in this run'));
    head.append(enabledRow);
    this.root.append(head);

    const cashRow = el('label', 'as-row');
    cashRow.append(el('span', 'as-label', 'Starting cash'));
    this.cash.type = 'number';
    this.cash.min = '0';
    this.cash.step = '1000';
    cashRow.append(this.cash);
    this.root.append(cashRow);

    // Both money modes, each with what it means for the account's size in this market
    const mode = el('div', 'as-modes');
    for (const [input, value, label, explain] of [
      [this.scaled, 'scaled', 'Scaled to this market',
        'Converted the way every trader\'s money here is, so you are as big as a real retail account '
        + 'of that size would be in a stock this small. Balances show in this market\'s dollars.'],
      [this.absolute, 'absolute', 'Dollars as entered',
        'Exactly the cash you type. The default market is small, so this can make you its largest trader.'],
    ] as const) {
      const row = el('label', 'as-mode');
      input.type = 'radio';
      input.name = 'as-money';
      input.value = value;
      const text = el('span', 'as-mode-text');
      text.append(el('span', 'as-mode-label', label), el('span', 'as-mode-explain', explain));
      row.append(input, text);
      mode.append(row);
    }
    this.root.append(mode);

    const brokerRow = el('label', 'as-row');
    brokerRow.append(el('span', 'as-label', 'Broker'));
    brokerRow.append(this.preset);
    this.root.append(brokerRow, this.caption);
    this.preset.addEventListener('change', () => this.refresh());
    this.enabled.addEventListener('change', () => this.refresh());

    this.fill(DEFAULT_USER, []);
  }

  /** The running server's account and its presets, from hello */
  fill(user: Partial<UserAccountParams> | undefined, presets: BrokerPreset[]): void {
    const u = { ...DEFAULT_USER, ...(user ?? {}) };
    if (presets.length > 0) {
      this.presets = presets;
      this.preset.replaceChildren(...presets.map((p) => {
        const o = el('option', '', p.verified ? p.name : `${p.name} (unverified)`);
        o.value = p.id;
        return o;
      }));
    }
    this.enabled.checked = u.enabled;
    this.cash.value = String(u.cash);
    this.scaled.checked = u.scaled;
    this.absolute.checked = !u.scaled;
    this.preset.value = u.preset;
    this.refresh();
  }

  read(): UserAccountParams {
    const cash = Number(this.cash.value);
    return {
      enabled: this.enabled.checked,
      cash: Number.isFinite(cash) && cash >= 0 ? cash : DEFAULT_USER.cash,
      scaled: this.scaled.checked,
      preset: this.preset.value || DEFAULT_USER.preset,
    };
  }

  private refresh(): void {
    this.root.classList.toggle('as-off', !this.enabled.checked);
    const p = this.presets.find((x) => x.id === this.preset.value);
    if (!p) { this.caption.textContent = ''; return; }
    this.caption.replaceChildren(
      el('div', 'as-summary', presetSummary(p)),
      el('div', p.verified ? 'as-verified' : 'as-unverified',
        `${p.verified ? 'Checked against the broker' : 'Not fully checked'} as of ${p.asOf}`),
      el('div', 'as-notes', p.notes),
    );
  }
}
