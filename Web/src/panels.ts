import type { AgentRow, Frame, Hello, SimParams } from './protocol';

/* The DOM panels: header, agent roster, event log, controls, reset dialog, back-data
 * overlay. No framework -- these update a handful of text nodes at 30 frames a second,
 * which is where a reconciler costs the most and buys the least.
 *
 * Each panel caches the nodes it writes to and only touches the ones that changed. That is
 * not premature: at 30Hz, rebuilding the header's innerHTML is enough to make text
 * selection impossible and to keep the main thread busy for no reason.
 */

function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  className?: string,
  text?: string,
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

const fmt = {
  price: (v: number) => v.toFixed(4),
  money: (v: number) =>
    Math.abs(v) >= 1e6 ? `$${(v / 1e6).toFixed(2)}M`
      : Math.abs(v) >= 1e3 ? `$${(v / 1e3).toFixed(1)}k`
        : `$${v.toFixed(2)}`,
  int: (v: number) => v.toLocaleString('en-US'),
  pct: (v: number) => `${(v * 100).toFixed(1)}%`,
  /** Sim time as the market clock it represents: t = 0 is 04:00 on day 0 */
  clock: (simTimeMs: number) => {
    const totalMin = simTimeMs / 60000;
    const day = Math.floor(totalMin / 1440);
    const intoDay = totalMin - day * 1440;
    const mins = Math.floor((intoDay + 240) % 1440);
    const hh = String(Math.floor(mins / 60)).padStart(2, '0');
    const mm = String(Math.floor(mins % 60)).padStart(2, '0');
    return `D${day + 1} ${hh}:${mm}`;
  },
};

// ---------------------------------------------------------------- header

export class Header {
  readonly root = el('header', 'header');
  private fields = new Map<string, HTMLElement>();
  private statusDot = el('span', 'dot');
  private statusText = el('span', 'status-text', 'connecting');

  constructor() {
    const brand = el('div', 'brand');
    brand.append(el('span', 'brand-name', 'OBSim'));
    this.root.append(brand);

    for (const [key, label] of [
      ['price', 'PRICE'], ['spread', 'SPREAD'], ['session', 'SESSION'], ['clock', 'CLOCK'],
      ['trades', 'TRADES'], ['resting', 'RESTING'], ['agents', 'AGENTS'],
      ['float', 'FLOAT'], ['sentiment', 'SENTIMENT'],
    ] as const) {
      const wrap = el('div', 'stat');
      wrap.append(el('span', 'stat-label', label));
      const value = el('span', 'stat-value', '--');
      wrap.append(value);
      this.fields.set(key, value);
      this.root.append(wrap);
    }

    const status = el('div', 'conn');
    status.append(this.statusDot, this.statusText);
    this.root.append(status);
  }

  private set(key: string, text: string, cls?: string): void {
    const node = this.fields.get(key);
    if (!node) return;
    if (node.textContent !== text) node.textContent = text;
    const want = `stat-value${cls ? ` ${cls}` : ''}`;
    if (node.className !== want) node.className = want;
  }

  update(f: Frame): void {
    this.set('price', `$${fmt.price(f.price)}`);
    this.set('spread', f.spread > 0 ? `$${fmt.price(f.spread)}` : '--');
    this.set('session', f.session, `session-${f.session.toLowerCase()}`);
    this.set('clock', fmt.clock(f.simTimeMs));
    this.set('trades', fmt.int(f.tickCount));
    this.set('resting', `${fmt.int(f.restingBids)} / ${fmt.int(f.restingAsks)}`);
    // Residents and live transients are different populations, not one number
    this.set(
      'agents',
      f.pop.fraction > 0
        ? `${fmt.int(f.pop.residents)} +${fmt.int(f.pop.transients)}`
        : fmt.int(f.pop.residents),
    );
    this.set('float', fmt.int(f.shareFloat));
    this.set('sentiment', f.sentiment.toFixed(3),
      f.sentiment > 0.001 ? 'up' : f.sentiment < -0.001 ? 'down' : undefined);
  }

