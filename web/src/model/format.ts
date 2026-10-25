// Port of firmware/components/dashboard_ui/src/panel_style.cpp number / time
// formatting so the editor shows exactly the strings the device draws.
// Keep in sync; covered by test/format.test.ts.

export interface NumberText {
  number: string; // "1,234.5", "1.9M", "-3.2"
  unit: string; // display unit, possibly rescaled ("GB")
  symbol: boolean; // unit drawn attached at full size ("%", "°C")
}

const isSymbolUnit = (u: string) => u === '%' || u === '‰' || u.startsWith('°');
const isByteUnit = (u: string) => u === 'B' || u === 'byte' || u === 'bytes';

/** text.cpp sanitizeText(single line, max 16 code points) approximation. */
function sanitizeUnit(unit: string): string {
  return [...unit].filter((c) => c.charCodeAt(0) >= 0x20 && c.charCodeAt(0) !== 0x7f).slice(0, 16).join('');
}

export function displayUnit(unit: string): string {
  if (unit === 'percent' || unit === 'pct') return '%';
  if (unit === 'state' || unit === 'status' || unit === 'bool') return '';
  if (unit === 'C' || unit === 'degC' || unit === 'celsius') return '°C';
  if (unit === 'F' || unit === 'degF' || unit === 'fahrenheit') return '°F';
  return sanitizeUnit(unit);
}

export function groupThousands(n: string): string {
  const start = n[0] === '-' || n[0] === '+' ? 1 : 0;
  let end = n.indexOf('.');
  if (end < 0) end = n.length;
  if (end - start <= 3) return n;
  let out = n.slice(0, start);
  const digits = end - start;
  for (let i = 0; i < digits; i++) {
    if (i > 0 && (digits - i) % 3 === 0) out += ',';
    out += n[start + i];
  }
  return out + n.slice(end);
}

function trimZeros(n: string): string {
  if (!n.includes('.')) return n;
  return n.replace(/0+$/, '').replace(/\.$/, '');
}

/** printf("%.*f") with "-0" / "-0.0" normalised to "0" / "0.0". */
export function printFixed(v: number, decimals: number): string {
  let out = v.toFixed(decimals);
  // toFixed rounds exact ties away from zero; glibc printf rounds them to even.
  const scale = Math.pow(10, decimals);
  const scaled = v * scale;
  if (Math.abs(scaled) < 1e15 && Math.abs(scaled % 1) === 0.5 && (Math.trunc(scaled) + Math.sign(scaled) * 0.5) / scale === v) {
    const even = 2 * Math.round(scaled / 2);
    out = (even / scale).toFixed(decimals);
  }
  if (out.length > 1 && out[0] === '-' && /^[0.]+$/.test(out.slice(1))) out = out.slice(1);
  return out;
}

function printExp(v: number): string {
  // printf("%.1e") → "1.2e+18"
  const [m, e] = v.toExponential(1).split('e');
  const n = Number(e);
  return `${m}e${n < 0 ? '-' : '+'}${String(Math.abs(n)).padStart(2, '0')}`;
}

export function compactNumber(value: number): string {
  const suffix = ['', 'K', 'M', 'B', 'T'];
  let scaled = value;
  let i = 0;
  while (Math.abs(scaled) >= 999.5 && i + 1 < suffix.length) {
    scaled /= 1000;
    i++;
  }
  if (Math.abs(scaled) >= 999.5) return printExp(value);
  const mag = Math.abs(scaled);
  const decimals = i === 0 ? 0 : mag < 10 ? 2 : mag < 100 ? 1 : 0;
  return trimZeros(printFixed(scaled, decimals)) + suffix[i];
}

export function formatNumber(value: number, unit: string, precision: number, compact = false): NumberText {
  const out: NumberText = { number: '', unit: displayUnit(unit), symbol: false };
  if (!Number.isFinite(value)) {
    out.number = '--';
    return out;
  }
  if (isByteUnit(out.unit)) {
    const bytes = ['B', 'kB', 'MB', 'GB', 'TB', 'PB'];
    let i = 0;
    while (Math.abs(value) >= 1000 && i + 1 < bytes.length) {
      value /= 1000;
      i++;
    }
    out.unit = bytes[i];
    if (i > 0) precision = Math.abs(value) < 10 ? 1 : 0;
  }
  out.symbol = isSymbolUnit(out.unit);
  const mag = Math.abs(value);
  if ((compact && mag >= 10000) || mag >= 1e15) {
    out.number = compactNumber(value);
    return out;
  }
  let decimals = Math.min(Math.max(0, Math.floor(precision)), 6);
  if (mag >= 1000) decimals = 0;
  else if (mag >= 100) decimals = Math.min(decimals, 1);
  if (Math.abs(value - Math.round(value)) < 1e-9 * Math.max(1, mag)) decimals = 0;
  if (mag > 0 && mag < 0.5 * Math.pow(10, -decimals)) {
    decimals = Math.min(8, -Math.floor(Math.log10(mag)) + 1);
  }
  out.number = groupThousands(printFixed(value, decimals));
  return out;
}

export function joined(t: NumberText): string {
  if (!t.unit) return t.number;
  return t.symbol ? t.number + t.unit : `${t.number} ${t.unit}`;
}

export function formatValueText(value: number, unit: string, precision: number, compact = false): string {
  return joined(formatNumber(value, unit, precision, compact));
}

/** "+1.2", "-3" (no sign for zero). */
export function formatSigned(value: number, unit: string, precision: number, compact = false): string {
  const t = formatNumber(value, unit, precision, compact);
  if (Number.isFinite(value) && value > 0 && /[^0.,]/.test(t.number)) t.number = '+' + t.number;
  return joined(t);
}

/** "HH:MM" (local) for epoch ms; small values are relative counters. */
export function formatTime(ms: number): string {
  if (ms < 1_000_000_000_000) return String(Math.trunc(ms));
  const d = new Date(ms);
  return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
}
