import './feature-switches.css';

import {
  DEFAULT_FEATURES, FEATURE_GROUPS, FEATURE_INFO, LULD_TIERS, withDefaults, type FeatureFlag,
} from './features';
import type { Features } from './protocol';

/* The order model switches, as a form section.
 *
 * Self-contained: it owns its elements and its styles, knows nothing about the dialog it sits
 * in, and talks only in Features. Whoever mounts it fills it from the running server's hello
 * and reads it back on submit, so it can move to a settings page, a sidebar or a preset
 * picker without changing.
 */

export class FeatureSwitches {
  readonly root = document.createElement('section');
  private boxes = new Map<FeatureFlag, HTMLInputElement>();
  private rows = new Map<FeatureFlag, HTMLElement>();
  private tier = document.createElement('select');

  constructor() {
    this.root.className = 'feature-switches';

    const head = document.createElement('div');
    head.className = 'fs-head';
    const title = document.createElement('span');
    title.className = 'fs-title';
    title.textContent = 'Order model';
    head.append(title);
    for (const [label, on] of [['none', false], ['all', true]] as const) {
      const btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'fs-quick';
      btn.textContent = label;
      btn.addEventListener('click', () => this.setAll(on));
      head.append(btn);
    }
    this.root.append(head);

    for (const group of FEATURE_GROUPS) {
      const box = document.createElement('div');
      box.className = 'fs-group';
      const heading = document.createElement('div');
      heading.className = 'fs-group-title';
      heading.textContent = group;
      box.append(heading);

      for (const info of FEATURE_INFO.filter((f) => f.group === group)) {
        const row = document.createElement('label');
        row.className = 'fs-row';
        const input = document.createElement('input');
        input.type = 'checkbox';
        input.setAttribute('aria-label', info.label);
        input.addEventListener('change', () => this.refresh());
        const name = document.createElement('span');
        name.className = 'fs-label';
        name.textContent = info.label;
        const hint = document.createElement('span');
        hint.className = 'fs-hint';
        hint.textContent = info.hint;
        row.append(input, name, hint);
        if (info.requires) {
          const needs = FEATURE_INFO.find((f) => f.key === info.requires)?.label ?? info.requires;
          row.title = `Does nothing unless ${needs} is on`;
        }
        this.boxes.set(info.key, input);
        this.rows.set(info.key, row);
        box.append(row);

        // The tier belongs to LULD, so it sits under it rather than in a list of its own
        if (info.key === 'luld') box.append(this.tierRow());
      }
      this.root.append(box);
    }

    this.fill(DEFAULT_FEATURES);
  }

  private tierRow(): HTMLElement {
    const row = document.createElement('label');
    row.className = 'fs-row fs-sub';
    const name = document.createElement('span');
    name.className = 'fs-label';
    name.textContent = 'Band tier';
    this.tier.className = 'fs-select';
    for (const { tier, label } of LULD_TIERS) {
      const option = document.createElement('option');
      option.value = String(tier);
      option.textContent = label;
      this.tier.append(option);
    }
    row.append(name, this.tier);
    return row;
  }

  /** Show a set of switches -- normally the running server's, from hello */
  fill(features?: Partial<Features>): void {
    const f = withDefaults(features);
    for (const [key, input] of this.boxes) input.checked = f[key];
    this.tier.value = String(f.luldTier);
    this.refresh();
  }

  read(): Features {
    const f = withDefaults();
    for (const [key, input] of this.boxes) f[key] = input.checked;
    f.luldTier = this.tier.value === '1' ? 1 : 2;
    return f;
  }

  private setAll(on: boolean): void {
    for (const input of this.boxes.values()) input.checked = on;
    this.refresh();
  }

  /** Dim what an unchecked prerequisite strands. Still sent as set: the engine ignores it. */
  private refresh(): void {
    for (const info of FEATURE_INFO) {
      const row = this.rows.get(info.key);
      if (!row) continue;
      const stranded = info.requires !== undefined && !this.boxes.get(info.requires)?.checked;
      row.classList.toggle('fs-stranded', stranded);
    }
    this.tier.disabled = !this.boxes.get('luld')?.checked;
  }
}