  setConnection(state: string, detail?: string): void {
    this.statusDot.className = `dot dot-${state}`;
    this.statusText.textContent = detail ? `${state} - ${detail}` : state;
  }
}

// ---------------------------------------------------------------- agents

export class AgentTable {
  readonly root = el('div', 'panel agents');
  private body = el('div', 'agent-body');
  private title = el('span', 'panel-title', 'Agents');
  private rows: AgentRow[] = [];
  private omitted = 0;
  private filter = '';

  constructor() {
    const head = el('div', 'panel-head');
    head.append(this.title);
    const search = el('input', 'agent-filter');
    search.type = 'search';
    search.placeholder = 'filter by id, type, status';
    search.addEventListener('input', () => {
      this.filter = search.value.trim().toUpperCase();
      this.render();
    });
    head.append(search);
    this.root.append(head, this.body);
  }

  /** Only called when a frame actually carries a roster. An absent roster means
   *  unchanged, so keeping the previous rows is correct rather than lazy. */
  set(rows: AgentRow[], omitted: number): void {
    this.rows = rows;
    this.omitted = omitted;
    this.render();
  }

  private render(): void {
    const matches = this.filter
      ? this.rows.filter((r) =>
        r.id.toUpperCase().includes(this.filter)
        || r.type.includes(this.filter)
        || r.subType.includes(this.filter)
        || r.status.includes(this.filter))
      : this.rows;

    const population = this.rows.length + this.omitted;
    const shown = this.filter ? `${matches.length} of ${this.rows.length} shown, ` : '';
    this.title.textContent = this.omitted > 0
      ? `Agents (${shown}${fmt.int(this.rows.length)} of ${fmt.int(population)} sent)`
      : `Agents (${shown}${fmt.int(population)})`;

    // Rebuild in one pass into a fragment. The roster arrives at 4Hz, not 30Hz, so this
    // is affordable -- it is the reason the roster has its own cadence at all.
    const frag = document.createDocumentFragment();
    const header = el('div', 'agent-row agent-head');
    for (const [label, cls] of [
      ['ID', 'c-id'], ['TYPE', 'c-type'], ['CASH', 'c-num'], ['HELD', 'c-num'],
      ['BID', 'c-num'], ['ASK', 'c-num'], ['SENT', 'c-num'], ['ST', 'c-status'],
    ] as const) {
      header.append(el('span', cls, label));
    }
    frag.append(header);

    for (const r of matches) {
      const row = el('div', `agent-row${r.transient ? ' transient' : ''}`);
      row.append(el('span', 'c-id', (r.transient ? '~' : '') + r.id.replace(/^A-0+/, 'A-')));
      row.append(el('span', 'c-type', `${r.type[0]}/${r.subType.slice(0, 4)}`));
      row.append(el('span', 'c-num', fmt.money(r.cash)));
      row.append(el('span', 'c-num', fmt.int(r.holdings)));
      row.append(el('span', 'c-num', String(r.bids)));
      row.append(el('span', 'c-num', String(r.asks)));
      const sent = el('span', 'c-num', r.sentiment.toFixed(2));
      if (r.sentiment > 0.05) sent.classList.add('up');
      else if (r.sentiment < -0.05) sent.classList.add('down');
      row.append(sent);
      const status = r.stranded ? 'STK' : r.status.slice(0, 3);
      row.append(el('span', `c-status st-${status.toLowerCase()}`, status));
      frag.append(row);
    }

    this.body.replaceChildren(frag);
  }
}

// ---------------------------------------------------------------- log

export class EventLog {
  readonly root = el('div', 'panel log');
  private body = el('div', 'log-body');
  private title = el('span', 'panel-title', 'Event log');
  private dropped = 0;
  private pinned = true;

