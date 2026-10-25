// In-browser mock of the device REST API (VITE_MOCK=1, dev only).
// Installed by main.tsx through a dynamic import that is dead-code-eliminated
// from production builds.
import { parseBundle, clone } from '../model/bundle';
import { columnsOf, isVisible, layoutIssues, panelPixels, screenCount } from '../model/grid';
import { checkExpression } from '../model/expression';
import { parseJsonPath, type PathToken } from '../model/jsonpath';
import type { Bundle, SourcePlan } from '../model/types';

const presetFiles = import.meta.glob('../../../firmware/presets/{panels,layouts,examples}/*.json', { eager: true, import: 'default' }) as Record<
  string,
  unknown
>;

const PASSWORD = 'admin';
const TOKEN = 'mock-token-0123456789';

const SETTINGS_SCHEMA = {
  schema_version: 1,
  fields: {
    language: { type: 'string', enum: ['zh', 'en'], default: 'zh', group: 'time_language' },
    power_mode: { type: 'string', enum: ['auto', 'mains', 'battery'], default: 'auto', group: 'power' },
    data_refresh_s: { type: 'int', range: [0, 86400], minimum_nonzero: 30, default: 0, group: 'refresh' },
    battery_refresh_min: { type: 'int', enum: [5, 10, 15, 30, 60], default: 15, group: 'battery' },
    battery_max_stale_min: { type: 'int', enum: [15, 30, 60, 120, 240, 480], default: 60, group: 'battery' },
    battery_interactive_timeout_s: { type: 'int', range: [20, 600], default: 60, group: 'battery' },
    sleep_start_min: { type: 'int', range: [-1, 1439], default: -1, group: 'battery' },
    sleep_end_min: { type: 'int', range: [-1, 1439], default: -1, group: 'battery' },
    timezone_offset_min: { type: 'int', range: [-840, 840], step: 15, default: 480, group: 'time_language' },
    frontlight_enabled: { type: 'bool', default: false, capability: 'frontlight', group: 'display' },
    frontlight_brightness: { type: 'int', range: [1, 100], default: 60, capability: 'frontlight', group: 'display' },
    frontlight_warmth: { type: 'int', range: [0, 100], default: 50, capability: 'frontlight', group: 'display' },
    frontlight_hardware: { type: 'string', enum: ['auto', 'present', 'absent'], default: 'auto', group: 'display' },
    epd_full_refresh_s: { type: 'int', range: [0, 604800], minimum_nonzero: 3600, default: 86400, group: 'refresh' },
    idle_after_min: { type: 'int', range: [0, 1440], default: 10, group: 'display_refresh' },
    idle_refresh_min: { type: 'int', enum: [2, 5, 10, 15, 30, 60], default: 5, group: 'display_refresh' },
    idle_critical_immediate: { type: 'bool', default: true, group: 'display_refresh' },
    diagnostics_enabled: { type: 'bool', default: true, group: 'diagnostics' },
    ble_observer_enabled: { type: 'bool', default: false, capability: 'ble_observer', group: 'ble' },
    ble_observer_window_s: { type: 'int', range: [2, 60], default: 10, group: 'ble' },
    ble_observer_interval_s: { type: 'int', range: [30, 86400], default: 300, group: 'ble' },
    ble_observer_dedup_ms: { type: 'int', range: [0, 60000], default: 5000, group: 'ble' },
    ble_observer_rules: { type: 'array', default: [], max_items: 4, group: 'ble' },
  },
};

interface MockState {
  bundle: Bundle | null;
  revision: number;
  settings: Record<string, unknown>;
  credentials: string[];
  alerts: Record<string, unknown>[];
  logs: { ts: number; unsynced: boolean; level: string; seq: number; msg: string }[];
}

function presetsByDir(dir: string): [string, unknown][] {
  return Object.entries(presetFiles).filter(([k]) => k.includes(`/presets/${dir}/`));
}

