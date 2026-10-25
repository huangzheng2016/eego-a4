// Types mirroring firmware/components/dashboard_core/include/dashboard_core/model.h
// and the eego.dashboard-bundle/1 wire format (DashboardStore::toJson).
// Fields are optional where the firmware applies defaults on parse.

export type Severity = 'info' | 'warning' | 'critical';
export type PanelKind = 'text' | 'image' | 'card' | 'table' | 'button' | 'alert' | 'chart';
export type SourceKind = 'http' | 'static';
export type VariableKind = 'text' | 'constant' | 'query' | 'time_range' | 'locale';
export type ActionKind = 'goto_page' | 'goto_panel' | 'set_variable' | 'refresh' | 'back' | 'ack_alert';
export type ChartKind = 'line' | 'bar' | 'sparkline';
export type ImageFit = 'contain' | 'cover' | 'stretch';
export type AlertSurface = 'banner' | 'card' | 'table_row' | 'sparkline';
export type ResponseKind =
  | 'generic_json'
  | 'prometheus'
  | 'standard_json'
  | 'metrics_json'
  | 'events_json'
  | 'image'
  | 'text';

export const PANEL_KINDS: PanelKind[] = ['text', 'card', 'chart', 'table', 'alert', 'image', 'button'];
export const SEVERITIES: Severity[] = ['info', 'warning', 'critical'];
export const ACTION_KINDS: ActionKind[] = ['goto_page', 'goto_panel', 'set_variable', 'refresh', 'back', 'ack_alert'];
export const CHART_KINDS: ChartKind[] = ['line', 'bar', 'sparkline'];
export const IMAGE_FITS: ImageFit[] = ['contain', 'cover', 'stretch'];
export const RESPONSE_KINDS: ResponseKind[] = [
  'standard_json',
  'generic_json',
  'prometheus',
  'metrics_json',
  'events_json',
  'image',
  'text',
];
export const VARIABLE_KINDS: VariableKind[] = ['text', 'constant', 'query', 'time_range', 'locale'];

// validate.cpp validVariant(): "default" is always accepted.
export const VARIANTS: Record<PanelKind, string[]> = {
  text: ['default', 'single', 'multiline', 'key_value', 'description', 'log_tail', 'markdown'],
  card: ['default', 'delta', 'progress', 'status', 'dual', 'triple'],
  table: ['default', 'status_grid', 'heatmap', 'timeline'],
  chart: ['default'],
  image: ['default'],
  button: ['default'],
  alert: ['default'],
};

export interface GridRect {
  x: number;
  y: number;
  w: number;
  h: number;
}

export interface TimeWindow {
  from: string;
  to: string;
}

export interface Threshold {
  value: number;
  severity: Severity;
  label?: string;
}

export interface Style {
  unit?: string;
  precision?: number;
  line_width?: number;
  show_legend?: boolean;
  invert?: boolean;
  variant?: string;
}

export interface Action {
  id: string;
  kind: ActionKind;
  label?: string;
  target?: string;
  value?: string;
  confirm?: boolean;
}

export interface TextSpec {
  content: string;
  multiline?: boolean;
  max_lines?: number;
}

export interface ImageSpec {
  asset_id: string;
  alt_text?: string;
  fit?: ImageFit;
}

export interface ChartSpec {
  kind: ChartKind;
  show_points?: boolean;
  fill?: boolean;
  y_min?: number | null;
  y_max?: number | null;
}

export interface TableColumn {
  key: string;
  title?: string;
  unit?: string;
  width?: number;
}

export interface Panel {
  id: string;
  kind: PanelKind;
  grid: GridRect;
  title?: string;
  visible?: boolean;
  source_id?: string;
  metric?: string;
  value_path?: string;
  value_expression?: string;
  time_window?: TimeWindow;
  refresh_ms?: number;
  thresholds?: Threshold[];
  style?: Style;
  linked_panel_ids?: string[];
  actions?: Action[];
  text?: TextSpec;
  image?: ImageSpec;
  chart?: ChartSpec;
  columns?: TableColumn[];
}

