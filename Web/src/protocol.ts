/* The wire protocol, as TypeScript types.
 *
 * Mirrors Server/include/Protocol.h. Repeated data arrives as TUPLES, not objects --
 * [price, volume, orders] for a book level, [epochSec, price, volume, side, kind] for a
 * trade print. `hello` carries the layouts so this file can assert it still agrees with the
 * server rather than reading the wrong column if one ever changes.
 *
 * Two times are sent for every frame. `simTimeMs` is authoritative; `epochSec` is derived
 * and lossy below a microsecond, and exists so the chart can put a date on its axis.
 */

export const PROTOCOL_VERSION = 1;

export type Side = 'B' | 'S';
/** T continuous trading, O opening cross, C closing cross, R reopening after a pause */
export type PrintKind = 'T' | 'O' | 'C' | 'R';

/** [price, volume, orders] */
export type BookLevelTuple = [number, number, number];
/** [epochSec, price, volume, side, kind] -- a cross's side is the side of its imbalance */
export type PrintTuple = [number, number, number, Side, PrintKind];
/** [simTimeMs, kind, text] */
export type LogTuple = [number, string, string];

export interface Hello {
  v: number;
  type: 'hello';
  protocol: number;
  epochBase: number;
  calendar: {
    premarketMinutes: number;
    regularMinutes: number;
    afterhoursMinutes: number;
    overnightMinutes: number;
    activeMinutesPerDay: number;
    totalMinutesPerDay: number;
  };
  params: SimParams;
  /** The brokers a user account can be priced from, as the server's data file has them */
  presets: BrokerPreset[];
  config: {
    framesPerSecond: number;
    agentRowsPerSecond: number;
    agentRowCap: number;
    bookDepthLevels: number;
    logLinesPerFrame: number;
  };
  tuples: { bookLevel: string[]; print: string[]; log: string[] };
}

export interface SimParams {
  seed: number;
  backDataDays: number;
  liveStartSession: SessionName;
  minLiquidity: number;
  agentCount: number;
  shareFloat: number;
  startPrice: number;
  transientFraction: number;
  /** The order model's switches, all off by default. See features.ts for what each does. */
  features: Features;
  /** The user's own account (OrderModelPlan Step 4.2) */
  user: UserAccountParams;
}

export interface UserAccountParams {
  enabled: boolean;
  /** Real-world dollars when scaled, dollars as entered when not */
  cash: number;
  /** Scaled to the market like every agent's money, or dollars as entered */
  scaled: boolean;
  /** A preset id from hello.presets */
  preset: string;
}

/** One broker's pricing, as Server/presets/brokers.json has it */
export interface BrokerPreset {
  id: string;
  name: string;
  asOf: string;
  /** Every figure read from the broker's own published page on asOf */
  verified: boolean;
  sources: string[];
  notes: string;
  commission: { perShare: number; perOrder: number; min: number; maxPct: number };
  exchangeFees: { passThrough: boolean; takerPerShare?: number; makerRebatePerShare?: number };
  regulatory: { passThrough: boolean; catPerShare: number };
  margin: {
    apr?: number;
    tiers?: { from: number; apr: number }[];
    blended?: boolean;
    interestFree?: number;
    houseMaintenance?: number;
  };
}

/** What a reset may set. Anything left out keeps the server's current value. */
export type ResetParams = Partial<Omit<SimParams, 'features' | 'user'>> & {
  features?: Partial<Features>;
  user?: Partial<UserAccountParams>;
};

/** Mirrors Core/include/Features.h, under the names the wire uses */
export interface Features {
  agentReplace: boolean;
  adversityFromEntry: boolean;
  agentBrackets: boolean;
  agentPostOnly: boolean;
  fees: boolean;
  margin: boolean;
  shorting: boolean;
  stopsExtendedHours: boolean;
  auctions: boolean;
  regularOnlyAgentOrders: boolean;
  luld: boolean;
  luldTier: 1 | 2;
}

export type SessionName = 'PREMARKET' | 'REGULAR' | 'AFTERHOURS' | 'OVERNIGHT' | 'CLOSED';

