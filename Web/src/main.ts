import './style.css';

import { BarSeries, TIMEFRAMES } from './bars';
import { PriceChart } from './chart';
import { ChartOverlays } from './chart-overlays';
import { Connection, type ConnectionState } from './connection';
import { FeatureSwitches } from './feature-switches';
import { countOn, withDefaults } from './features';
import { DepthLadder } from './ladder';
import { BROKER_CHIPS, MARKET_CHIPS, StatusStrip } from './market-status';
import {
  AgentTable, BackDataOverlay, Controls, EventLog, Header, ResetDialog,
} from './panels';
import { checkTuples, type Features, type Frame, type Hello, type ServerMessage } from './protocol';
import { rosterColumns } from './roster-columns';

/* Wiring.
 *
 * One connection, one bar series, and the panels. The only real subtlety is that frames are
 * applied as they arrive rather than on an animation frame: the server has already
 * coalesced to ~30/s against the wall clock, so re-coalescing here would add latency to
 * fix a problem that was solved upstream.
 *
 * This file is the only one that knows the layout. Every panel is a self-contained module
 * with a root element and an update method, so moving one is an edit here and nowhere else.
 */

const DEFAULT_URL = `ws://${location.hostname || '127.0.0.1'}:8787`;
// ?server=host:port lets one page drive an engine on another port without a rebuild
const serverParam = new URLSearchParams(location.search).get('server');
const SERVER_URL = serverParam
  ? (serverParam.startsWith('ws') ? serverParam : `ws://${serverParam}`)
  : DEFAULT_URL;

const app = document.getElementById('app');
if (!app) throw new Error('#app missing');

// ---- layout ----------------------------------------------------------------

const header = new Header();

const chartPanel = document.createElement('div');
chartPanel.className = 'panel chart';
const chartHead = document.createElement('div');
chartHead.className = 'panel-head';
const chartTitle = document.createElement('span');
chartTitle.className = 'panel-title';
chartTitle.textContent = 'Price';
const chartNote = document.createElement('span');
chartNote.className = 'panel-note';
chartHead.append(chartTitle, chartNote);
const chartBody = document.createElement('div');
chartBody.className = 'chart-body';
chartPanel.append(chartHead, chartBody);

const ladderPanel = document.createElement('div');
ladderPanel.className = 'panel ladder';
const ladderHead = document.createElement('div');
ladderHead.className = 'panel-head';
const ladderTitle = document.createElement('span');
ladderTitle.className = 'panel-title';
ladderTitle.textContent = 'Order book';
ladderHead.append(ladderTitle);
const ladderBody = document.createElement('div');
ladderBody.className = 'ladder-body';
ladderPanel.append(ladderHead, ladderBody);

const agents = new AgentTable();
const log = new EventLog();
// The order model's state: the market's structure in a row under the header, the brokers'
// activity in a panel over the log. Both hide themselves while nothing is switched on.
const marketStatus = new StatusStrip(MARKET_CHIPS);
const brokerStatus = new StatusStrip(BROKER_CHIPS, { title: 'Brokers' });
const switches = new FeatureSwitches();

const upper = document.createElement('div');
upper.className = 'row upper';
upper.append(chartPanel, ladderPanel);

const side = document.createElement('div');
side.className = 'stack';
side.append(brokerStatus.root, log.root);

const lower = document.createElement('div');
lower.className = 'row lower';
lower.append(agents.root, side);

// ---- state -----------------------------------------------------------------

let hello: Hello | null = null;
let features: Features = withDefaults();
let timeframeIndex = 3; // 1m
const bars = new BarSeries(TIMEFRAMES[timeframeIndex] ?? TIMEFRAMES[0]!);
let lastFrame: Frame | null = null;
let seenSequence = 0;
let gapCount = 0;

const chart = new PriceChart(chartBody);
const overlays = new ChartOverlays(chart.priceSeries, TIMEFRAMES[timeframeIndex] ?? TIMEFRAMES[0]!);
const ladder = new DepthLadder(ladderBody);

const controls = new Controls(
  {
    onToggle: () => connection.send({ type: 'toggle' }),
    onStep: () => connection.send({ type: 'step' }),
    onSpeed: (value) => connection.send({ type: 'speed', value }),
    onSentiment: (delta) => connection.send({ type: 'sentimentNudge', delta }),
    onTimeframe: (index) => setTimeframe(index),
    onReset: () => openReset(),
  },
  TIMEFRAMES.map((t) => t.label),
);

const resetDialog = new ResetDialog((params) => {
  // The engine restarts from t = 0, so everything drawn from the old run must go
  bars.reset(TIMEFRAMES[timeframeIndex] ?? TIMEFRAMES[0]!);
  chart.clear();
  overlays.clear();
  log.clear();
  marketStatus.clear();
  brokerStatus.clear();
  seenSequence = 0;
  gapCount = 0;
  connection.send({ type: 'reset', params: { ...params, features: switches.read() } });
});
resetDialog.mount(switches.root);

/** Prefilled from what the server says is running, switches included */
function openReset(): void {
  switches.fill(hello?.params.features);
  resetDialog.showFrom(hello);
}

const backData = new BackDataOverlay(() => {
  connection.send({ type: 'cancelBackData' });
  openReset();
});

app.append(header.root, marketStatus.root, upper, lower, controls.root, resetDialog.root, backData.root);
controls.setTimeframe(timeframeIndex);

