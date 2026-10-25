// Editor rendition of the e-ink panel renderer, ported from
// firmware/components/dashboard_ui/src/{renderer.cpp drawPanel, panels.cpp,
// panel_style.cpp, text.cpp}. Drawn as SVG in device pixels (1:1 with the
// 552×768 screen), so proportions match the device: 1px rounded frame (r 6),
// 8px padding, caption-bold title + right accessory, value "tiers" (native
// 80/48/28/20 px Noto Sans CJK Bold digits, then UI body/caption), sparkline,
// charts, tables, status chips, alert lists and the shared empty state.
// Numbers use the exact firmware formatting (model/format.ts).
import { memo, type ReactNode } from 'react';
import type { Bundle, Panel, Severity, TableColumn } from '../../model/types';
import type { ObservedMetrics } from '../../model/metrics';
import { panelSeries, type Point } from '../../model/series';
import { formatNumber, formatSigned, formatTime, formatValueText, joined } from '../../model/format';
import { DISPLAY_GLYPHS } from '../../model/displayFont';

// ------------------------------------------------------------------ metrics
const UI_PX = 12; // UI font size (render harness: Noto Sans CJK 12 px)
const LH = 17; // style::lineHeight(font) at caption scale
const ASC = 14; // style::ascent(font)
const PAD = 8; // style::kPad
const RADIUS = 6; // style::kRadius
const TITLE_GAP = 6; // style::kTitleGap
const BADGE_H = LH + 2;
const ZEBRA = '#b4b4b4'; // gray 11 (light)
const DISPLAY = [
  { cap: 80, asc: 80, desc: 23 },
  { cap: 48, asc: 48, desc: 13 },
  { cap: 28, asc: 28, desc: 8 },
  { cap: 20, asc: 20, desc: 6 },
]; // tiers 0..3 (largest first); tier 4 = UI body, 5 = UI caption
const TIERS = DISPLAY.length + 2;
const DIGIT_EM = 0.733; // digit height of Noto Sans CJK in em
const FAMILY = "'Noto Sans SC', 'Noto Sans CJK SC', 'Source Han Sans SC', 'PingFang SC', 'Microsoft YaHei', system-ui, sans-serif";
const RANK: Record<Severity, number> = { info: 0, warning: 1, critical: 2 };

const lineHeight = (scale = 1) => LH * scale;
const ascent = (scale = 1) => ASC * scale;
const tierHeight = (t: number) => (t < DISPLAY.length ? DISPLAY[t].asc + DISPLAY[t].desc : lineHeight(t === DISPLAY.length ? 2 : 1));
const tierAscent = (t: number) => (t < DISPLAY.length ? DISPLAY[t].asc : ascent(t === DISPLAY.length ? 2 : 1));

let ctx: CanvasRenderingContext2D | null | undefined;
const charCache = new Map<string, number>();
/** Integer advance of one character in the 12 px UI font (FreeType-like rounding). */
function charAdvance(ch: string): number {
  const hit = charCache.get(ch);
  if (hit !== undefined) return hit;
  if (ctx === undefined) {
    try {
      ctx = typeof document !== 'undefined' ? document.createElement('canvas').getContext('2d') : null;
    } catch {
      ctx = null;
    }
  }
  let w: number;
  if (ctx) {
    ctx.font = `400 ${UI_PX}px ${FAMILY}`;
    w = Math.round(ctx.measureText(ch).width);
  } else w = /[\u2e80-\u9fff\uff00-\uffef]/.test(ch) ? UI_PX : Math.round(UI_PX * 0.55);
  charCache.set(ch, w);
  return w;
}
/** UI font width; bold is drawn as regular twice (+1 px), like the firmware, so widths match regular. */
function measureText(text: string, scale = 1, _bold = false): number {
  let w = 0;
  for (const ch of text) w += charAdvance(ch);
  return w * scale;
}
// Display tiers 0..3 use the native fonts' glyph advances (sizes 80/48/28/20).
const displayMaps = DISPLAY_GLYPHS.map((g) => new Map([...g.chars].map((c, i) => [c, g.advances[i]] as [string, number])));
const fallbackScale = (t: number) => Math.floor((DISPLAY[t].asc + DISPLAY[t].desc + Math.floor(LH / 2)) / LH);
function tierTextWidth(t: number, text: string): number {
  if (t >= DISPLAY.length) return measureText(text, t === DISPLAY.length ? 2 : 1);
  const map = displayMaps[DISPLAY.length - 1 - t];
  let w = 0;
  for (const ch of text) w += map.get(ch) ?? charAdvance(ch) * fallbackScale(t);
  return w;
}

function ellipsize(text: string, w: number, scale: number, bold: boolean): string {
  if (measureText(text, scale, bold) <= w) return text;
  const chars = [...text];
  while (chars.length && measureText(chars.join('') + '...', scale, bold) > w) chars.pop();
  return chars.length ? chars.join('') + '...' : '';
}

// ------------------------------------------------------------------ painter
interface Box {
  x: number;
  y: number;
  w: number;
  h: number;
}
type Align = 'left' | 'center' | 'right';
const inset = (b: Box, n: number): Box => ({ x: b.x + n, y: b.y + n, w: b.w - 2 * n, h: b.h - 2 * n });
const right = (b: Box) => b.x + b.w;
const bottom = (b: Box) => b.y + b.h;
const empty = (b: Box) => b.w <= 0 || b.h <= 0;

