// Source authentication helpers (pure; unit-tested in test/credentials.test.ts).
// Rules mirror firmware net::CredentialStore (components/net_stack/src/credentials.cpp)
// and data_pipeline buildSourceRequest(): the device supports exactly
//   * auth_token_ref     → "Authorization: Bearer <secret>"
//   * header_secret_refs → "<Header>: <secret>" (Authorization/Host/Content-Length reserved)
// Secrets live write-only in NVS namespace "source"; configs carry "nvs:source/<key>".
import type { Source } from './types';

export const CRED_PREFIX = 'nvs:source/';
export const CRED_KEY_MAX = 15;
export const CRED_VALUE_MAX = 1024;
export const HEADER_NAME_MAX = 64;
/** Plain + secret header budget per source (dashboard_core kMaxHeaders). */
export const MAX_HEADERS = 16;

export type AuthMode = 'none' | 'bearer' | 'headers';
export const AUTH_MODES: AuthMode[] = ['none', 'bearer', 'headers'];

export const credentialRef = (key: string) => CRED_PREFIX + key;
/** Key part of a valid "nvs:source/<key>" ref, '' otherwise. */
export function keyFromRef(ref: string | undefined): string {
  if (!ref || !ref.startsWith(CRED_PREFIX)) return '';
  const key = ref.slice(CRED_PREFIX.length);
  return credentialKeyError(key) ? '' : key;
}

export type CredKeyError = 'key_empty' | 'key_long' | 'key_chars';
export function credentialKeyError(key: string): CredKeyError | null {
  if (!key) return 'key_empty';
  if (key.length > CRED_KEY_MAX) return 'key_long';
  if (!/^[A-Za-z0-9_-]+$/.test(key)) return 'key_chars';
  return null;
}

export type CredValueError = 'value_empty' | 'value_long' | 'value_ctrl';
export function credentialValueError(value: string): CredValueError | null {
  if (!value) return 'value_empty';
  if (new TextEncoder().encode(value).length > CRED_VALUE_MAX) return 'value_long';
  // eslint-disable-next-line no-control-regex
  if (/[\u0000-\u001f\u007f]/.test(value)) return 'value_ctrl';
  return null;
}

/** Strip characters the device rejects and clamp to 15 bytes. */
export function sanitizeCredentialKey(raw: string): string {
  return raw.replace(/[^A-Za-z0-9_-]/g, '').slice(0, CRED_KEY_MAX);
}

/**
 * Suggest a free credential key derived from the source id (and optional
 * header name), e.g. "weather" → "weather_tok", "api-x" + "X-Api-Key" →
 * "api-x_xapikey". Collisions get a numeric suffix within the 15-byte budget.
 */
export function suggestCredentialKey(sourceId: string, existing: string[], header?: string): string {
  const suffix = header ? '_' + header.replace(/[^A-Za-z0-9]/g, '').toLowerCase().slice(0, 6) : '_tok';
  const base = sanitizeCredentialKey(sourceId) || 'source';
  const stem = base.slice(0, Math.max(1, CRED_KEY_MAX - suffix.length));
  const taken = new Set(existing);
  let cand = (stem + suffix).slice(0, CRED_KEY_MAX);
  for (let i = 2; taken.has(cand) && i < 1000; i++) {
    const n = String(i);
    cand = (stem + suffix).slice(0, CRED_KEY_MAX - n.length) + n;
  }
  return cand;
}

/** Mode shown for a source: Bearer wins when both are configured. */
export function authModeOf(s: Pick<Source, 'auth_token_ref' | 'header_secret_refs'>): AuthMode {
  if (s.auth_token_ref) return 'bearer';
  if (Object.keys(s.header_secret_refs ?? {}).length) return 'headers';
  return 'none';
}

// ---------------------------------------------------------------- headers
/** net::CredentialStore::sensitiveHeaderName — such names cannot be plain headers. */
export function sensitiveHeaderName(name: string): boolean {
  const l = name.toLowerCase();
  return ['auth', 'key', 'token', 'pass', 'secret', 'cookie', 'credential'].some((w) => l.includes(w));
}
const RESERVED = ['authorization', 'host', 'content-length'];
export const reservedHeaderName = (name: string) => RESERVED.includes(name.toLowerCase());
export const validHeaderName = (name: string) => name.length > 0 && name.length <= HEADER_NAME_MAX && /^[A-Za-z0-9!#$%&'*+.^_`|~-]+$/.test(name);

export type HeaderError = 'name_invalid' | 'name_reserved' | 'name_authorization' | 'name_sensitive' | 'name_duplicate' | 'value_ctrl' | 'value_long' | 'ref_missing';

/** Inline validation for plain (non-secret) headers, keyed by header name. */
export function plainHeaderErrors(headers: Record<string, string> | undefined, secret: Record<string, string> | undefined = {}): Record<string, HeaderError> {
  const out: Record<string, HeaderError> = {};
  const seen = new Set<string>();
  for (const [k, v] of Object.entries(headers ?? {})) {
    const l = k.toLowerCase();
    if (!validHeaderName(k)) out[k] = 'name_invalid';
    else if (l === 'authorization') out[k] = 'name_authorization';
    else if (reservedHeaderName(k)) out[k] = 'name_reserved';
    else if (sensitiveHeaderName(k)) out[k] = 'name_sensitive';
    else if (seen.has(l) || Object.keys(secret).some((s) => s.toLowerCase() === l)) out[k] = 'name_duplicate';
    // eslint-disable-next-line no-control-regex
    else if (/[\u0000-\u0008\u000a-\u001f\u007f]/.test(v)) out[k] = 'value_ctrl';
    else if ([...v].length > 512) out[k] = 'value_long';
    seen.add(l);
  }
  return out;
}

/** Inline validation for secret headers (name → ref). */
export function secretHeaderErrors(secret: Record<string, string> | undefined): Record<string, HeaderError> {
  const out: Record<string, HeaderError> = {};
  const seen = new Set<string>();
  for (const [k, ref] of Object.entries(secret ?? {})) {
    const l = k.toLowerCase();
    if (!validHeaderName(k)) out[k] = 'name_invalid';
    else if (l === 'authorization') out[k] = 'name_authorization';
    else if (reservedHeaderName(k)) out[k] = 'name_reserved';
    else if (seen.has(l)) out[k] = 'name_duplicate';
    else if (!keyFromRef(ref)) out[k] = 'ref_missing';
    seen.add(l);
  }
  return out;
}