function initialState(): MockState {
  const example = presetsByDir('examples').find(([k]) => k.endsWith('server-metrics.json'))?.[1];
  const settings: Record<string, unknown> = {};
  for (const [k, f] of Object.entries(SETTINGS_SCHEMA.fields)) settings[k] = clone((f as { default: unknown }).default);
  const now = Math.floor(Date.now() / 1000);
  return {
    bundle: example ? { ...parseBundle(example), revision: 3 } : null,
    revision: 3,
    settings,
    credentials: ['demo_token'],
    alerts: [
      { id: 'a1', type: 'disk.full', severity: 'critical', state: 'active', title: '磁盘空间不足', message: '/var 98%', occurrences: 3, last_seen: now - 60 },
      { id: 'a2', type: 'backup.late', severity: 'warning', state: 'acked', title: '备份延迟', message: '', occurrences: 1, last_seen: now - 3600 },
      { id: 'a3', type: 'deploy', severity: 'info', state: 'cleared', title: '部署完成', occurrences: 1, last_seen: now - 7200 },
    ],
    logs: Array.from({ length: 30 }, (_, i) => ({ ts: now - (30 - i) * 37, unsynced: false, level: i % 7 === 0 ? 'warn' : 'info', seq: i + 1, msg: `mock log line ${i + 1}` })),
  };
}

const state = initialState();
const capabilities = () => ({
  frontlight: state.settings.frontlight_hardware === 'present',
  battery: true,
  bluetooth: true,
  ble_provisioning: true,
  ble_observer: true,
  display: true,
  sd: true,
  rtc: false,
});

// ------------------------------------------------------------ mock upstream
function mockBody(url: string): unknown {
  const t = Date.now();
  if (/weather/.test(url)) return { city: 'Shanghai', temperature: 21.5, humidity: 63, wind_speed: 12, updated: t };
  if (/token/.test(url))
    return { metrics: [{ metric: 'token.used', value: 812345, unit: '', timestamp_ms: t }, { metric: 'token.limit', value: 1000000, unit: '', timestamp_ms: t }], reset_at: t + 86400000 };
  if (/security|events/.test(url))
    return { events: [{ id: 'e1', severity: 'warning', title: 'SSH login burst', message: '12 attempts', timestamp_ms: t, state: 'active' }], metrics: [] };
  return {
    host: 'srv-demo',
    api_key: '********',
    cpu: { usage: 37.2, load: [0.42, 0.51, 0.6] },
    memory: { used_pct: 61.8, total_mb: 16384 },
    disks: [
      { mount: '/', used_pct: 71.3 },
      { mount: '/data', used_pct: 45.0 },
    ],
    metrics: [
      { metric: 'cpu.usage', value: 37.2, unit: 'percent', timestamp_ms: t, labels: { host: 'srv-demo' } },
      { metric: 'memory.used', value: 61.8, unit: 'percent', timestamp_ms: t, labels: { host: 'srv-demo' } },
    ],
  };
}

function select(root: unknown, tokens: PathToken[]): unknown[] {
  let cur: unknown[] = [root];
  for (const t of tokens) {
    const next: unknown[] = [];
    for (const v of cur) {
      if (v == null || typeof v !== 'object') continue;
      if (t.kind === 'wildcard') next.push(...(Array.isArray(v) ? v : Object.values(v)));
      else if (t.kind === 'index' && Array.isArray(v)) next.push(v[t.index]);
      else if (t.kind === 'child') next.push((v as Record<string, unknown>)[t.key]);
    }
    cur = next.filter((x) => x !== undefined);
  }
  return cur;
}
const first = (root: unknown, path?: string) => {
  const tokens = path ? parseJsonPath(path) : null;
  return tokens ? select(root, tokens)[0] : undefined;
};

function normalise(plan: SourcePlan, body: unknown) {
  const metrics: Record<string, unknown>[] = [];
  const t = Date.now();
  if (plan.response === 'standard_json' || plan.response === 'metrics_json' || !plan.metrics?.length) {
    const list = (body as { metrics?: Record<string, unknown>[] })?.metrics ?? [];
    for (const m of list) metrics.push({ source_id: plan.id, metric: m.metric, value: m.value, unit: m.unit ?? '', timestamp_ms: m.timestamp_ms ?? t, labels: m.labels ?? {} });
  }
  for (const m of plan.metrics ?? []) {
    const items = m.items_path ? select(body, parseJsonPath(m.items_path) ?? []) : [body];
    for (const item of items) {
      const value = Number(first(item, m.value_path));
      const name = m.name || String(first(item, m.name_path) ?? '');
      if (!name || !Number.isFinite(value)) continue;
      const labels: Record<string, string> = {};
      for (const l of m.labels ?? []) labels[l.name] = l.value || String(first(item, l.path) ?? first(body, l.path) ?? '');
      metrics.push({ source_id: plan.id, metric: name, value, unit: m.target_unit || m.unit || String(first(item, m.unit_path) ?? ''), timestamp_ms: t, labels });
    }
  }
  return { source_id: plan.id, status_code: 200, latency_ms: 42, metrics: metrics.slice(0, 16), events: [], assets: [], truncated: { metrics: metrics.length > 16, events: false } };
}