class Painter {
  nodes: ReactNode[] = [];
  private k = 0;
  push(n: (key: number) => ReactNode) {
    this.nodes.push(n(this.k++));
  }
  fill(x: number, y: number, w: number, h: number, color = '#000') {
    if (w <= 0 || h <= 0) return;
    this.push((key) => <rect key={key} x={x} y={y} width={w} height={h} fill={color} />);
  }
  roundRect(x: number, y: number, w: number, h: number, r: number, fill = false, color = '#000') {
    if (w <= 1 || h <= 1) return;
    this.push((key) =>
      fill ? <rect key={key} x={x} y={y} width={w} height={h} rx={r} fill={color} /> : <rect key={key} x={x + 0.5} y={y + 0.5} width={w - 1} height={h - 1} rx={Math.max(0, r - 0.5)} fill="none" stroke={color} />,
    );
  }
  rect(x: number, y: number, w: number, h: number) {
    this.push((key) => <rect key={key} x={x + 0.5} y={y + 0.5} width={w - 1} height={h - 1} fill="none" stroke="#000" />);
  }
  dottedH(x0: number, x1: number, y: number, step = 3) {
    if (x1 < x0) return;
    this.push((key) => <line key={key} x1={x0} x2={x1 + 1} y1={y + 0.5} y2={y + 0.5} stroke="#000" strokeDasharray={`1 ${step - 1}`} />);
  }
  dottedV(x: number, y0: number, y1: number, step = 3) {
    this.push((key) => <line key={key} x1={x + 0.5} x2={x + 0.5} y1={y0} y2={y1 + 1} stroke="#000" strokeDasharray={`1 ${step - 1}`} />);
  }
  dashedH(x0: number, x1: number, y: number) {
    this.push((key) => <line key={key} x1={x0} x2={x1 + 1} y1={y + 0.5} y2={y + 0.5} stroke="#000" strokeDasharray="4 3" />);
  }
  polyline(pts: [number, number][], lw: number) {
    if (pts.length < 2) return;
    this.push((key) => <polyline key={key} points={pts.map((p) => `${p[0] + 0.5},${p[1] + 0.5}`).join(' ')} fill="none" stroke="#000" strokeWidth={lw} strokeLinejoin="round" strokeLinecap="round" />);
  }
  circle(cx: number, cy: number, r: number, fill = '#000', stroke?: string) {
    this.push((key) => <circle key={key} cx={cx + 0.5} cy={cy + 0.5} r={r} fill={fill} stroke={stroke} />);
  }
  pattern(d: string, level: number) {
    if (level <= 0 || !d) return;
    this.push((key) => <path key={key} d={d} fill={level >= 4 ? '#000' : `url(#dp-pat${level})`} />);
  }
  triangle(x: number, y: number, size: number, up: boolean) {
    if (size < 2) return;
    const pts = up ? `${x},${y + size} ${x + size / 2 + 0.5},${y} ${x + size + 1},${y + size}` : `${x},${y} ${x + size / 2 + 0.5},${y + size} ${x + size + 1},${y}`;
    this.push((key) => <polygon key={key} points={pts} fill="#000" />);
  }
  /** style::text — ellipsized single line, top-left anchored. Returns width. */
  text(x: number, y: number, w: number, value: string, scale = 1, bold = false, align: Align = 'left', ink = '#000'): number {
    if (w <= 0 || !value) return 0;
    const fitted = ellipsize(value, w, scale, bold);
    if (!fitted) return 0;
    const width = measureText(fitted, scale, bold);
    const left = align === 'center' ? x + Math.max(0, (w - width) / 2) : align === 'right' ? x + Math.max(0, w - width) : x;
    const base = y + ascent(scale);
    this.push((key) => (
      <text key={key} x={left} y={base} fontSize={UI_PX * scale} fill={ink}>
        {fitted}
      </text>
    ));
    if (bold)
      this.push((key) => (
        <text key={key} x={left + 1} y={base} fontSize={UI_PX * scale} fill={ink}>
          {fitted}
        </text>
      ));
    return width;
  }
  tierText(t: number, x: number, y: number, text: string, ink = '#000') {
    if (t >= DISPLAY.length) {
      this.text(x, y, tierTextWidth(t, text) + 1, text, t === DISPLAY.length ? 2 : 1, true, 'left', ink);
      return;
    }
    // Native display font: glyphs placed at the firmware's integer advances.
    const map = displayMaps[DISPLAY.length - 1 - t];
    const base = y + DISPLAY[t].asc;
    const glyphs: ReactNode[] = [];
    let pen = x;
    let i = 0;
    for (const ch of text) {
      const adv = map.get(ch);
      if (adv !== undefined) {
        glyphs.push(
          <text key={i++} x={pen} y={base} fontSize={DISPLAY[t].cap / DIGIT_EM} fontWeight={700} fill={ink}>
            {ch}
          </text>,
        );
        pen += adv;
      } else {
        const sc = fallbackScale(t);
        glyphs.push(
          <text key={i++} x={pen} y={base} fontSize={UI_PX * sc} fontWeight={700} fill={ink}>
            {ch}
          </text>,
        );
        pen += charAdvance(ch) * sc;
      }
    }
    this.push((key) => <g key={key}>{glyphs}</g>);
  }
  triangleMarker(x: number, y: number, size: number, sev: Severity, hollow = false) {
    if (size < 4) return;
    if (hollow || sev === 'info') {
      const r = Math.floor(size / 2) - 1;
      this.push((key) => <circle key={key} cx={x + size / 2} cy={y + size / 2} r={r} fill="none" stroke="#000" />);
      if (!hollow) this.circle(x + size / 2 - 0.5, y + size / 2 - 0.5, Math.max(1, Math.floor(r / 3)));
      return;
    }
    if (sev === 'warning') this.triangle(x, y, size, true);
    else this.fill(x, y, size, size);
  }
  badge(rightX: number, y: number, maxW: number, label: string, sev: Severity): number {
    if (!label || maxW <= 0) return 0;
    const pad = 8;
    const tw = measureText(label, 1, true) + 1;
    const w = tw + pad * 2;
    if (w > maxW) return 0;
    const x = rightX - w;
    const r = BADGE_H / 2;
    if (sev === 'critical') {
      this.roundRect(x, y, w, BADGE_H, r, true);
      this.text(x + pad, y + 1, tw, label, 1, true, 'left', '#fff');
    } else {
      this.roundRect(x, y, w, BADGE_H, r);
      if (sev === 'warning') this.roundRect(x + 1, y + 1, w - 2, BADGE_H - 2, r - 1);
      this.text(x + pad, y + 1, tw, label, 1, true);
    }
    return w;
  }
  emptyState(b: Box, message = 'No data') {
    if (empty(b)) return;
    const barW = Math.min(28, Math.floor(b.w / 2));
    const withText = !!message && b.h >= 3 + 6 + LH;
    const block = 3 + (withText ? 6 + LH : 0);
    const top = b.y + Math.max(0, Math.floor((b.h - block) / 2));
    this.fill(b.x + Math.floor((b.w - barW) / 2), top, barW, 3);
    if (withText) this.text(b.x, top + 9, b.w, message, 1, false, 'center');
  }
}

// ------------------------------------------------------------------ values
interface ValueFit {
  tier: number;
  unit: boolean;
  compact: boolean;
  small: boolean;
  width: number;
}
const unitScaleFor = (t: number) => (tierHeight(t) >= lineHeight(2) * 2 ? 2 : 1);

function blockWidth(t: number, number: string, unitText: string, symbol: boolean, withUnit: boolean, small: boolean): number {
  let w = tierTextWidth(t, number);
  if (withUnit && unitText) {
    if (symbol && !small) w += tierTextWidth(t, unitText);
    else {
      const us = unitScaleFor(t);
      w += (symbol ? 2 : 2 + 2 * us) + measureText(unitText, us);
    }
  }
  return w;
}

function fitValue(value: number, unit: string, prec: number, w: number, h: number, maxTier = 0, minTier = 99, prefix = ''): ValueFit | null {
  const normal = formatNumber(value, unit, prec, false);
  const compact = formatNumber(value, unit, prec, true);
  const hasCompact = compact.number !== normal.number;
  const last = Math.min(minTier, TIERS - 1);
  for (let pass = 0; pass < 2; pass++) {
    const withUnit = pass === 0;
    if (!withUnit && !normal.unit) break;
    for (let t = Math.max(0, maxTier); t <= last; t++) {
      if (tierHeight(t) > h) continue;
      for (let v = 0; v < 4; v++) {
        const useCompact = v >= 2;
        const small = (v & 1) !== 0;
        if (useCompact && !hasCompact) break;
        if (small && !(withUnit && normal.symbol)) continue;
        const txt = useCompact ? compact : normal;
        const width = blockWidth(t, prefix + txt.number, txt.unit, txt.symbol, withUnit, small);
        if (width <= w) return { tier: t, unit: withUnit, compact: useCompact, small, width };
      }
    }
  }
  return null;
}

function drawValue(p: Painter, x: number, y: number, value: number, unit: string, prec: number, fit: ValueFit, ink = '#000', prefix = '') {
  const n = formatNumber(value, unit, prec, fit.compact);
  const text = prefix + n.number;
  const nw = tierTextWidth(fit.tier, text);
  p.tierText(fit.tier, x, y, text, ink);
  if (!fit.unit || !n.unit) return;
  if (n.symbol && !fit.small) {
    p.tierText(fit.tier, x + nw, y, n.unit, ink);
    return;
  }
  const us = unitScaleFor(fit.tier);
  const ux = x + nw + (n.symbol ? 2 : 2 + 2 * us);
  const uy = y + tierAscent(fit.tier) - ascent(us);
  p.text(ux, uy, measureText(n.unit, us) + 1, n.unit, us, false, 'left', ink);
}

