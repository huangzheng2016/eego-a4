import { describe, expect, it } from 'vitest';
import {
  authModeOf,
  credentialKeyError,
  credentialRef,
  credentialValueError,
  keyFromRef,
  plainHeaderErrors,
  sanitizeCredentialKey,
  secretHeaderErrors,
  sensitiveHeaderName,
  suggestCredentialKey,
} from '../src/model/credentials';

describe('credential keys and values (net::CredentialStore rules)', () => {
  it('validates keys', () => {
    expect(credentialKeyError('')).toBe('key_empty');
    expect(credentialKeyError('a'.repeat(16))).toBe('key_long');
    expect(credentialKeyError('bad key')).toBe('key_chars');
    expect(credentialKeyError('weather_tok-1')).toBeNull();
  });
  it('validates values', () => {
    expect(credentialValueError('')).toBe('value_empty');
    expect(credentialValueError('a\nb')).toBe('value_ctrl');
    expect(credentialValueError('x'.repeat(1025))).toBe('value_long');
    expect(credentialValueError('中'.repeat(342))).toBe('value_long'); // 1026 UTF-8 bytes
    expect(credentialValueError('sk-abc 123')).toBeNull();
  });
  it('sanitizes and round-trips refs', () => {
    expect(sanitizeCredentialKey('my token!/with-long-name')).toBe('mytokenwith-lon');
    expect(credentialRef('k1')).toBe('nvs:source/k1');
    expect(keyFromRef('nvs:source/k1')).toBe('k1');
    expect(keyFromRef('nvs:sec/admin')).toBe('');
    expect(keyFromRef('nvs:source/bad key')).toBe('');
    expect(keyFromRef(undefined)).toBe('');
  });
});

describe('suggestCredentialKey', () => {
  it('derives a key from the source id within 15 bytes', () => {
    expect(suggestCredentialKey('weather', [])).toBe('weather_tok');
    const long = suggestCredentialKey('very.long-source.identifier', []);
    expect(long.length).toBeLessThanOrEqual(15);
    expect(long.endsWith('_tok')).toBe(true);
    expect(credentialKeyError(long)).toBeNull();
  });
  it('uses the header name and avoids collisions', () => {
    expect(suggestCredentialKey('api', [], 'X-Api-Key')).toBe('api_xapike');
    expect(suggestCredentialKey('weather', ['weather_tok'])).toBe('weather_tok2');
    const taken = ['abcdefghijk_tok', 'abcdefghijk_to2'];
    const k = suggestCredentialKey('abcdefghijklmnop', taken);
    expect(taken).not.toContain(k);
    expect(k.length).toBeLessThanOrEqual(15);
  });
  it('falls back when the id has no usable characters', () => {
    expect(suggestCredentialKey('…', [])).toBe('source_tok');
  });
});

describe('auth mode + header validation', () => {
  it('derives the mode', () => {
    expect(authModeOf({ auth_token_ref: '', header_secret_refs: {} })).toBe('none');
    expect(authModeOf({ auth_token_ref: 'nvs:source/a' })).toBe('bearer');
    expect(authModeOf({ header_secret_refs: { 'X-Api-Key': 'nvs:source/a' } })).toBe('headers');
  });
  it('flags sensitive and reserved plain headers', () => {
    expect(sensitiveHeaderName('X-Api-Key')).toBe(true);
    expect(sensitiveHeaderName('Accept')).toBe(false);
    expect(plainHeaderErrors({ Accept: 'application/json', 'X-Api-Key': 'x', Authorization: 'Bearer x', Host: 'h', 'bad name': 'v' })).toEqual({
      'X-Api-Key': 'name_sensitive',
      Authorization: 'name_authorization',
      Host: 'name_reserved',
      'bad name': 'name_invalid',
    });
    expect(plainHeaderErrors({ 'X-Tenant': 'a' }, { 'x-tenant': 'nvs:source/t' })).toEqual({ 'X-Tenant': 'name_duplicate' });
    expect(plainHeaderErrors({ Accept: 'a\nb' })).toEqual({ Accept: 'value_ctrl' });
  });
  it('validates secret headers', () => {
    expect(secretHeaderErrors({ 'X-Api-Key': 'nvs:source/k', Authorization: 'nvs:source/k', 'X-Other': 'plain' })).toEqual({
      Authorization: 'name_authorization',
      'X-Other': 'ref_missing',
    });
  });
});