  constructor(private cap = 300) {
    const head = el('div', 'panel-head');
    head.append(this.title);
    head.append(el('span', 'panel-note', 'newest last'));
    this.root.append(head, this.body);

    // Unpin when the user scrolls up to read, repin when they return to the bottom
    this.body.addEventListener('scroll', () => {
      const atBottom =
        this.body.scrollHeight - this.body.scrollTop - this.body.clientHeight < 24;
      this.pinned = atBottom;
    });
  }

  append(lines: [number, string, string][], droppedThisFrame: number): void {
    this.dropped += droppedThisFrame;
    if (lines.length > 0) {
      const frag = document.createDocumentFragment();
      for (const [simTimeMs, kind, text] of lines) {
        const row = el('div', `log-line kind-${kind.toLowerCase()}`);
        row.append(el('span', 'log-time', fmt.clock(simTimeMs)));
        row.append(el('span', 'log-kind', kind));
        row.append(el('span', 'log-text', text));
        frag.append(row);
      }
      this.body.append(frag);
      while (this.body.childElementCount > this.cap) this.body.firstElementChild?.remove();
      if (this.pinned) this.body.scrollTop = this.body.scrollHeight;
    }

    // Dropped lines are normal at speed -- onLog fires on every agent event and the
    // server caps per frame. Saying so is better than implying the market went quiet.
    this.title.textContent = this.dropped > 0
      ? `Event log (${fmt.int(this.dropped)} dropped)`
      : 'Event log';
  }

  clear(): void {
    this.body.replaceChildren();
    this.dropped = 0;
    this.pinned = true;
  }
}

// ---------------------------------------------------------------- controls

export interface ControlHandlers {
  onToggle: () => void;
  onStep: () => void;
  onSpeed: (multiplier: number) => void;
  onSentiment: (delta: number) => void;
  onTimeframe: (index: number) => void;
  onReset: () => void;
}

export const SPEEDS = [0.25, 0.5, 1, 2, 5, 10, 25, 50, 100, 250, 1000];

export class Controls {
  readonly root = el('div', 'controls');
  private playBtn = el('button', 'btn btn-play', 'Pause');
  private speedLabel = el('span', 'speed-value', '1x');
  private speedIndex = SPEEDS.indexOf(1);
  private timeframeButtons: HTMLButtonElement[] = [];

  constructor(handlers: ControlHandlers, timeframeLabels: string[]) {
    this.playBtn.addEventListener('click', handlers.onToggle);
    this.root.append(this.playBtn);

    const step = el('button', 'btn', 'Step');
    step.addEventListener('click', handlers.onStep);
    this.root.append(step);

    const group = el('div', 'group');
    const slower = el('button', 'btn btn-narrow', '-');
    const faster = el('button', 'btn btn-narrow', '+');
    slower.addEventListener('click', () => {
      this.speedIndex = Math.max(0, this.speedIndex - 1);
      handlers.onSpeed(SPEEDS[this.speedIndex] ?? 1);
    });
    faster.addEventListener('click', () => {
      this.speedIndex = Math.min(SPEEDS.length - 1, this.speedIndex + 1);
      handlers.onSpeed(SPEEDS[this.speedIndex] ?? 1);
    });
    group.append(el('span', 'group-label', 'SPEED'), slower, this.speedLabel, faster);
    this.root.append(group);

    const tf = el('div', 'group');
    tf.append(el('span', 'group-label', 'BARS'));
    timeframeLabels.forEach((label, i) => {
      const btn = el('button', 'btn btn-narrow', label);
      btn.addEventListener('click', () => handlers.onTimeframe(i));
      this.timeframeButtons.push(btn);
      tf.append(btn);
    });
    this.root.append(tf);

    const sent = el('div', 'group');
    const down = el('button', 'btn btn-narrow', 'bear');
    const up = el('button', 'btn btn-narrow', 'bull');
    down.addEventListener('click', () => handlers.onSentiment(-0.01));
    up.addEventListener('click', () => handlers.onSentiment(0.01));
    sent.append(el('span', 'group-label', 'MARKET'), down, up);
    this.root.append(sent);

    this.root.append(el('div', 'spacer'));
    const reset = el('button', 'btn btn-reset', 'Reset run');
    reset.addEventListener('click', handlers.onReset);
    this.root.append(reset);
  }