// ------------------------------------------------------------------ helpers
function severityFor(panel: Panel, v: number): Severity {
  let sev: Severity = 'info';
  if (!Number.isFinite(v)) return sev;
  for (const th of panel.thresholds ?? []) if (crossed(panel, th.value, v) && RANK[th.severity] >= RANK[sev]) sev = th.severity;
  return sev;
}
const crossed = (panel: Panel, th: number, v: number) => (panel.style?.invert ? v <= th : v >= th);
function severityLabel(panel: Panel, v: number, sev: Severity): string {
  let out = '';
  for (const th of panel.thresholds ?? []) if (crossed(panel, th.value, v) && th.severity === sev && th.label) out = th.label;
  return out || sev;
}
const unitFor = (panel: Panel, unit: string) => panel.style?.unit || unit;
const prec = (panel: Panel) => panel.style?.precision ?? 1;

function statusText(panel: Panel, pt: Point, unit: string): string {
  let s = pt.labels?.status || pt.labels?.state || '';
  if (!s) {
    const sev = severityFor(panel, pt.v);
    for (const th of panel.thresholds ?? []) if (crossed(panel, th.value, pt.v) && th.severity === sev && th.label) s = th.label;
  }
  if (!s) s = panel.thresholds?.length ? 'OK' : formatValueText(pt.v, unitFor(panel, unit), prec(panel));
  return [...s].slice(0, 32).join('');
}

function expandRange(min: number, max: number): [number, number] {
  if (max > min) return [min, max];
  const c = min;
  const m = Math.max(1, Math.abs(c) * 0.000001);
  return [c - m, c + m];
}
function ratioOf(v: number, min: number, max: number): number | null {
  if (!Number.isFinite(v) || !(max > min)) return null;
  return Math.max(0, Math.min(1, (v - min) / (max - min)));
}

function interpolate(s: string, b: Bundle | null): string {
  if (!b) return s;
  return s.replace(/\$\{([A-Za-z0-9_]+)\}/g, (m, name: string) => b.dashboard.variables.find((v) => v.name === name)?.default_value ?? m);
}

/** Greedy wrap (word-aware for latin, per-char for CJK). */
function wrap(text: string, w: number, scale: number, maxLines: number): string[] {
  const out: string[] = [];
  for (const para of text.split('\n')) {
    const tokens = para.match(/[⺀-鿿＀-￯]|[^\s⺀-鿿＀-￯]+|\s+/g) ?? [''];
    let line = '';
    for (const tok of tokens) {
      const next = line + tok;
      if (!line || measureText(next.trimEnd(), scale) <= w) line = next;
      else {
        out.push(line.trimEnd());
        line = tok.trimStart();
      }
      if (out.length >= maxLines) break;
    }
    if (out.length >= maxLines) break;
    out.push(line.trimEnd());
  }
  if (out.length > maxLines) out.length = maxLines;
  return out;
}

// ------------------------------------------------------------------ cards
function drawSparkline(p: Painter, panel: Panel, pts: Point[], area: Box) {
  if (area.w < 8 || area.h < 6 || pts.length < 2) return;
  const vals = pts.map((x) => x.v).filter(Number.isFinite);
  if (!vals.length) return;
  const [min, max] = expandRange(Math.min(...vals), Math.max(...vals));
  const plot = { x: area.x, y: area.y + 3, w: area.w - 4, h: area.h - 6 };
  const step = Math.max(1, Math.floor(pts.length / Math.max(1, Math.floor(plot.w / 2))));
  const out: [number, number][] = [];
  for (let i = 0; i < pts.length; i += step) {
    let r = ratioOf(pts[i].v, min, max);
    if (r === null) continue;
    if (panel.style?.invert) r = 1 - r;
    out.push([plot.x + Math.floor((i * (plot.w - 1)) / (pts.length - 1)), Math.floor(plot.y + plot.h - 1 - r * (plot.h - 1))]);
  }
  p.polyline(out, 2);
  if (out.length) p.circle(out[out.length - 1][0], out[out.length - 1][1], 3);
}

function drawDelta(p: Painter, x: number, y: number, w: number, latest: number, previous: number, unit: string, precision: number) {
  const delta = latest - previous;
  if (!Number.isFinite(delta) || w <= 0) return;
  const rounded = formatNumber(delta, unit, precision);
  const zero = delta === 0 || !/[^0.,-]/.test(rounded.number);
  let cursor = x;
  if (!zero) {
    const size = Math.max(5, Math.floor((ascent() * 3) / 5));
    p.triangle(cursor, y + ascent() - size, size, delta > 0);
    cursor += size + 4;
  }
  const remaining = x + w - cursor;
  let main = zero ? 'unchanged' : formatSigned(delta, unit, precision);
  if (!zero && measureText(main) > remaining) main = formatSigned(delta, unit, precision, true);
  const ut = formatNumber(latest, unit, precision);
  if (!zero && !ut.symbol && previous !== 0 && Number.isFinite(previous)) {
    const rel = `${main} (${formatSigned((delta / Math.abs(previous)) * 100, '%', 1)})`;
    if (measureText(rel) <= remaining) main = rel;
  }
  p.text(cursor, y, remaining, main);
}

function drawValueCard(p: Painter, panel: Panel, pts: Point[], unitRaw: string, b: Box, allowSpark = true) {
  const latest = pts[pts.length - 1];
  const unit = unitFor(panel, unitRaw);
  const gap = 6;
  const hasPrev = pts.length > 1 && Number.isFinite(latest.v) && Number.isFinite(pts[pts.length - 2].v);
  const bare = fitValue(latest.v, unit, prec(panel), b.w, b.h);
  let fit = bare;
  let footer = false;
  if (hasPrev) {
    const wf = fitValue(latest.v, unit, prec(panel), b.w, b.h - LH - gap);
    if (wf && bare && wf.tier <= bare.tier) {
      fit = wf;
      footer = true;
    }
  }
  if (!fit) {
    p.text(b.x, b.y, b.w, formatValueText(latest.v, unit, prec(panel), true), 1, true);
    return;
  }
  const valueH = tierHeight(fit.tier);
  const blockH = valueH + (footer ? gap + LH : 0);
  const spare = b.h - blockH;
  const spark = allowSpark && pts.length >= 3 && spare >= LH * 3;
  drawValue(p, b.x, b.y, latest.v, unit, prec(panel), fit);
  if (footer) drawDelta(p, b.x, b.y + valueH + gap, b.w, latest.v, pts[pts.length - 2].v, unit, prec(panel));
  if (spark) {
    const sh = Math.min(spare - gap * 2, Math.max(LH * 3, Math.floor(b.h / 3)));
    drawSparkline(p, panel, pts, { x: b.x, y: bottom(b) - sh, w: b.w, h: sh });
  }
}

function drawDeltaCard(p: Painter, panel: Panel, pts: Point[], unitRaw: string, b: Box) {
  const latest = pts[pts.length - 1];
  const unit = unitFor(panel, unitRaw);
  const previous = pts.length > 1 ? pts[pts.length - 2].v : latest.v;
  const delta = latest.v - previous;
  const gap = 6;
  const footer = b.h >= LH * 2 + gap;
  const availH = b.h - (footer ? LH + gap : 0);
  const zero = !Number.isFinite(delta) || delta === 0;
  const sign = Number.isFinite(delta) && delta > 0 ? '+' : '';
  let fit: ValueFit | null = null;
  let arrow = 0;
  for (let t = 0; t < TIERS && !fit; t++) {
    arrow = zero ? 0 : Math.max(5, Math.floor((tierAscent(t) * 3) / 5)) + 6;
    fit = fitValue(delta, unit, prec(panel), b.w - arrow, availH, t, t, sign);
  }
  let y = b.y;
  if (fit) {
    if (!zero) {
      const size = arrow - 6;
      p.triangle(b.x, y + tierAscent(fit.tier) - size, size, delta > 0);
    }
    drawValue(p, b.x + arrow, y, delta, unit, prec(panel), fit, '#000', sign);
    y += tierHeight(fit.tier) + gap;
  }
  if (footer) p.text(b.x, y, b.w, 'now ' + formatValueText(latest.v, unit, prec(panel)));
}