export interface AgentRow {
  id: string;
  cash: number;
  holdings: number;
  bids: number;
  asks: number;
  sentiment: number;
  status: 'ACTIVE' | 'INACTIVE' | 'BANKRUPT' | 'LEAVING' | 'POOLED';
  type: 'RETAIL' | 'INSTITUTION';
  subType: 'NOISE' | 'MOMENTUM' | 'ALGO' | 'INFORMED';
  transient: boolean;
  stranded: boolean;
  /** Worth at the last price: cash, escrow and long shares, less shares owed */
  equity: number;
  /** Shares sold short and still owed, and how many of those are on loan */
  short: number;
  borrowed: number;
  buyingPower: number;
  /** Has the equity to borrow under the margin rules */
  margin: boolean;
  /** Below its maintenance requirement right now */
  violation: boolean;
  /** Stops and other orders the broker is holding for it */
  held: number;
}

/** The market's structure rather than its price */
export interface MarketState {
  luld: { active: boolean; lower: number; upper: number; reference: number; limitState: boolean };
  pause: { paused: boolean; endsMs: number };
  ssr: { active: boolean; untilMs: number; referenceClose: number };
  official: { open: number; close: number; previousClose: number };
  /** The next cross, while it collects: through the premarket, and after the 15:50 cutoff */
  auction: {
    collecting: boolean; cross: 'OPEN' | 'CLOSE'; price: number; matched: number;
    imbalance: number; side: Side; orders: number;
  };
}

export interface LendingState {
  supply: number; borrowed: number; utilisation: number; feeRate: number; shortInterest: number;
}

/** The ledger's house accounts */
export interface HouseState {
  commissions: number; exchangeFees: number; regulatoryFees: number;
  marginInterest: number; borrowFees: number; brokerLosses: number;
}

/** What the broker has done over the run */
export interface BrokerCounts {
  marginCalls: number; writeOffs: number; stopsTriggered: number; cascades: number;
  deepestCascade: number; recalledShares: number; buyIns: number; brackets: number;
  tradingPauses: number; ssrTriggers: number;
}

export interface BackDataProgress {
  percent: number;
  session: SessionName;
  dayIndex: number;
  activeMinutes: number;
  totalMinutes: number;
  targetMinutes: number;
  events: number;
  ticks: number;
  extraDays: number;
  transientArrivals: number;
  transientFraction: number;
  liveTransients: number;
}

export interface Frame {
  v: number;
  type: 'frame';
  seq: number;
  simTimeMs: number;
  epochSec: number;
  price: number;
  spread: number;
  session: SessionName;
  sentiment: number;
  shareFloat: number;
  tickCount: number;
  restingBids: number;
  restingAsks: number;
  pop: { residents: number; transients: number; total: number; fraction: number };
  clock: {
    speed: number;
    paused: boolean;
    running: boolean;
    backDataRunning: boolean;
    backDataAborted: boolean;
  };
  book: { bids: BookLevelTuple[]; asks: BookLevelTuple[] };
  prints: PrintTuple[];
  printsDropped: number;
  market: MarketState;
  lending: LendingState;
  house: HouseState;
  broker: BrokerCounts;
  logs: LogTuple[];
  logsDropped: number;
  /* ABSENT means "roster unchanged, keep what you have". An empty rows array would mean
   * the market actually emptied -- the distinction is load bearing, so this is optional
   * rather than defaulted. */
  agents?: { rows: AgentRow[]; omitted: number };
  /* Present only while back data is building */
  backData?: BackDataProgress;
  /** The user's account, on a run that has one */
  user?: UserAccount;
}

export type UserOrderType = 'market' | 'limit' | 'stop' | 'stopLimit' | 'trailingStop';
export type TimeInForce = 'DAY' | 'GTC' | 'GTD' | 'IOC' | 'FOK' | 'OPG' | 'CLS';

export interface UserOrder {
  id: string;
  side: Side;
  type: UserOrderType;
  price: number;
  stopPrice: number;
  trailAmount: number;
  /** A percentage: 5 is 5% */
  trailPercent: number;
  /** Still to fill, and as entered */
  qty: number;
  entered: number;
  tif: TimeInForce;
  /** Resting in the book, held by the broker until it triggers, or waiting for a cross */
  state: 'working' | 'held' | 'auction';
  short: boolean;
  hidden: boolean;
  displayQty: number;
  midpointPeg: boolean;
  extendedHours: boolean;
  /** The OCO or bracket it belongs to, '' if none */
  group: string;
}