export interface Source {
  id: string;
  kind: SourceKind;
  enabled?: boolean;
  url?: string;
  refresh_ms?: number;
  timeout_ms?: number;
  max_response_bytes?: number;
  tls_verify?: boolean;
  auth_token_ref?: string;
  headers?: Record<string, string>;
  header_secret_refs?: Record<string, string>;
}

export interface VariableOption {
  label: string;
  value: string;
}

export interface Variable {
  name: string;
  label?: string;
  kind: VariableKind;
  default_value?: string;
  options?: VariableOption[];
  source_id?: string;
  value_path?: string;
  multi?: boolean;
  include_all?: boolean;
}

export interface AlertRule {
  id: string;
  title?: string;
  enabled?: boolean;
  source_id?: string;
  metric?: string;
  expression?: string;
  for_ms?: number;
  message?: string;
  style?: { surfaces?: AlertSurface[]; severity?: Severity; linked_panel_id?: string };
}

export interface Asset {
  id: string;
  kind: 'image' | 'font' | 'binary';
  uri?: string;
  media_type?: string;
  sha256?: string;
  size_bytes?: number;
  cache_seconds?: number;
}

export interface MetricSample {
  source_id: string;
  metric: string;
  timestamp_ms?: number;
  value: number;
  unit?: string;
  labels?: Record<string, string>;
}

export interface Page {
  id: string;
  title?: string;
  visible?: boolean;
  grid_columns?: number;
  panels: Panel[];
}

export interface Dashboard {
  schema?: string;
  schema_version?: number;
  id: string;
  title?: string;
  default_page_id?: string;
  refresh_ms?: number;
  sources: Source[];
  variables: Variable[];
  pages: Page[];
  alert_rules: AlertRule[];
  assets: Asset[];
  samples: MetricSample[];
  events: unknown[];
}

// data_pipeline source_plan.cpp mapping structures.
export interface LabelMapping {
  name: string;
  path?: string;
  value?: string;
}

export interface MetricMapping {
  name?: string;
  items_path?: string;
  name_path?: string;
  value_path?: string;
  timestamp_path?: string;
  unit?: string;
  unit_path?: string;
  target_unit?: string;
  expression?: string;
  reset_at_path?: string;
  fields?: Record<string, string>;
  labels?: LabelMapping[];
}

export interface EventMapping {
  items_path?: string;
  id_path?: string;
  rule_id_path?: string;
  timestamp_path?: string;
  severity_path?: string;
  state_path?: string;
  title_path?: string;
  message_path?: string;
  linked_panel_id_path?: string;
  severity_map?: Record<string, string>;
  clear_states?: string[];
}

export interface SourcePlan extends Source {
  response?: ResponseKind;
  method?: string;
  body?: string;
  static_body?: string;
  metrics?: MetricMapping[];
  events?: EventMapping[];
  text_path?: string;
  reset_at_path?: string;
  stale_after_ms?: number;
  conditional_get?: boolean;
  max_json_depth?: number;
}

export interface Bundle {
  schema: 'eego.dashboard-bundle/1';
  schema_version: 1;
  revision: number;
  dashboard: Dashboard;
  source_plans: SourcePlan[];
}

export interface PresetDoc {
  schema: 'eego.dashboard-preset/1';
  schema_version: 1;
  id: string;
  kind: 'panel' | 'layout';
  title?: string;
  description?: string;
  dashboard: Partial<Dashboard> & { pages: Page[] };
  source_plans?: SourcePlan[];
}

export const BUNDLE_SCHEMA = 'eego.dashboard-bundle/1';
export const DASHBOARD_SCHEMA = 'eego.dashboard/2';
export const PRESET_SCHEMA = 'eego.dashboard-preset/1';