function drawProgressCard(p: Painter, panel: Panel, pt: Point, b: Box) {
  const unit = panel.style?.unit || '%';
  const gap = 8;
  const barH = Math.max(10, Math.min(18, Math.floor(b.h / 6)));
  const clamped = Number.isFinite(pt.v) ? Math.max(0, Math.min(100, pt.v)) : 0;
  const bar = (x: number, y: number, w: number, h: number) => {
    p.roundRect(x, y, w, h, Math.min(4, Math.floor(h / 2)));
    const inner = w - 6;
    const filled = Math.floor((inner * clamped) / 100 + 0.5);
    p.fill(x + 3, y + 3, filled, h - 6);
    for (const th of panel.thresholds ?? []) {
      if (!(th.value > 0 && th.value < 100)) continue;
      const tx = Math.floor(x + 3 + (inner * th.value) / 100);
      p.fill(tx, y - 4, 1, 3);
      p.fill(tx, y + h + 1, 1, 3);
    }
  };
  if (b.h < lineHeight(2) + gap + barH && b.w >= 120) {
    const side = fitValue(pt.v, unit, prec(panel), Math.floor(b.w / 2), b.h);
    if (side) {
      drawValue(p, b.x, b.y + Math.floor((b.h - tierHeight(side.tier)) / 2), pt.v, unit, prec(panel), side);
      const bx = b.x + side.width + 12;
      const h = Math.min(barH + 2, b.h);
      bar(bx, b.y + Math.floor((b.h - h) / 2), right(b) - bx, h);
      return;
    }
  }
  const fit = fitValue(pt.v, unit, prec(panel), b.w, b.h - barH - gap);
  let y = b.y;
  if (fit) {
    drawValue(p, b.x, y, pt.v, unit, prec(panel), fit);
    y += tierHeight(fit.tier) + gap;
  }
  if (bottom(b) - y < 6) return;
  bar(b.x, y, b.w, Math.min(barH, bottom(b) - y));
}

function drawStatusChip(p: Painter, chip: Box, status: string, detail: string, sev: Severity) {
  if (empty(chip)) return;
  const inverted = sev === 'critical';
  if (inverted) p.roundRect(chip.x, chip.y, chip.w, chip.h, 6, true);
  else {
    p.roundRect(chip.x, chip.y, chip.w, chip.h, 6);
    if (sev === 'warning') p.roundRect(chip.x + 1, chip.y + 1, chip.w - 2, chip.h - 2, 5);
  }
  const ink = inverted ? '#fff' : '#000';
  const inner = inset(chip, 8);
  if (empty(inner)) return;
  let tier = -1;
  for (let t = 1; t < TIERS; t++) {
    if (tierHeight(t) <= inner.h && tierTextWidth(t, status) <= inner.w - 4) {
      tier = t;
      break;
    }
  }
  if (tier < 0) tier = TIERS - 1;
  const sh = tierHeight(tier);
  const withDetail = !!detail && inner.h >= sh + 6 + LH;
  const block = sh + (withDetail ? 6 + LH : 0);
  let y = inner.y + Math.max(0, Math.floor((inner.h - block) / 2));
  const sw = tierTextWidth(tier, status);
  if (sw <= inner.w) p.tierText(tier, inner.x + Math.floor((inner.w - Math.min(inner.w, sw)) / 2), y, status, ink);
  else p.text(inner.x, y, inner.w, status, 1, true, 'center', ink);
  if (withDetail) {
    y += sh + 6;
    p.text(inner.x, y, inner.w, detail, 1, false, 'center', ink);
  }
}

function drawStatusCard(p: Painter, panel: Panel, pt: Point, unit: string, b: Box) {
  const sev = severityFor(panel, pt.v);
  const status = pt.labels?.status || pt.labels?.state || severityLabel(panel, pt.v, sev);
  const detail = formatValueText(pt.v, unitFor(panel, unit), prec(panel));
  const chip = { ...b };
  const maxH = lineHeight(3) + lineHeight() + 28;
  if (chip.h > maxH) {
    chip.y += Math.floor((chip.h - maxH) / 2);
    chip.h = maxH;
  }
  drawStatusChip(p, chip, [...status].slice(0, 32).join(''), detail, sev);
}

function drawLinkedCards(p: Painter, panel: Panel, bundle: Bundle | null, observed: ObservedMetrics | undefined, b: Box) {
  const count = panel.style?.variant === 'dual' ? 2 : 3;
  const cards: Panel[] = [panel];
  for (const id of panel.linked_panel_ids ?? []) {
    const linked = bundle?.dashboard.pages.flatMap((x) => x.panels).find((x) => x.id === id);
    if (!linked || linked.kind !== 'card') return p.emptyState(b, 'Invalid links');
    cards.push(linked);
  }
  if (cards.length !== count) return p.emptyState(b, 'Invalid links');
  const columns = b.w >= count * 110 ? count : 1;
  const rows = Math.ceil(count / columns);
  const gap = 16;
  const cw = Math.floor((b.w - (columns - 1) * gap) / columns);
  const ch = Math.floor((b.h - (rows - 1) * gap) / rows);
  if (cw <= 0 || ch <= 0) return;
  cards.forEach((card, i) => {
    const cell = { x: b.x + (i % columns) * (cw + gap), y: b.y + Math.floor(i / columns) * (ch + gap), w: cw, h: ch };
    if (i > 0) {
      if (columns > 1) p.dottedV(cell.x - gap / 2, cell.y, bottom(cell) - 1, 2);
      else p.dottedH(cell.x, right(cell) - 1, cell.y - gap / 2, 2);
    }
    const name = card.title ? interpolate(card.title, bundle) : card.metric ?? '';
    const s = panelSeries(bundle, card, observed);
    let badgeW = 0;
    if (s.points.length && cell.h >= LH * 3) {
      const v = s.points[s.points.length - 1].v;
      const sev = severityFor(card, v);
      if (sev !== 'info') badgeW = p.badge(right(cell), cell.y, Math.floor(cell.w / 2), severityLabel(card, v, sev), sev);
    }
    const value = { ...cell };
    if (cell.h >= LH * 2 + 4) {
      p.text(cell.x, cell.y, cell.w - badgeW - (badgeW ? 6 : 0), name, 1, true);
      value.y += LH + 6;
      value.h -= LH + 6;
    }
    if (!s.points.length) p.emptyState(value);
    else drawValueCard(p, card, s.points, s.unit, value, false);
  });
}

