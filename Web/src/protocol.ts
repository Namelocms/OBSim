/* The wire protocol, as TypeScript types.
 *
 * Mirrors Server/include/Protocol.h. Repeated data arrives as TUPLES, not objects --
 * [price, volume, orders] for a book level, [epochSec, price, volume, side] for a trade
 * print. `hello` carries the layouts so this file can assert it still agrees with the
 * server rather than reading the wrong column if one ever changes.
 *
 * Two times are sent for every frame. `simTimeMs` is authoritative; `epochSec` is derived
 * and lossy below a microsecond, and exists so the chart can put a date on its axis.
 */

export const PROTOCOL_VERSION = 1;

export type Side = 'B' | 'S';

/** [price, volume, orders] */
export type BookLevelTuple = [number, number, number];
/** [epochSec, price, volume, side] */
export type PrintTuple = [number, number, number, Side];
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
  logs: LogTuple[];
  logsDropped: number;
  /* ABSENT means "roster unchanged, keep what you have". An empty rows array would mean
   * the market actually emptied -- the distinction is load bearing, so this is optional
   * rather than defaulted. */
  agents?: { rows: AgentRow[]; omitted: number };
  /* Present only while back data is building */
  backData?: BackDataProgress;
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

export type ServerMessage = Hello | Frame | Backfill | Ack | ProtocolError;

/** Outbound control messages. `order` is reserved by the server and refused by name. */
export type ClientMessage =
  | { type: 'pause' | 'resume' | 'toggle' | 'step' | 'cancelBackData'; id?: string }
  | { type: 'speed'; value: number; id?: string }
  | { type: 'sentiment'; value: number; id?: string }
  | { type: 'sentimentNudge'; delta: number; id?: string }
  | { type: 'backfill'; limit?: number; id?: string }
  | { type: 'reset'; params: Partial<SimParams>; id?: string };

/* The tuple layouts this client was written against. Checked against `hello` on connect:
 * a server that reorders a tuple would otherwise be read silently and wrongly, which is
 * exactly the failure a version number alone does not catch. */
export const EXPECTED_TUPLES: Record<string, string[]> = {
  bookLevel: ['price', 'volume', 'orders'],
  print: ['epochSec', 'price', 'volume', 'side'],
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