// ------------------------------------------------------------ routing
function json(status: number, body: unknown): Response {
  return new Response(body === null ? null : JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } });
}
const err = (status: number, code: string, message: string, fields?: unknown) => json(status, { error: { code, message, ...(fields ? { fields } : {}) } });

function validate(b: Bundle): { field: string; message: string }[] {
  const errors: { field: string; message: string }[] = [];
  if (!/^[A-Za-z0-9_-]{1,64}$/.test(b.dashboard.id ?? '')) errors.push({ field: 'dashboard.id', message: 'must match [A-Za-z0-9_-]{1,64}' });
  const ids = new Set<string>();
  b.dashboard.pages.forEach((page, pi) => {
    const issues = layoutIssues(page);
    page.panels.forEach((p, i) => {
      const path = `dashboard.pages[${pi}].panels[${i}]`;
      if (ids.has(p.id)) errors.push({ field: `${path}.id`, message: 'duplicate panel ID' });
      ids.add(p.id);
      if (!/^[A-Za-z0-9_-]{1,64}$/.test(p.id)) errors.push({ field: `${path}.id`, message: 'invalid ID' });
      for (const issue of issues.filter((x) => x.panelId === p.id)) errors.push({ field: `${path}.grid`, message: issue.message + (issue.otherId ? ' ' + issue.otherId : '') });
      if (p.source_id && !b.dashboard.sources.some((s) => s.id === p.source_id)) errors.push({ field: `${path}.source_id`, message: 'unknown source' });
    });
  });
  b.dashboard.pages.forEach((page, pi) => {
    if ('row_height' in page) errors.push({ field: `dashboard.pages[${pi}].row_height`, message: 'unknown field' });
  });
  b.dashboard.alert_rules.forEach((r, i) => {
    const path = `dashboard.alert_rules[${i}]`;
    if (!b.dashboard.sources.some((s) => s.id === r.source_id)) errors.push({ field: `${path}.source_id`, message: 'unknown source ID' });
    if (!r.metric) errors.push({ field: `${path}.metric`, message: 'metric is required and bounded' });
    const e = checkExpression(r.expression ?? '');
    if (e) errors.push({ field: `${path}.expression`, message: e.message });
    if (!r.style?.surfaces?.length) errors.push({ field: `${path}.style.surfaces`, message: 'select at least one known alert surface' });
  });
  b.dashboard.sources.forEach((s, i) => {
    if (s.kind === 'http' && !/^https?:\/\//.test(s.url ?? '')) errors.push({ field: `dashboard.sources[${i}].url`, message: 'must be an http(s) URL' });
  });
  return errors;
}

function templates() {
  return {
    schema: 'eego.dashboard-template-library/1',
    schema_version: 1,
    grid_rows_per_screen: 8,
    presets: [...presetsByDir('layouts'), ...presetsByDir('panels')].map(([, d]) => d),
    examples: presetsByDir('examples').map(([, d]) => {
      const b = d as { dashboard: { id: string; title: string } };
      return { id: b.dashboard.id, title: b.dashboard.title, bundle: d };
    }),
  };
}

async function handle(method: string, path: string, headers: Headers, body: unknown): Promise<Response> {
  await new Promise((r) => setTimeout(r, 120 + Math.random() * 180));
  if (method === 'POST' && path === 'auth/login') {
    return (body as { password?: string })?.password === PASSWORD ? json(200, { token: TOKEN, token_type: 'Bearer', expires_in: 3600 }) : err(401, 'unauthorized', 'authentication required');
  }
  if (headers.get('Authorization') !== `Bearer ${TOKEN}`) return err(401, 'unauthorized', 'authentication required');
  const route = `${method} ${path}`;
  const b = body as Record<string, unknown>;
  switch (route) {
    case 'POST auth/logout':
      return json(204, null);
    case 'POST auth/change-password':
      return (b.new_password as string)?.length >= 8 ? json(200, { status: 'changed' }) : err(400, 'validation_failed', 'password policy check failed', { new_password: 'at least 8 characters' });
    case 'GET status':
      return json(200, {
        device: { fw_version: 'mock-1.0.0', fw_sha256: 'abc', uptime_s: Math.floor(performance.now() / 1000) + 3600, mode: 'online' },
        network: { ssid: 'MockWiFi', ip: '192.168.1.50', rssi: -58, wifi_state: 3, ap_active: false },
        time: { valid: true, now: Math.floor(Date.now() / 1000), sntp_synced: true, last_sync_at: 0 },
        memory: { internal_free: 81234, psram_free: 6123456 },
        storage: { sd_ready: true, config_valid: true },
        alerts: { available: true, critical: 1, warning: 1, info: 0, active_total: 2 },
        security: { sessions: 1, ingest_key_masked: 'ab12****', provisioned: true },
      });
    case 'GET system/logs':
      return json(200, { entries: state.logs.slice().reverse() });
    case 'POST system/reboot':
      return json(202, { status: 'reboot scheduled' });
    case 'POST system/factory-reset':
      return b.password === PASSWORD ? json(202, { status: 'factory reset scheduled' }) : err(401, 'unauthorized', 'authentication required');
    case 'POST system/ingest-key/rotate':
      return json(200, { ingest_key: 'mock-' + Math.random().toString(36).slice(2) });
    case 'GET alerts':
      return json(200, { alerts: state.alerts, total: state.alerts.length, snapshot_seq: 1 });
    case 'POST alerts/ack-all':
      state.alerts.forEach((a) => a.state === 'active' && (a.state = 'acked'));
      return json(200, { ok: true, affected: 1 });
    case 'GET settings/schema':
      return json(200, SETTINGS_SCHEMA);
    case 'GET settings':
      return json(200, { schema_version: 1, revision: 1, settings: state.settings, capabilities: capabilities(), power: { last_wake_reason: 'power_on', recovery_ap_active: false }, ble: { compiled: true, role: 'off', provisioning_state: 'idle' } });
    case 'PUT settings': {
      const fields: Record<string, string> = {};
      for (const [k, v] of Object.entries(b)) {
        const f = (SETTINGS_SCHEMA.fields as Record<string, { range?: number[]; enum?: (string | number)[] }>)[k];
        if (!f) fields[k] = 'unknown field';
        else if (f.enum && !f.enum.includes(v as string | number)) fields[k] = `must be one of ${f.enum.join('|')}`;
        else if (f.range && typeof v === 'number' && (v < f.range[0] || v > f.range[1])) fields[k] = `must be in [${f.range[0]}, ${f.range[1]}]`;
      }
      const next = { ...state.settings, ...b };
      if (!fields.battery_max_stale_min && Number(next.battery_max_stale_min) < Number(next.battery_refresh_min))
        fields.battery_max_stale_min = 'must be >= battery_refresh_min';
      if (Object.keys(fields).length) return err(400, 'validation_failed', 'settings validation failed', fields);
      Object.assign(state.settings, b);
      return handle('GET', 'settings', headers, null);
    }
    case 'POST settings/ble/unbind':
      return json(200, { status: 'cleared' });
    case 'GET sources/credentials':
      return json(200, { namespace: 'source', credentials: state.credentials.map((key) => ({ key, ref: `nvs:source/${key}` })) });
    case 'PUT sources/credentials':
    case 'DELETE sources/credentials': {
      const key = String(b.key ?? '');
      if (!/^[A-Za-z0-9_-]{1,15}$/.test(key)) return err(400, 'validation_failed', 'validation failed', { key: 'must be 1..15 [A-Za-z0-9_-] chars' });
      state.credentials = state.credentials.filter((k) => k !== key);
      if (method === 'PUT') state.credentials.push(key);
      return json(200, { key, ref: `nvs:source/${key}`, deleted: method === 'DELETE' });
    }
    case 'GET dashboard':
      return state.bundle ? json(200, { ...state.bundle, revision: state.revision }) : json(404, { error: { code: 'not_found', message: 'dashboard is not configured' }, configured: false, revision: state.revision });
    case 'PUT dashboard':
    case 'POST dashboard': {
      const ifMatch = headers.get('If-Match');
      if (ifMatch && ifMatch !== '*' && Number(ifMatch) !== state.revision) return err(409, 'conflict', 'dashboard revision mismatch');
      const errors = validate(body as Bundle);
      if (errors.length) return err(400, 'validation_failed', 'dashboard validation failed', Object.fromEntries(errors.map((e) => [e.field, e.message])));
      state.revision += 1;
      state.bundle = { ...(body as Bundle), revision: state.revision };
      return json(200, state.bundle);
    }
    case 'POST dashboard/validate': {
      const errors = validate(body as Bundle);
      return json(200, { valid: !errors.length, errors });
    }
    case 'GET dashboard/sample':
      return json(200, parseBundle(presetsByDir('examples')[0][1]));
    case 'GET dashboard/templates':
      return json(200, templates());
    case 'POST dashboard/sources/probe': {
      const plan = b.plan as SourcePlan;
      const doc = mockBody(plan?.url ?? '');
      return json(200, { source_id: plan?.id ?? '', status_code: 200, latency_ms: 87, content_type: 'application/json', body_bytes: JSON.stringify(doc).length, format: 'json', json: doc, masked_values: 1, truncated: false });
    }
    case 'POST dashboard/sources/test': {
      const plan = b.plan as SourcePlan;
      return json(200, normalise(plan, mockBody(plan?.url ?? '')));
    }
  }
  const m = /^alerts\/([^/]+)\/(ack|clear)$/.exec(path);
  if (m && method === 'POST') {
    const a = state.alerts.find((x) => x.id === m![1]);
    if (!a) return err(404, 'not_found', 'alert not found');
    a.state = m[2] === 'ack' ? 'acked' : 'cleared';
    return json(200, { ok: true, affected: 1 });
  }
  if (method === 'POST' && path === 'dashboard/render-preview') {
    // Mirrors POST /dashboard/render-preview (crude drawing; real device uses its fonts).
    const req = body as { bundle?: unknown; page_id?: string; screen?: number; refresh?: string } | null;
    if (!req || typeof req.bundle !== 'object' || req.bundle === null) return err(400, 'invalid_request', 'bundle object required');
    if (req.refresh !== undefined && req.refresh !== 'full' && req.refresh !== 'fast') return err(400, 'invalid_request', 'refresh must be full or fast');
    if (req.screen !== undefined && (!Number.isInteger(req.screen) || req.screen < 0)) return err(400, 'invalid_request', 'screen must be a non-negative integer');
    renderCalls.splice(0, renderCalls.length, ...renderCalls.filter((t) => Date.now() - t < 60000));
    if (renderCalls.length >= 10) return new Response(JSON.stringify({ error: { code: 'rate_limited', message: 'too many preview renders' } }), { status: 429, headers: { 'Content-Type': 'application/json', 'Retry-After': '30' } });
    renderCalls.push(Date.now());
    let draft: Bundle;
    try {
      draft = parseBundle({ schema: 'eego.dashboard-bundle/1', ...(req.bundle as object) });
    } catch (e) {
      return err(400, 'invalid_request', String(e));
    }
    const errors = validate(draft);
    if (errors.length) return err(400, 'validation_failed', 'dashboard validation failed', Object.fromEntries(errors.map((e) => [e.field, e.message])));
    const page = draft.dashboard.pages.find((p) => p.id === (req.page_id || draft.dashboard.default_page_id)) ?? draft.dashboard.pages[0];
    if (!page) return err(400, 'invalid_request', 'unknown page');
    const screens = screenCount(page);
    const screen = Math.min(req.screen ?? 0, screens - 1);
    const blob = await snapshot(draft, page.id, screen, req.refresh === 'fast');
    if (!blob) return err(503, 'unavailable', 'render preview unavailable');
    const data = draft.dashboard.samples.length ? 'bundle' : 'placeholder';
    return new Response(blob, { status: 200, headers: { 'Content-Type': 'image/png', 'X-Preview-Page': page.id, 'X-Preview-Screen': String(screen), 'X-Preview-Screens': String(screens), 'X-Preview-Data': data } });
  }
  if (path === 'display/snapshot.bmp') {
    const blob = await snapshot();
    return blob ? new Response(blob, { status: 200, headers: { 'Content-Type': 'image/png' } }) : err(503, 'unavailable', 'display snapshot unavailable');
  }
  return err(404, 'not_found', 'resource not found');
}

/** Rough 1-bit-looking frame of the applied dashboard's first screen. */
const renderCalls: number[] = [];

async function snapshot(source: Bundle | null = state.bundle, pageId?: string, screen = 0, fast = false): Promise<Blob | null> {
  const b = source;
  const c = document.createElement('canvas');
  c.width = 552;
  c.height = 768;
  const g = c.getContext('2d');
  if (!g) return null;
  g.fillStyle = '#fff';
  g.fillRect(0, 0, 552, 768);
  g.fillStyle = '#000';
  g.strokeStyle = '#000';
  g.lineWidth = 1;
  const page = b?.dashboard.pages.find((p) => p.id === (pageId || b.dashboard.default_page_id)) ?? b?.dashboard.pages[0];
  if (!b || !page) {
    g.font = '16px sans-serif';
    g.fillText('No dashboard configured', 180, 384);
  } else {
    const cols = columnsOf(page);
    for (const p of page.panels.filter(isVisible)) {
      const px = panelPixels(p.grid, cols);
      if (px.screen !== screen) continue;
      g.strokeRect(px.x + 0.5, px.y + 0.5, px.w - 1, px.h - 1);
      let y = px.y + 6;
      if (p.title) {
        g.font = 'bold 16px sans-serif';
        g.fillText(p.title, px.x + 6, y + 15, px.w - 12);
        g.fillRect(px.x + 6, y + 19, px.w - 12, 1);
        y += 21;
      }
      const sample = b.dashboard.samples.filter((s) => s.source_id === p.source_id && s.metric === p.metric).pop();
      if (p.kind === 'card' && sample) {
        const size = Math.min(64, Math.max(16, Math.floor((px.y + px.h - y - 6) / 16) * 16));
        g.font = `bold ${size}px sans-serif`;
        const text = sample.value.toFixed(p.style?.precision ?? 1) + (p.style?.unit ? ' ' + p.style.unit : '');
        const w = g.measureText(text).width;
        g.fillText(text, px.x + Math.max(6, (px.w - w) / 2), y + (px.y + px.h - y) / 2 + size / 3, px.w - 12);
      } else if (p.kind === 'text' && p.text?.content) {
        g.font = '16px sans-serif';
        g.fillText(p.text.content.slice(0, 60), px.x + 6, y + 18, px.w - 12);
      } else if (p.kind === 'chart') {
        g.beginPath();
        const pts = 16;
        for (let i = 0; i < pts; i++) {
          const xx = px.x + 6 + (i * (px.w - 12)) / (pts - 1);
          const yy = y + 8 + ((Math.sin(i / 2) + 1) / 2) * (px.y + px.h - y - 20);
          if (i) g.lineTo(xx, yy);
          else g.moveTo(xx, yy);
        }
        g.stroke();
      } else if (p.kind !== 'button') {
        g.font = '16px sans-serif';
        g.fillStyle = fast ? '#000' : '#666';
        g.fillText('No data', px.x + px.w / 2 - 28, y + (px.y + px.h - y) / 2);
        g.fillStyle = '#000';
      } else {
        g.font = 'bold 16px sans-serif';
        g.fillText(p.actions?.[0]?.label ?? 'Button', px.x + 12, px.y + px.h / 2 + 5);
      }
    }
  }
  return new Promise((res) => c.toBlob((x) => res(x), 'image/png'));
}

export async function installMock(): Promise<void> {
  // Dev conveniences for headless screenshots: ?auto logs in, ?fresh starts
  // unconfigured, ?fixture=<name> loads .tmp/render/fixtures/<name>.json (the
  // firmware render harness input) as the applied dashboard.
  const q = new URLSearchParams(location.search);
  if (q.has('auto')) sessionStorage.setItem('eego_admin_token', TOKEN);
  if (q.has('fresh')) state.bundle = null;
  const fixture = q.get('fixture');
  if (fixture && /^[a-z0-9-]+$/.test(fixture)) {
    try {
      const res = await fetch(`/@fs${import.meta.env.VITE_REPO_ROOT ?? ''}/.tmp/render/fixtures/${fixture}.json`);
      if (res.ok) state.bundle = { ...parseBundle(await res.json()), revision: state.revision };
      localStorage.removeItem('eego_dashboard_draft');
    } catch {
      /* fixture missing: keep the default example */
    }
  }
  const realFetch = window.fetch.bind(window);
  window.fetch = async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = typeof input === 'string' ? input : input instanceof URL ? input.href : input.url;
    const u = new URL(url, location.href);
    if (!u.pathname.startsWith('/api/v1/')) return realFetch(input, init);
    const headers = new Headers(init?.headers);
    let body: unknown = null;
    if (typeof init?.body === 'string') {
      try {
        body = JSON.parse(init.body);
      } catch {
        return err(400, 'invalid_request', 'invalid JSON');
      }
    }
    return handle((init?.method ?? 'GET').toUpperCase(), u.pathname.slice(8), headers, body);
  };
  console.info('[eego] mock API installed — password: admin');
}