// ------------------------------------------------------------------ charts
function drawChart(p: Painter, panel: Panel, pts: Point[], unitRaw: string, b: Box) {
  const vals = pts.map((x) => x.v).filter(Number.isFinite);
  if (b.w < 8 || b.h < 8 || !vals.length) return p.emptyState(b);
  let min = panel.chart?.y_min ?? Math.min(...vals);
  let max = panel.chart?.y_max ?? Math.max(...vals);
  const bar = panel.chart?.kind === 'bar';
  if (bar) {
    if (panel.chart?.y_min == null) min = Math.min(0, min);
    if (panel.chart?.y_max == null) max = Math.max(0, max);
  }
  [min, max] = expandRange(min, max);
  const invert = !!panel.style?.invert;
  const legendOn = panel.style?.show_legend !== false;
  const sparkline = panel.chart?.kind === 'sparkline' || b.h < LH * 3 + 8;
  const axes = !sparkline;
  const yLabels = axes && legendOn && b.h >= LH * 3 + 8 && b.w >= 120;
  const times = axes && b.h >= LH * 5 && pts.length > 1 && pts[0].t >= 1e12 && pts[pts.length - 1].t > pts[0].t;
  const unit = unitFor(panel, unitRaw);
  const axisLabel = (v: number) => {
    const t = formatNumber(v, unit, prec(panel), true);
    return t.symbol ? joined(t) : t.number;
  };
  const plot = { ...b };
  if (times) plot.h -= LH + 4;
  if (sparkline && legendOn && b.h >= LH * 3 + 8 && b.w >= 120) {
    plot.h -= LH + 4;
    const half = Math.floor(b.w / 2);
    p.text(b.x, bottom(b) - LH, half, 'min ' + axisLabel(min));
    p.text(b.x + half, bottom(b) - LH, half, 'max ' + axisLabel(max), 1, false, 'right');
  }
  let maxLabel = '';
  let minLabel = '';
  if (yLabels) {
    maxLabel = axisLabel(max);
    minLabel = axisLabel(min);
    const gutter = Math.max(measureText(maxLabel), measureText(minLabel)) + 6;
    if (gutter < plot.w / 3) {
      plot.x += gutter;
      plot.w -= gutter;
    } else maxLabel = minLabel = '';
    plot.y += Math.floor(LH / 2);
    plot.h -= LH;
  } else if (axes) {
    plot.y += 3;
    plot.h -= 3;
  }
  const lineKind = !bar;
  if (lineKind) plot.w -= 4;
  if (plot.w < 4 || plot.h < 4) return p.emptyState(b, '');
  const yFor = (v: number) => {
    let r = ratioOf(v, min, max) ?? 0.5;
    if (invert) r = 1 - r;
    return Math.floor(plot.y + plot.h - 1 - r * (plot.h - 1));
  };
  const plotRight = right(plot) - 1 + (lineKind ? 4 : 0);
  const zeroInside = min < 0 && max > 0;
  const baseY = zeroInside ? yFor(0) : yFor(invert ? max : min);
  if (axes) {
    p.dottedH(plot.x, plotRight, yFor(invert ? min : max), 3);
    if (zeroInside) p.dottedH(plot.x, plotRight, yFor(invert ? max : min), 3);
    p.fill(plot.x, baseY, plotRight - plot.x + 1, 1);
    for (const th of panel.thresholds ?? []) if (th.value > min && th.value < max) p.dashedH(plot.x, plotRight, yFor(th.value));
  }
  if (bar) {
    const n = pts.length;
    const slot = Math.max(1, Math.floor(plot.w / n));
    const gap = slot >= 8 ? Math.max(2, Math.floor(slot / 5)) : slot >= 3 ? 1 : 0;
    const zeroY = yFor(0);
    pts.forEach((pt, i) => {
      if (!Number.isFinite(pt.v)) return;
      const x0 = plot.x + Math.floor((i * plot.w) / n);
      const x1 = plot.x + Math.floor(((i + 1) * plot.w) / n) - gap;
      const y = yFor(pt.v);
      p.fill(x0, Math.min(y, zeroY), Math.max(1, x1 - x0), Math.max(1, Math.abs(zeroY - y) + 1));
    });
  } else {
    const out: [number, number][] = [];
    pts.forEach((pt, i) => {
      if (!Number.isFinite(pt.v)) return;
      out.push([plot.x + (pts.length === 1 ? Math.floor(plot.w / 2) : Math.floor((i * (plot.w - 1)) / (pts.length - 1))), yFor(pt.v)]);
    });
    if (panel.chart?.fill && out.length > 1) {
      const d = `M${out[0][0]} ${baseY + 1}` + out.map(([x, y]) => `L${x + 1} ${y}`).join('') + `L${out[out.length - 1][0] + 1} ${baseY + 1}Z`;
      p.pattern(d, 1);
    }
    let lw = Math.max(1, Math.min(4, panel.style?.line_width ?? 1));
    if (lw < 2 && plot.h >= 32) lw = 2;
    p.polyline(out, lw);
    if (out.length === 1 || (panel.chart?.show_points && out.length * 6 <= plot.w)) for (const [x, y] of out) p.circle(x, y, 2);
    if (out.length) p.circle(out[out.length - 1][0], out[out.length - 1][1], 4, '#000', '#fff');
  }
  if (maxLabel) {
    const gw = plot.x - b.x - 6;
    const topY = yFor(invert ? min : max);
    const botY = yFor(invert ? max : min);
    p.text(b.x, topY - Math.floor(LH / 2), gw, invert ? minLabel : maxLabel, 1, false, 'right');
    p.text(b.x, botY - Math.floor(LH / 2), gw, invert ? maxLabel : minLabel, 1, false, 'right');
  }
  if (times) {
    const y = bottom(b) - LH;
    const half = Math.floor((plotRight - plot.x + 1) / 2);
    p.text(plot.x, y, half, formatTime(pts[0].t));
    p.text(plotRight + 1 - half, y, half, formatTime(pts[pts.length - 1].t), 1, false, 'right');
  }
}

// ------------------------------------------------------------------ tables
const isTimeKey = (k: string) => k === 'time' || k === 'timestamp' || k === 'timestamp_ms';
function tableValue(pt: Point, col: TableColumn, panel: Panel, unitRaw: string, compact = false): string {
  if (col.key === 'value') return formatValueText(pt.v, col.unit || panel.style?.unit || unitRaw, prec(panel), compact);
  if (col.key === 'metric') return panel.metric ?? '';
  if (col.key === 'source' || col.key === 'source_id') return panel.source_id ?? '';
  if (isTimeKey(col.key)) return formatTime(pt.t);
  return pt.labels?.[col.key] ?? '';
}

function drawTable(p: Painter, panel: Panel, pts: Point[], unitRaw: string, b: Box) {
  if (!pts.length) return p.emptyState(b);
  const rowH = LH + 8;
  const header = panel.style?.show_legend !== false && b.h >= rowH * 2 + 2;
  const headerH = header ? rowH + 2 : 0;
  const rows = Math.min(pts.length, Math.floor(Math.max(0, b.h - headerH) / rowH));
  if (rows === 0) return;
  const more = pts.length > rows && rows >= 3;
  const dataRows = more ? rows - 1 : rows;
  const columns: TableColumn[] = panel.columns?.length ? panel.columns : [{ key: 'value', title: 'Value', width: 1 }];
  const weight = columns.reduce((n, c) => n + Math.max(1, c.width ?? 1), 0);
  const xs: number[] = [];
  const ws: number[] = [];
  let x = b.x;
  columns.forEach((c, i) => {
    const w = i + 1 === columns.length ? right(b) - x : Math.floor((b.w * Math.max(1, c.width ?? 1)) / weight);
    xs.push(x);
    ws.push(w);
    x += w;
  });
  const alignOf = (c: TableColumn): Align => (c.key === 'value' || isTimeKey(c.key) ? 'right' : 'left');
  const cp = 4;
  let y = b.y;
  if (header) {
    columns.forEach((c, i) => p.text(xs[i] + cp, y + 3, ws[i] - cp * 2, c.title || c.key, 1, true, alignOf(c)));
    p.fill(b.x, y + rowH, b.w, 2);
    y += headerH;
  }
  for (let r = 0; r < dataRows; r++) {
    const pt = pts[pts.length - 1 - r];
    if (r % 2 === 1) p.fill(b.x, y, b.w, rowH, ZEBRA);
    const sev = severityFor(panel, pt.v);
    columns.forEach((c, i) => {
      const cw = ws[i] - cp * 2;
      let cell = tableValue(pt, c, panel, unitRaw);
      if (c.key === 'value' && measureText(cell) > cw) cell = tableValue(pt, c, panel, unitRaw, true);
      p.text(xs[i] + cp, y + 4, cw, cell, 1, c.key === 'value' && sev !== 'info', alignOf(c));
    });
    if (sev !== 'info') p.triangleMarker(b.x, y + Math.floor((rowH - 6) / 2), 6, sev);
    y += rowH;
  }
  if (more) p.text(b.x, y + 4, b.w, `+${pts.length - dataRows} more`, 1, false, 'right');
}

