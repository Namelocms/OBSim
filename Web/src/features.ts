import type { Features } from './protocol';

/* What each order model switch is, as data.
 *
 * The reset dialog builds its switches from this list, and anything else that needs to name
 * a switch -- a status strip, a column, a future broker preset dropdown -- reads it here rather
 * than carrying its own copy of the wording. No DOM in this file on purpose: a reworked UI
 * keeps the list and throws away the widgets.
 *
 * Mirrors Core/include/Features.h. Every switch defaults off, and a run with all of them off
 * is the market as it was before the order model.
 */

/** The switches that are on/off, as opposed to luldTier, which picks a value */
export type FeatureFlag = Exclude<keyof Features, 'luldTier'>;

export type FeatureGroup = 'Agents' | 'Broker' | 'Market';

export interface FeatureInfo {
  key: FeatureFlag;
  group: FeatureGroup;
  label: string;
  /** One line on what changes when it is on */
  hint: string;
  /** A switch this one does nothing without */
  requires?: FeatureFlag;
}

export const FEATURE_GROUPS: readonly FeatureGroup[] = ['Agents', 'Broker', 'Market'];

export const FEATURE_INFO: readonly FeatureInfo[] = [
  {
    key: 'agentReplace', group: 'Agents', label: 'Cancel/replace',
    hint: 'Agents amend working orders instead of cancelling and re-posting',
  },
  {
    key: 'adversityFromEntry', group: 'Agents', label: 'Adversity from entry',
    hint: 'Pain is measured from where a position opened, long or short',
  },
  {
    key: 'agentBrackets', group: 'Agents', label: 'Protective brackets',
    hint: 'Weak-conviction entries get a stop-loss and a take-profit',
  },
  {
    key: 'agentPostOnly', group: 'Agents', label: 'Post-only quotes',
    hint: 'Market makers never take, repricing a tick passive instead',
  },
  {
    key: 'fees', group: 'Broker', label: 'Fees',
    hint: 'Commissions, maker-taker exchange fees, SEC and FINRA fees',
  },
  {
    key: 'margin', group: 'Broker', label: 'Margin',
    hint: 'Reg T borrowing from $2,000 of equity, maintenance calls, interest',
  },
  {
    key: 'shorting', group: 'Broker', label: 'Short selling',
    hint: 'Locates, a lending pool with a utilisation fee, recalls, Rule 201',
    requires: 'margin',
  },
  {
    key: 'stopsExtendedHours', group: 'Broker', label: 'Stops off hours',
    hint: 'Stops may trigger outside the regular session',
  },
  {
    key: 'auctions', group: 'Market', label: 'Opening and closing crosses',
    hint: 'One-price auctions at 09:30 and 16:00, with on-open and on-close orders',
  },
  {
    key: 'regularOnlyAgentOrders', group: 'Market', label: 'Regular-only orders',
    hint: 'Orders placed in the regular session cannot trade outside it',
  },
  {
    key: 'luld', group: 'Market', label: 'Limit up-limit down',
    hint: 'Price bands, limit states and five-minute trading pauses',
  },
];

export const LULD_TIERS: readonly { tier: Features['luldTier']; label: string }[] = [
  { tier: 1, label: 'Tier 1: S&P 500, Russell 1000 (5% above $3)' },
  { tier: 2, label: 'Tier 2: every other stock (10% above $3)' },
];

export const DEFAULT_FEATURES: Readonly<Features> = {
  agentReplace: false,
  adversityFromEntry: false,
  agentBrackets: false,
  agentPostOnly: false,
  fees: false,
  margin: false,
  shorting: false,
  stopsExtendedHours: false,
  auctions: false,
  regularOnlyAgentOrders: false,
  luld: false,
  luldTier: 2,
};

/** A complete set from a partial one, so an older server's hello still reads */
export function withDefaults(features?: Partial<Features>): Features {
  return { ...DEFAULT_FEATURES, ...(features ?? {}) };
}

/** On, and not stranded by a switch it needs */
export function isEffective(features: Features, key: FeatureFlag): boolean {
  if (!features[key]) return false;
  const requires = FEATURE_INFO.find((f) => f.key === key)?.requires;
  return requires ? isEffective(features, requires) : true;
}

export function countOn(features: Features): number {
  return FEATURE_INFO.filter((f) => features[f.key]).length;
}