  setTimeframe(index: number): void {
    this.timeframeButtons.forEach((b, i) => b.classList.toggle('active', i === index));
  }

  update(f: Frame): void {
    const label = f.clock.paused ? 'Resume' : 'Pause';
    if (this.playBtn.textContent !== label) this.playBtn.textContent = label;
    this.playBtn.classList.toggle('paused', f.clock.paused);

    // Follow the server rather than assuming our own button press won. A second client,
    // or a reset, can change the speed underneath this one.
    const speedText = `${f.clock.speed}x`;
    if (this.speedLabel.textContent !== speedText) {
      this.speedLabel.textContent = speedText;
      const idx = SPEEDS.indexOf(f.clock.speed);
      if (idx >= 0) this.speedIndex = idx;
    }
  }
}

// ---------------------------------------------------------------- reset dialog

export class ResetDialog {
  readonly root = el('div', 'overlay hidden');
  private inputs = new Map<keyof SimParams, HTMLInputElement | HTMLSelectElement>();
  private derivedNote = el('div', 'dialog-note');

  constructor(private onSubmit: (params: Partial<SimParams>) => void) {
    const box = el('div', 'dialog');
    box.append(el('h2', 'dialog-title', 'Reset simulation'));

    const fields: Array<[keyof SimParams, string, string, string]> = [
      ['seed', 'Seed', 'number', '1'],
      ['backDataDays', 'Back data (days)', 'number', '1'],
      ['minLiquidity', 'Min liquidity', 'number', '0'],
      ['agentCount', 'Agent count', 'number', '500'],
      ['shareFloat', 'Share float', 'number', '250000'],
      ['startPrice', 'Start price', 'number', '1.00'],
      ['transientFraction', 'Transient fraction', 'number', '0.04'],
    ];

    for (const [key, label, type, value] of fields) {
      const row = el('label', 'dialog-row');
      row.append(el('span', 'dialog-label', label));
      const input = el('input', 'dialog-input');
      input.type = type;
      input.value = value;
      if (key === 'startPrice') input.step = '0.01';
      if (key === 'transientFraction') input.step = '0.01';
      row.append(input);
      this.inputs.set(key, input);
      box.append(row);
    }

    const sessionRow = el('label', 'dialog-row');
    sessionRow.append(el('span', 'dialog-label', 'Live start'));
    const select = el('select', 'dialog-input');
    for (const name of ['PREMARKET', 'REGULAR', 'AFTERHOURS']) {
      const option = el('option');
      option.value = name;
      option.textContent = name;
      if (name === 'REGULAR') option.selected = true;
      select.append(option);
    }
    sessionRow.append(select);
    this.inputs.set('liveStartSession', select);
    box.append(sessionRow);

    // The one field whose effect cannot be seen before pressing Go
    box.append(this.derivedNote);
    const agentInput = this.inputs.get('agentCount');
    const updateNote = () => {
      const n = Number(agentInput instanceof HTMLInputElement ? agentInput.value : '0');
      this.derivedNote.textContent = n === 0
        ? 'Agent count 0 derives the population from the market cap. A bigger float means '
        + 'more agents, more memory and a longer back-data run.'
        : '';
    };
    agentInput?.addEventListener('input', updateNote);
    updateNote();

    const buttons = el('div', 'dialog-buttons');
    const cancel = el('button', 'btn', 'Cancel');
    cancel.addEventListener('click', () => this.hide());
    const go = el('button', 'btn btn-primary', 'Go');
    go.addEventListener('click', () => this.submit());
    buttons.append(cancel, go);
    box.append(buttons);

    this.root.append(box);
    this.root.addEventListener('click', (e) => {
      if (e.target === this.root) this.hide();
    });
  }