function drawStatusGrid(p: Painter, panel: Panel, pts: Point[], unitRaw: string, b: Box) {
  if (!pts.length) return p.emptyState(b);
  const gap = 5;
  let items = pts.slice(-32);
  const columns = Math.max(1, Math.min(Math.floor((b.w + gap) / 110), Math.min(items.length, 6)));
  const minCellH = LH + 8;
  const maxRows = Math.max(1, Math.floor((b.h + gap) / (minCellH + gap)));
  if (items.length > maxRows * columns) {
    items = items.map((pt, i) => ({ pt, i })).sort((a, c) => RANK[severityFor(panel, c.pt.v)] - RANK[severityFor(panel, a.pt.v)] || a.i - c.i).map((x) => x.pt).slice(0, maxRows * columns);
  }
  const rows = Math.ceil(items.length / columns);
  const cw = Math.floor((b.w - (columns - 1) * gap) / columns);
  const idealH = LH + lineHeight(2) + 18;
  const ch = Math.min(idealH, Math.floor((b.h - (rows - 1) * gap) / rows));
  const innerW = cw - 12;
  const twoLines = ch - 12 >= LH * 2 + 4;
  let scale = twoLines && ch - 12 - LH - 4 >= lineHeight(2) ? 2 : 1;
  for (const pt of items) if (measureText(statusText(panel, pt, unitRaw), scale) > innerW) scale = 1;
  items.forEach((pt, i) => {
    const cell = { x: b.x + (i % columns) * (cw + gap), y: b.y + Math.floor(i / columns) * (ch + gap), w: cw, h: ch };
    const sev = severityFor(panel, pt.v);
    const inv = sev === 'critical';
    if (inv) p.roundRect(cell.x, cell.y, cell.w, cell.h, 5, true);
    else {
      p.roundRect(cell.x, cell.y, cell.w, cell.h, 5);
      if (sev === 'warning') p.roundRect(cell.x + 1, cell.y + 1, cell.w - 2, cell.h - 2, 4);
    }
    const ink = inv ? '#fff' : '#000';
    const name = pt.labels?.name || Object.values(pt.labels ?? {})[0] || panel.metric || '';
    const status = statusText(panel, pt, unitRaw);
    const inner = inset(cell, 6);
    if (empty(inner)) return;
    if (twoLines) {
      p.text(inner.x, inner.y, inner.w, name, 1, false, 'left', ink);
      p.text(inner.x, bottom(inner) - lineHeight(scale), inner.w, status, scale, true, 'left', ink);
    } else {
      const sw = Math.min(Math.floor(inner.w / 2), measureText(status, 1, true) + 1);
      const y = inner.y + Math.floor((inner.h - LH) / 2);
      p.text(inner.x, y, inner.w - sw - 4, name, 1, false, 'left', ink);
      p.text(right(inner) - sw, y, sw, status, 1, true, 'right', ink);
    }
  });
}

function drawHeatmap(p: Painter, panel: Panel, pts: Point[], unitRaw: string, b: Box) {
  const vals = pts.map((x) => x.v).filter(Number.isFinite);
  if (!vals.length) return p.emptyState(b);
  const count = Math.min(pts.length, 64);
  const recent = pts.slice(-count);
  const rv = recent.map((x) => x.v).filter(Number.isFinite);
  const lo = Math.min(...rv);
  const hi = Math.max(...rv);
  const [min, max] = expandRange(lo, hi);
  const legend = b.h >= LH * 4 && b.w >= 160;
  const grid = { ...b, h: legend ? b.h - LH - 8 : b.h };
  let columns = 1;
  while (columns < count) {
    const rows = Math.ceil(count / columns);
    if (grid.w * rows <= grid.h * columns) break;
    columns++;
  }
  const rows = Math.ceil(count / columns);
  const gap = 3;
  const cell = Math.min(Math.floor((grid.w + gap) / columns), Math.floor((grid.h + gap) / rows));
  if (cell - gap < 3) return p.emptyState(b, '');
  const width = cell * columns - gap;
  const x0 = grid.x + Math.floor((grid.w - width) / 2);
  recent.forEach((pt, i) => {
    const box = { x: x0 + (i % columns) * cell, y: grid.y + Math.floor(i / columns) * cell, w: cell - gap, h: cell - gap };
    let r = ratioOf(pt.v, min, max);
    if (r === null) return p.dottedH(box.x, right(box) - 1, box.y + Math.floor(box.h / 2), 2);
    if (panel.style?.invert) r = 1 - r;
    p.rect(box.x, box.y, box.w, box.h);
    const ib = inset(box, 1);
    p.pattern(`M${ib.x} ${ib.y}h${ib.w}v${ib.h}h${-ib.w}z`, Math.round(r * 4));
  });
  if (legend) {
    const y = grid.y + rows * cell - gap + 8;
    const unit = unitFor(panel, unitRaw);
    let x = x0;
    x += p.text(x, y, 120, formatValueText(lo, unit, prec(panel), true)) + 6;
    const sw = LH - 4;
    for (let level = 0; level <= 4; level++) {
      p.rect(x, y + 2, sw, sw);
      p.pattern(`M${x + 1} ${y + 3}h${sw - 2}v${sw - 2}h${-(sw - 2)}z`, level);
      x += sw + 2;
    }
    p.text(x + 4, y, 120, formatValueText(hi, unit, prec(panel), true));
  }
}

function drawTimeline(p: Painter, panel: Panel, pts: Point[], b: Box) {
  if (!pts.length) return p.emptyState(b);
  const items = pts.slice(-32);
  const marker = Math.max(6, Math.min(14, Math.floor(b.h / 4)));
  let warnings = 0;
  let criticals = 0;
  for (const pt of items) {
    const s = severityFor(panel, pt.v);
    if (s === 'warning') warnings++;
    if (s === 'critical') criticals++;
  }
  const summary = b.h >= LH * 2 + marker * 2 + 12;
  const times = b.h >= LH + marker * 2 + 8 && items[0].t >= 1e12;
  let top = b.y;
  if (summary) {
    let text = criticals ? `${criticals} critical` : '';
    if (warnings) text += (text ? ', ' : '') + `${warnings} warning`;
    p.text(b.x, top, b.w, text || 'All clear');
    top += LH + 8;
  }
  const y = summary ? top + Math.floor(marker / 2) + 2 : b.y + Math.floor((b.h - (times ? LH + 4 : 0)) / 2);
  const left = b.x + Math.floor(marker / 2);
  const span = b.w - marker - 1;
  p.fill(b.x, y, b.w, 1);
  items.forEach((pt, i) => {
    const x = left + (items.length === 1 ? Math.floor(span / 2) : Math.floor((i * span) / (items.length - 1)));
    const sev = severityFor(panel, pt.v);
    if (sev === 'info') p.fill(x, y - 3, 1, 7);
    else {
      p.fill(x - Math.floor(marker / 2) - 1, y - Math.floor(marker / 2) - 1, marker + 2, marker + 2, '#fff');
      p.triangleMarker(x - Math.floor(marker / 2), y - Math.floor(marker / 2), marker, sev);
    }
  });
  if (times) {
    const ty = y + Math.floor(marker / 2) + 4;
    const half = Math.floor(b.w / 2);
    p.text(b.x, ty, half, formatTime(items[0].t));
    p.text(b.x + half, ty, half, formatTime(items[items.length - 1].t), 1, false, 'right');
  }
}