export interface UserFill {
  simTimeMs: number;
  orderId: string;
  side: Side;
  price: number;
  qty: number;
  fee: number;
  liquidity: 'added' | 'removed' | 'cross';
  short: boolean;
}

export interface UserAccount {
  broker: string;
  scaled: boolean;
  /** Sim dollars per real-world dollar for this account */
  moneyScale: number;
  cash: number;
  escrow: number;
  equity: number;
  buyingPower: number;
  debit: number;
  long: number;
  short: number;
  borrowed: number;
  margin: boolean;
  violation: boolean;
  /** Net shares, long positive */
  position: number;
  averageCost: number;
  realizedPnl: number;
  unrealizedPnl: number;
  feesPaid: number;
  orders: UserOrder[];
  /** Executions since the last frame */
  fills: UserFill[];
  fillsDropped: number;
}

/** The outcome of one of the user's commands, sent to every client */
export interface UserResult {
  v: number;
  type: 'userResult';
  command: 'order' | 'oco' | 'bracket' | 'cancel' | 'replace' | 'sentiment';
  accepted: boolean;
  orderId?: string;
  reason?: string;
  simTimeMs: number;
  /** The request's id */
  echo?: string;
}

/** An order as the ticket sends it */
export interface OrderMessage {
  type: 'order';
  id?: string;
  side: 'buy' | 'sell' | 'sellShort';
  orderType: UserOrderType;
  qty: number;
  limitPrice?: number;
  stopPrice?: number;
  trailAmount?: number;
  trailPercent?: number;
  tif?: TimeInForce;
  expiresAtMs?: number;
  extendedHours?: boolean;
  postOnly?: boolean;
  hidden?: boolean;
  displayQty?: number;
  midpointPeg?: boolean;
  bracket?: { takeProfit: number; stopLoss: number };
  oco?: { stopPrice: number; stopLimitPrice?: number };
}

export interface Backfill {
  v: number;
  type: 'backfill';
  tickCountAtCapture: number;
  truncated: boolean;
  prints: PrintTuple[];
}

export interface Ack { v: number; type: 'ack'; of: string; echo?: string }
export interface ProtocolError { v: number; type: 'error'; message: string; echo?: string }

export type ServerMessage = Hello | Frame | Backfill | Ack | ProtocolError | UserResult;

/** Outbound control messages */
export type ClientMessage =
  | OrderMessage
  | { type: 'cancel'; orderId: string; id?: string }
  | { type: 'replace'; orderId: string; limitPrice: number; qty: number; id?: string }
  | { type: 'pause' | 'resume' | 'toggle' | 'step' | 'cancelBackData'; id?: string }
  | { type: 'speed'; value: number; id?: string }
  | { type: 'sentiment'; value: number; id?: string }
  | { type: 'sentimentNudge'; delta: number; id?: string }
  | { type: 'backfill'; limit?: number; id?: string }
  | { type: 'reset'; params: ResetParams; id?: string };

/* The tuple layouts this client was written against. Checked against `hello` on connect:
 * a server that reorders a tuple would otherwise be read silently and wrongly, which is
 * exactly the failure a version number alone does not catch. */
export const EXPECTED_TUPLES: Record<string, string[]> = {
  bookLevel: ['price', 'volume', 'orders'],
  print: ['epochSec', 'price', 'volume', 'side', 'kind'],
  log: ['simTimeMs', 'kind', 'text'],
};

export function checkTuples(hello: Hello): string | null {
  const tuples = hello.tuples as unknown as Record<string, string[] | undefined>;
  for (const [name, expected] of Object.entries(EXPECTED_TUPLES)) {
    const actual = tuples?.[name];
    if (!actual || actual.join(',') !== expected.join(',')) {
      return `server tuple layout for "${name}" is [${actual?.join(', ') ?? '?'}], ` +
        `this client expects [${expected.join(', ')}]`;
    }
  }
  if (hello.protocol !== PROTOCOL_VERSION) {
    return `server speaks protocol ${hello.protocol}, this client speaks ${PROTOCOL_VERSION}`;
  }
  return null;
}