  private submit(): void {
    const params: Partial<SimParams> = {};
    for (const [key, input] of this.inputs) {
      if (key === 'liveStartSession') {
        params.liveStartSession = input.value as SimParams['liveStartSession'];
        continue;
      }
      const n = Number(input.value);
      if (Number.isFinite(n)) (params as Record<string, number>)[key] = n;
    }
    this.hide();
    this.onSubmit(params);
  }

  /** Prefill from what the server says is actually running, not from stale defaults */
  showFrom(hello: Hello | null): void {
    if (hello) {
      for (const [key, input] of this.inputs) {
        const value = hello.params[key];
        if (value !== undefined) input.value = String(value);
      }
    }
    this.root.classList.remove('hidden');
  }

  hide(): void {
    this.root.classList.add('hidden');
  }

  get visible(): boolean {
    return !this.root.classList.contains('hidden');
  }
}

// ---------------------------------------------------------------- back data overlay

export class BackDataOverlay {
  readonly root = el('div', 'overlay hidden');
  private bar = el('div', 'progress-fill');
  private percent = el('span', 'progress-percent', '0%');
  private rows = new Map<string, HTMLElement>();

  constructor(onCancel: () => void) {
    const box = el('div', 'dialog');
    box.append(el('h2', 'dialog-title', 'Building back data'));
    box.append(el('p', 'dialog-note',
      'The market is being simulated from t = 0 up to the live start, headless and '
      + 'unthrottled. Nothing is shown until it reaches the handoff.'));

    const track = el('div', 'progress-track');
    track.append(this.bar);
    const progress = el('div', 'progress');
    progress.append(track, this.percent);
    box.append(progress);

    for (const [key, label] of [
      ['session', 'Session'], ['day', 'Day'], ['clock', 'Market clock'],
      ['minutes', 'Minutes'], ['trades', 'Trades'], ['events', 'Events'],
      ['transients', 'Transients'], ['extra', 'Extra days'],
    ] as const) {
      const row = el('div', 'dialog-row');
      row.append(el('span', 'dialog-label', label));
      const value = el('span', 'dialog-value', '--');
      row.append(value);
      this.rows.set(key, value);
      box.append(row);
    }

    const buttons = el('div', 'dialog-buttons');
    const cancel = el('button', 'btn', 'Cancel and reconfigure');
    cancel.addEventListener('click', onCancel);
    buttons.append(cancel);
    box.append(buttons);
    this.root.append(box);
  }

  private set(key: string, text: string): void {
    const node = this.rows.get(key);
    if (node && node.textContent !== text) node.textContent = text;
  }

  update(f: Frame): void {
    const p = f.backData;
    if (!p) return;
    this.bar.style.width = `${Math.max(0, Math.min(100, p.percent))}%`;
    this.percent.textContent = `${p.percent.toFixed(1)}%`;
    this.set('session', p.session);
    this.set('day', String(p.dayIndex + 1));
    this.set('clock', fmt.clock(p.totalMinutes * 60000));
    this.set('minutes', `${p.totalMinutes.toFixed(0)} / ${p.targetMinutes.toFixed(0)}`);
    this.set('trades', fmt.int(p.ticks));
    this.set('events', fmt.int(p.events));
    this.set('transients', p.transientFraction > 0
      ? `${p.liveTransients} live, ${fmt.int(p.transientArrivals)} total `
      + `(${fmt.pct(p.transientFraction)} of residents this run)`
      : 'off');
    this.set('extra', p.extraDays > 0
      ? `${p.extraDays} added chasing minimum liquidity` : 'none');
  }

  setVisible(visible: boolean): void {
    this.root.classList.toggle('hidden', !visible);
  }
}