// ------------------------------------------------------------------ alerts / text / buttons
interface EventLike {
  id?: string;
  rule_id?: string;
  source_id?: string;
  timestamp_ms?: number;
  severity?: Severity;
  state?: string;
  title?: string;
  message?: string;
  linked_panel_id?: string;
}
function matchingEvents(b: Bundle | null, panel: Panel): EventLike[] {
  const events = ((b?.dashboard.events ?? []) as EventLike[]).filter((e) => {
    if (panel.source_id && e.source_id !== panel.source_id) return false;
    if (panel.linked_panel_ids?.length && e.linked_panel_id !== panel.id && !panel.linked_panel_ids.includes(e.linked_panel_id ?? '')) return false;
    if (panel.metric && !b?.dashboard.alert_rules.some((r) => r.id === e.rule_id && r.metric === panel.metric)) return false;
    return true;
  });
  const active = (e: EventLike) => (e.state ?? 'active') === 'active';
  return events.sort((l, r) => {
    if (active(l) !== active(r)) return active(l) ? -1 : 1;
    if (active(l) && l.severity !== r.severity) return RANK[r.severity ?? 'info'] - RANK[l.severity ?? 'info'];
    return (r.timestamp_ms ?? 0) - (l.timestamp_ms ?? 0);
  });
}

function drawAlerts(p: Painter, panel: Panel, bundle: Bundle | null, b: Box) {
  const events = matchingEvents(bundle, panel);
  if (!events.length) {
    if (!panel.text?.content) return p.emptyState(b, 'No active alerts');
    return drawStaticText(p, panel, bundle, b);
  }
  const single = LH + 10;
  const twin = LH * 2 + 12;
  const twoLines = Math.floor(b.h / twin) >= Math.min(events.length, 4);
  const rowH = twoLines ? twin : single;
  let rows = Math.min(events.length, Math.floor(Math.max(0, b.h) / rowH));
  if (!rows) return;
  const more = events.length > rows && rows >= 2 && b.h - rows * rowH < LH + 4;
  if (more) rows--;
  const marker = Math.max(6, Math.floor(LH / 2));
  const tx = b.x + marker + 8;
  for (let i = 0; i < rows; i++) {
    const e = events[i];
    const y = b.y + i * rowH;
    const active = (e.state ?? 'active') === 'active';
    const ty = y + 4;
    p.triangleMarker(b.x, ty + Math.floor((LH - marker) / 2), marker, e.severity ?? 'info', !active);
    const rightText = active ? formatTime(e.timestamp_ms ?? 0) : 'cleared';
    const rw = measureText(rightText);
    const showRight = rw < b.w / 3;
    p.text(tx, ty, right(b) - tx - (showRight ? rw + 8 : 0), e.title || e.id || '', 1, active);
    if (showRight) p.text(right(b) - rw, ty, rw, rightText, 1, false, 'right');
    if (twoLines && e.message) p.text(tx, ty + LH + 2, right(b) - tx, e.message);
    if (i + 1 < rows || more) p.dottedH(b.x, right(b) - 1, y + rowH - 2, 2);
  }
  if (more) p.text(tx, b.y + rows * rowH + 4, right(b) - tx, `+${events.length - rows} more`);
}

/** panels.cpp drawStaticText (cards without data, alert fallback text). */
function drawStaticText(p: Painter, panel: Panel, bundle: Bundle | null, b: Box, emptyMessage = 'No data') {
  const content = interpolate(panel.text?.content ?? '', bundle);
  if (!content) return p.emptyState(b, emptyMessage);
  const lh = LH + 4;
  const room = Math.min(Math.max(panel.text?.max_lines ?? 1, 1), Math.floor(b.h / lh));
  wrap(content, b.w, 1, room).forEach((l, i) => p.text(b.x, b.y + i * lh, b.w, l));
}

function drawText(p: Painter, panel: Panel, bundle: Bundle | null, b: Box) {
  const content = interpolate(panel.text?.content ?? '', bundle);
  if (!content.trim()) return p.emptyState(b, 'No text');
  const variant = panel.style?.variant ?? 'default';
  const multiline = ['multiline', 'description', 'log_tail', 'markdown'].includes(variant) || (variant === 'default' && !!panel.text?.multiline);
  let maxLines = panel.text?.max_lines ?? 1;
  if ((multiline || variant === 'key_value') && !panel.text?.multiline && maxLines === 1) maxLines = 8;
  maxLines = Math.max(1, maxLines);
  if (variant === 'key_value') {
    const rows = content.split('\n').filter((r) => r.trim()).slice(0, maxLines);
    const bodyRow = LH * 2 + 12;
    const scale = b.w >= 280 && rows.length * bodyRow <= b.h ? 2 : 1;
    const rowH = scale === 2 ? bodyRow : LH + 10;
    const pairs = rows.slice(0, Math.floor(b.h / rowH)).map((r) => {
      const i = r.search(/[:=]/);
      return i < 0 ? ['', r.trim()] : [r.slice(0, i).trim(), r.slice(i + 1).trim()];
    });
    const keyW = Math.max(0, ...pairs.map(([k]) => measureText(k, scale)));
    const labelW = Math.min(keyW + 16, Math.floor((b.w * 2) / 5));
    pairs.forEach(([k, v], i) => {
      const y = b.y + i * rowH;
      const ty = y + Math.floor((rowH - LH * scale) / 2) - 1;
      p.text(b.x, ty, labelW - 8, k, scale);
      p.text(b.x + labelW, ty, b.w - labelW, v, scale, true);
      if (i + 1 < pairs.length) p.dottedH(b.x, right(b) - 1, y + rowH - 1, 2);
    });
    return;
  }
  if (variant === 'log_tail') {
    const rowH = LH + 5;
    const lines = content.split('\n').filter((r) => r.trim());
    const shown = lines.slice(-Math.min(maxLines, Math.floor(b.h / rowH)));
    shown.forEach((row, i) => {
      const y = b.y + i * rowH;
      const alert = /WARN|ERROR|ERR |CRIT|FATAL|FAIL/.test(row);
      if (alert) p.fill(b.x, y + 1, 3, LH);
      p.text(b.x + 8, y, b.w - 8, row, 1, alert);
    });
    return;
  }
  if (multiline) {
    const bodyLine = LH * 2 + 6;
    if (b.w >= 240) {
      const room = Math.min(maxLines, Math.floor(b.h / bodyLine));
      const lines = wrap(content, b.w, 2, room + 1);
      if (room > 0 && lines.length <= room) {
        lines.forEach((l, i) => p.text(b.x, b.y + i * bodyLine, b.w, l, 2));
        return;
      }
    }
    const lh = LH + 5;
    const room = Math.min(maxLines, Math.floor(b.h / lh));
    wrap(content, b.w, 1, room).forEach((l, i) => p.text(b.x, b.y + i * lh, b.w, l));
    return;
  }
  for (const scale of [3, 2, 1]) {
    if (LH * scale > b.h) continue;
    if (scale > 1 && measureText(content, scale) > b.w) continue;
    p.text(b.x, b.y, b.w, content.split('\n')[0], scale, scale > 1);
    return;
  }
}