function setTimeframe(index: number): void {
  const bucketer = TIMEFRAMES[index];
  if (!bucketer) return;
  timeframeIndex = index;
  controls.setTimeframe(index);
  // Re-bucketing needs the whole print stream again, and only the server has it.
  // Asking is cheaper and more honest than keeping a second copy of history here.
  bars.reset(bucketer);
  chart.clear();
  overlays.setBucketer(bucketer);
  connection.send({ type: 'backfill' });
}

// ---- messages --------------------------------------------------------------

function onMessage(msg: ServerMessage): void {
  switch (msg.type) {
    case 'hello': {
      hello = msg;
      const mismatch = checkTuples(msg);
      if (mismatch) {
        // A reordered tuple would be read silently and wrongly, which a version number
        // alone does not catch. Say so loudly rather than drawing nonsense.
        header.setConnection('closed', mismatch);
        connection.close();
        return;
      }
      // A hello arrives on connect and again after every reset, so the switches here are
      // always the running engine's
      features = withDefaults(msg.params.features);
      marketStatus.setFeatures(features);
      brokerStatus.setFeatures(features);
      agents.setColumns(rosterColumns(features));
      const on = countOn(features);
      chartNote.textContent =
        `seed ${msg.params.seed} - ${msg.params.agentCount === 0 ? 'derived' : msg.params.agentCount} agents`
        + ` - float ${msg.params.shareFloat.toLocaleString('en-US')}`
        + (on > 0 ? ` - ${on} order model switch${on === 1 ? '' : 'es'} on` : '');
      return;
    }

    case 'backfill': {
      // Rebuild from scratch: this arrives on connect and on a timeframe change, and in
      // both cases whatever is on the chart is either empty or the wrong shape.
      bars.reset(TIMEFRAMES[timeframeIndex] ?? TIMEFRAMES[0]!, msg.prints);
      chart.setAll(bars.all);
      overlays.reset(msg.prints);
      chartTitle.textContent = msg.truncated
        ? `Price (history from ${msg.prints.length.toLocaleString('en-US')} of `
        + `${msg.tickCountAtCapture.toLocaleString('en-US')} trades)`
        : 'Price';
      return;
    }

    case 'frame': {
      applyFrame(msg);
      return;
    }

    case 'error': {
      log.append([[lastFrame?.simTimeMs ?? 0, 'ERROR', msg.message]], 0);
      return;
    }

    default:
      return;
  }
}

function applyFrame(f: Frame): void {
  // A gap means frames were lost in flight. It is not fatal -- the next frame carries
  // absolute state -- but the trades in the missing frames are gone, so the chart would
  // quietly under-report volume. Count it and say so rather than pretending.
  if (seenSequence !== 0 && f.seq !== seenSequence + 1) gapCount += f.seq - seenSequence - 1;
  seenSequence = f.seq;
  lastFrame = f;

  backData.setVisible(f.clock.backDataRunning);
  if (f.clock.backDataRunning) {
    backData.update(f);
    header.update(f);
    return;
  }

  header.update(f);
  controls.update(f);
  marketStatus.update(f);
  brokerStatus.update(f);

  if (f.prints.length > 0) {
    bars.add(f.prints);
    const last = bars.last;
    if (last) chart.update(last);
    overlays.add(f.prints);
  }
  overlays.update(f);

  ladder.set({ bids: f.book.bids, asks: f.book.asks, spread: f.spread, price: f.price });

  // Absent, not empty, means the roster is unchanged -- so only touch the table when the
  // server actually sent one.
  if (f.agents) agents.set(f.agents.rows, f.agents.omitted);

  log.append(f.logs, f.logsDropped);

  if (gapCount > 0 || f.printsDropped > 0) {
    chartTitle.textContent =
      `Price (${gapCount} frames lost, ${f.printsDropped} trades dropped)`;
  }
}

function onState(state: ConnectionState, detail?: string): void {
  header.setConnection(state, detail);
  if (state === 'open') {
    // The server sends a backfill on connect unasked, so nothing is needed here beyond
    // clearing what belonged to the previous connection.
    seenSequence = 0;
    gapCount = 0;
  }
}

const connection = new Connection(SERVER_URL, { onMessage, onState });
connection.connect();

// ---- keyboard --------------------------------------------------------------

// The terminal UI's bindings, kept: anyone moving between the two should not have to
// relearn them.
window.addEventListener('keydown', (e) => {
  const target = e.target as HTMLElement | null;
  if (target && (target.tagName === 'INPUT' || target.tagName === 'SELECT')) return;
  if (resetDialog.visible && e.key !== 'Escape') return;

  switch (e.key) {
    case ' ':
      e.preventDefault();
      connection.send({ type: 'toggle' });
      break;
    case 's': case 'S':
      connection.send({ type: 'step' });
      break;
    case 'ArrowRight': case '+': case '=':
      controls.root.querySelectorAll('button')[3]?.click();
      break;
    case 'ArrowLeft': case '-':
      controls.root.querySelectorAll('button')[2]?.click();
      break;
    case 't': case 'T':
      setTimeframe((timeframeIndex + 1) % TIMEFRAMES.length);
      break;
    case ',': case '<':
      connection.send({ type: 'sentimentNudge', delta: -0.01 });
      break;
    case '.': case '>':
      connection.send({ type: 'sentimentNudge', delta: 0.01 });
      break;
    case 'r': case 'R':
      openReset();
      break;
    case 'Escape':
      resetDialog.hide();
      break;
    default:
      break;
  }
});
