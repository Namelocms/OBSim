/* Number and time formatting, shared by every panel so a price reads the same everywhere */

export const fmt = {
  price: (v: number) => v.toFixed(4),
  money: (v: number) =>
    Math.abs(v) >= 1e6 ? `$${(v / 1e6).toFixed(2)}M`
      : Math.abs(v) >= 1e3 ? `$${(v / 1e3).toFixed(1)}k`
        : `$${v.toFixed(2)}`,
  int: (v: number) => v.toLocaleString('en-US'),
  pct: (v: number) => `${(v * 100).toFixed(1)}%`,
  /** A change, signed, to two places: +2.41% */
  change: (v: number) => `${v >= 0 ? '+' : ''}${(v * 100).toFixed(2)}%`,
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
  /** A span of sim time as m:ss, for countdowns */
  span: (ms: number) => {
    const total = Math.max(0, Math.ceil(ms / 1000));
    return `${Math.floor(total / 60)}:${String(total % 60).padStart(2, '0')}`;
  },
};