function drawButtons(p: Painter, panel: Panel, bundle: Bundle | null, w: number, h: number) {
  const actions = panel.actions ?? [];
  const content = { x: 6, y: 6, w: w - 12, h: h - 12 }; // LayoutConfig::panel_inset
  const gap = 4;
  const columns = actions.length === 1 ? 1 : actions.length === 3 && content.w >= 264 ? 3 : 2;
  const rows = Math.ceil(actions.length / columns);
  const bw = Math.floor((content.w - (columns - 1) * gap) / columns);
  const bh = Math.floor((content.h - (rows - 1) * gap) / rows);
  if (bw <= 0 || bh <= 0) return;
  actions.forEach((a, i) => {
    const b = { x: content.x + (i % columns) * (bw + gap), y: content.y + Math.floor(i / columns) * (bh + gap), w: bw, h: bh };
    const critical = a.kind === 'ack_alert';
    const r = Math.min(8, Math.floor(b.h / 4));
    if (critical) p.roundRect(b.x, b.y, b.w, b.h, r, true);
    else {
      if (a.kind === 'set_variable') p.roundRect(b.x, b.y, b.w, b.h, r, true, ZEBRA);
      p.roundRect(b.x, b.y, b.w, b.h, r);
      p.roundRect(b.x + 1, b.y + 1, b.w - 2, b.h - 2, Math.max(0, r - 1));
    }
    const label = interpolate(a.label || a.kind, bundle);
    const lb = inset(b, 6);
    if (empty(lb)) return;
    const scale = lineHeight(2) > lb.h || measureText(label, 2, true) > lb.w ? 1 : 2;
    p.text(lb.x, lb.y + Math.max(0, Math.floor((lb.h - lineHeight(scale)) / 2)), lb.w, label, scale, true, 'center', critical ? '#fff' : '#000');
  });
}

function drawImage(p: Painter, panel: Panel, b: Box) {
  p.pattern(`M${b.x} ${b.y}h${b.w}v${b.h}h${-b.w}z`, 1);
  p.text(b.x, b.y + Math.floor((b.h - LH) / 2), b.w, panel.image?.alt_text || panel.image?.asset_id || 'image', 1, false, 'center');
}

// ------------------------------------------------------------------ header accessory
function headerAccessory(panel: Panel, bundle: Bundle | null, s: { points: Point[]; unit: string } | null): { text: string; severity: Severity; badge: boolean } | null {
  const pts = s?.points ?? [];
  switch (panel.kind) {
    case 'card': {
      const v = panel.style?.variant ?? 'default';
      if (v === 'status' || v === 'dual' || v === 'triple' || !pts.length) return null;
      const val = pts[pts.length - 1].v;
      const sev = severityFor(panel, val);
      return sev === 'info' ? null : { text: severityLabel(panel, val, sev), severity: sev, badge: true };
    }
    case 'chart': {
      if (!pts.length || !Number.isFinite(pts[pts.length - 1].v)) return null;
      const val = pts[pts.length - 1].v;
      const sev = severityFor(panel, val);
      return { text: formatValueText(val, unitFor(panel, s!.unit), prec(panel), true), severity: sev, badge: sev !== 'info' };
    }
    case 'alert': {
      let active = 0;
      let worst: Severity = 'info';
      for (const e of matchingEvents(bundle, panel)) {
        if ((e.state ?? 'active') !== 'active') continue;
        active++;
        if (RANK[e.severity ?? 'info'] > RANK[worst]) worst = e.severity ?? 'info';
      }
      return active ? { text: `${active} active`, severity: worst, badge: true } : null;
    }
    case 'table': {
      if (panel.style?.variant !== 'status_grid') return null;
      let issues = 0;
      let worst: Severity = 'info';
      for (const pt of pts.slice(-32)) {
        const sev = severityFor(panel, pt.v);
        if (sev === 'info') continue;
        issues++;
        if (RANK[sev] > RANK[worst]) worst = sev;
      }
      if (!issues) return pts.length ? { text: 'All OK', severity: 'info', badge: false } : null;
      return { text: `${issues} ${issues === 1 ? 'issue' : 'issues'}`, severity: worst, badge: true };
    }
  }
  return null;
}

// ------------------------------------------------------------------ main
function PatternDefs() {
  // 2×2 Bayer (thresholds 0,2 / 3,1): levels 1..3 = 25/50/75 % ink.
  const cells: [number, number, number][] = [
    [0, 0, 0],
    [1, 0, 2],
    [0, 1, 3],
    [1, 1, 1],
  ];
  return (
    <defs>
      {[1, 2, 3].map((level) => (
        <pattern key={level} id={`dp-pat${level}`} width="2" height="2" patternUnits="userSpaceOnUse">
          {cells.filter(([, , t]) => t < level).map(([x, y]) => (
            <rect key={`${x}${y}`} x={x} y={y} width="1" height="1" fill="#000" />
          ))}
        </pattern>
      ))}
    </defs>
  );
}

export const DevicePanel = memo(function DevicePanel({
  panel,
  bundle,
  observed,
  w,
  h,
  showDemoTag = true,
  demoTitle = '示例数据',
}: {
  panel: Panel;
  bundle: Bundle | null;
  observed?: ObservedMetrics;
  w: number;
  h: number;
  showDemoTag?: boolean;
  demoTitle?: string;
}) {
  const p = new Painter();
  const needsData = panel.kind === 'card' || panel.kind === 'chart' || panel.kind === 'table';
  const s = needsData ? panelSeries(bundle, panel, observed) : null;
  if (panel.kind === 'button') drawButtons(p, panel, bundle, w, h);
  else {
    p.roundRect(0, 0, w, h, RADIUS);
    let body = inset({ x: 0, y: 0, w, h }, PAD);
    const variant = panel.style?.variant ?? 'default';
    const linked = panel.kind === 'card' && (variant === 'dual' || variant === 'triple');
    if (panel.title && !linked && !empty(body) && body.h >= LH * 2 + TITLE_GAP) {
      const title = interpolate(panel.title, bundle);
      const acc = headerAccessory(panel, bundle, s);
      const rowH = Math.max(LH, BADGE_H);
      const titleY = body.y + Math.floor((rowH - LH) / 2);
      let used = 0;
      if (acc?.text) {
        const maxW = Math.floor(body.w / 2);
        if (acc.badge) used = p.badge(right(body), body.y, maxW, acc.text, acc.severity);
        else if (measureText(acc.text, 1, true) + 1 <= maxW) used = p.text(body.x, titleY, body.w, acc.text, 1, true, 'right') + 1;
      }
      p.text(body.x, titleY, body.w - (used > 0 ? used + 8 : 0), title, 1, true);
      body = { x: body.x, y: body.y + rowH + TITLE_GAP, w: body.w, h: body.h - rowH - TITLE_GAP };
    }
    if (!empty(body)) {
      const pts = s?.points ?? [];
      const unit = s?.unit ?? '';
      switch (panel.kind) {
        case 'card':
          if (linked) drawLinkedCards(p, panel, bundle, observed, body);
          else if (!pts.length) drawStaticText(p, panel, bundle, body);
          else if (variant === 'progress') drawProgressCard(p, panel, pts[pts.length - 1], body);
          else if (variant === 'status') drawStatusCard(p, panel, pts[pts.length - 1], unit, body);
          else if (variant === 'delta') drawDeltaCard(p, panel, pts, unit, body);
          else drawValueCard(p, panel, pts, unit, body);
          break;
        case 'chart':
          drawChart(p, panel, pts.slice(-128), unit, body);
          break;
        case 'table':
          if (variant === 'status_grid') drawStatusGrid(p, panel, pts, unit, body);
          else if (variant === 'heatmap') drawHeatmap(p, panel, pts, unit, body);
          else if (variant === 'timeline') drawTimeline(p, panel, pts, body);
          else drawTable(p, panel, pts, unit, body);
          break;
        case 'alert':
          drawAlerts(p, panel, bundle, body);
          break;
        case 'image':
          drawImage(p, panel, body);
          break;
        default:
          drawText(p, panel, bundle, body);
      }
    }
  }
  return (
    <div className="dp">
      <svg width={w} height={h} viewBox={`0 0 ${w} ${h}`} fontFamily={FAMILY} textRendering="geometricPrecision" aria-hidden>
        <PatternDefs />
        {p.nodes}
      </svg>
      {showDemoTag && s?.demo && <span className="demo-tag" title={demoTitle} />}
    </div>
  );
});
