import { isEffective } from './features';
import { fmt } from './format';
import type { AgentRow, Features } from './protocol';

/* The agent roster's columns, as data.
 *
 * Each column says how to read a row and when it is worth showing: the account columns appear
 * only when the switches that give them meaning are on, so a run with every switch off has the
 * roster it always had. Adding, dropping or reordering a column is an edit to this list.
 */

export interface RosterColumn {
  key: string;
  label: string;
  /** Its grid track */
  width: string;
  /** The cell's class, which sets alignment: c-id, c-type, c-num, c-status */
  cls: string;
  /** Header tooltip */
  title?: string;
  /** Shown only when this holds for the run's switches */
  when?: (features: Features) => boolean;
  text: (r: AgentRow) => string;
  /** An extra class for one cell, by its value */
  tone?: (r: AgentRow) => string | undefined;
}

const statusCode = (r: AgentRow) => (r.stranded ? 'STK' : r.status.slice(0, 3));

export const ROSTER_COLUMNS: readonly RosterColumn[] = [
  {
    key: 'id', label: 'ID', width: '1.6fr', cls: 'c-id',
    text: (r) => (r.transient ? '~' : '') + r.id.replace(/^A-0+/, 'A-'),
  },
  { key: 'type', label: 'TYPE', width: '1fr', cls: 'c-type', text: (r) => `${r.type[0]}/${r.subType.slice(0, 4)}` },
  { key: 'cash', label: 'CASH', width: '1fr', cls: 'c-num', text: (r) => fmt.money(r.cash) },
  { key: 'holdings', label: 'HELD', width: '0.8fr', cls: 'c-num', title: 'shares held', text: (r) => fmt.int(r.holdings) },
  {
    key: 'short', label: 'SHORT', width: '0.8fr', cls: 'c-num', title: 'shares sold short and still owed',
    when: (f) => isEffective(f, 'shorting'),
    text: (r) => (r.short > 0 ? fmt.int(r.short) : ''),
    tone: (r) => (r.short > 0 ? 'down' : undefined),
  },
  {
    key: 'equity', label: 'EQUITY', width: '1fr', cls: 'c-num', title: 'cash and shares at the last price, less shares owed',
    when: (f) => f.margin,
    text: (r) => fmt.money(r.equity),
  },
  {
    key: 'buyingPower', label: 'BP', width: '1fr', cls: 'c-num', title: 'buying power',
    when: (f) => f.margin,
    text: (r) => fmt.money(r.buyingPower),
  },
  { key: 'bids', label: 'BID', width: '0.6fr', cls: 'c-num', title: 'resting bids', text: (r) => String(r.bids) },
  { key: 'asks', label: 'ASK', width: '0.6fr', cls: 'c-num', title: 'resting asks', text: (r) => String(r.asks) },
  {
    key: 'held', label: 'STOPS', width: '0.6fr', cls: 'c-num', title: 'orders the broker holds until they trigger',
    when: (f) => f.agentBrackets,
    text: (r) => (r.held > 0 ? String(r.held) : ''),
  },
  {
    key: 'sentiment', label: 'SENT', width: '0.7fr', cls: 'c-num',
    text: (r) => r.sentiment.toFixed(2),
    tone: (r) => (r.sentiment > 0.05 ? 'up' : r.sentiment < -0.05 ? 'down' : undefined),
  },
  {
    key: 'account', label: 'ACCT', width: '0.6fr', cls: 'c-status', title: 'M: margin privileges, !: below maintenance',
    when: (f) => f.margin,
    text: (r) => (r.violation ? '!' : r.margin ? 'M' : ''),
    tone: (r) => (r.violation ? 'st-bnk' : r.margin ? 'st-mgn' : undefined),
  },
  {
    key: 'status', label: 'ST', width: '0.6fr', cls: 'c-status',
    text: statusCode,
    tone: (r) => `st-${statusCode(r).toLowerCase()}`,
  },
];

export function rosterColumns(features: Features): RosterColumn[] {
  return ROSTER_COLUMNS.filter((c) => !c.when || c.when(features));
}
