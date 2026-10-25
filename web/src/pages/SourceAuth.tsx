// Source authentication editor: mode [none | Bearer | secret headers], with
// inline "create / replace token" forms that write straight to the device's
// write-only credential store (PUT /sources/credentials) and then reference
// the stored secret from the draft source. Basic auth is not offered: the
// firmware only injects Bearer tokens and named secret headers, and reserves
// the Authorization header.
import { useEffect, useState, type FormEvent } from 'react';
import * as api from '../api';
import { useI18n, type DictKey } from '../i18n';
import { useToast } from '../state/toast';
import { errorText, fieldMap } from '../state/errors';
import {
  AUTH_MODES,
  MAX_HEADERS,
  authModeOf,
  credentialKeyError,
  credentialRef,
  credentialValueError,
  keyFromRef,
  sanitizeCredentialKey,
  secretHeaderErrors,
  suggestCredentialKey,
  type AuthMode,
} from '../model/credentials';
import type { SourcePlan } from '../model/types';
import { Badge, Button, Icon, Segmented, SecretInput, TextInput } from '../ui';

interface SecretFieldProps {
  /** Current "nvs:source/<key>" ref ('' when none). */
  value: string;
  onChange: (ref: string) => void;
  creds: api.CredentialMeta[] | null;
  reloadCreds: () => Promise<void>;
  /** Suggested key for a new credential. */
  suggest: string;
  /** Label for the secret ("令牌" / "请求头的值"). */
  secretLabel: string;
  compact?: boolean;
}

/** One credential reference: shows the stored secret or an inline create/replace form. */
export function SecretField({ value, onChange, creds, reloadCreds, suggest, secretLabel, compact }: SecretFieldProps) {
  const { t } = useI18n();
  const toast = useToast();
  const key = keyFromRef(value);
  const exists = !!key && !!creds?.some((c) => c.key === key);
  const others = (creds ?? []).filter((c) => c.key !== key);
  const [mode, setMode] = useState<'new' | 'replace' | null>(null);
  const [name, setName] = useState('');
  const [secret, setSecret] = useState('');
  const [errs, setErrs] = useState<{ key?: string; value?: string }>({});
  const [busy, setBusy] = useState(false);

  // No stored token yet and nothing to pick → open the create form directly.
  const formOpen = mode !== null || (!value && creds !== null && !creds.length);
  const formMode = mode ?? (value ? 'replace' : 'new');
  useEffect(() => {
    if (formOpen && formMode === 'new' && !name) setName(suggest);
  }, [formOpen, formMode, name, suggest]);

  const close = () => {
    setMode(null);
    setSecret('');
    setErrs({});
  };
  const openNew = () => {
    setName(suggest);
    setSecret('');
    setErrs({});
    setMode('new');
  };

  const submit = async (e?: FormEvent) => {
    e?.preventDefault();
    const k = formMode === 'replace' ? key : name.trim();
    const ke = credentialKeyError(k);
    const ve = credentialValueError(secret);
    if (ke || ve) {
      setErrs({ key: ke ? t(`auth.err.${ke}` as DictKey) : undefined, value: ve ? t(`auth.err.${ve}` as DictKey) : undefined });
      return;
    }
    setBusy(true);
    try {
      const r = await api.writeCredential(k, secret);
      setSecret('');
      onChange(r.ref || credentialRef(k));
      toast('ok', t(formMode === 'replace' ? 'auth.replaced' : 'auth.created', { key: k }));
      setMode(null);
      setErrs({});
      await reloadCreds();
    } catch (x) {
      const f = fieldMap(x);
      setErrs({ key: f.key, value: f.value });
      toast('error', errorText(x, t));
    } finally {
      setBusy(false);
    }
  };

  if (formOpen) {
    const overwrite = formMode === 'new' && !!creds?.some((c) => c.key === name.trim());
    return (
      <form className={`secret-form${compact ? ' compact' : ''}`} onSubmit={(e) => void submit(e)} autoComplete="off">
        <div className="secret-form-fields">
          {formMode === 'new' ? (
            <TextInput
              label={t('auth.key')}
              value={name}
              onChange={(v) => {
                setName(sanitizeCredentialKey(v));
                setErrs((s) => ({ ...s, key: undefined }));
              }}
              maxLength={15}
              spellCheck={false}
              error={errs.key}
              hint={overwrite ? t('auth.key_overwrite') : t('auth.key_hint')}
            />
          ) : (
            <div className="field">
              <span className="label">{t('auth.key')}</span>
              <div className="secret-fixed mono">{key}</div>
              <span className="hint">{t('auth.replace_hint')}</span>
            </div>
          )}
          <SecretInput
            label={secretLabel}
            value={secret}
            onChange={(v) => {
              setSecret(v);
              setErrs((s) => ({ ...s, value: undefined }));
            }}
            maxLength={1024}
            error={errs.value}
            placeholder={t('auth.secret_ph')}
            showLabel={t('auth.show')}
            hideLabel={t('auth.hide')}
            autoFocus={mode !== null}
          />
        </div>
        <div className="row">
          <Button type="submit" size="sm" variant="primary" loading={busy} disabled={!secret}>
            <Icon name="key" size={14} /> {t('auth.save_to_device')}
          </Button>
          {(mode !== null && (value || creds?.length)) ? (
            <Button size="sm" variant="ghost" onClick={close}>
              {t('common.cancel')}
            </Button>
          ) : null}
        </div>
      </form>
    );
  }

  return (
    <div className={`secret-field${compact ? ' compact' : ''}`}>
      {value ? (
        <div className={`secret-chip${exists ? '' : ' missing'}`}>
          <Icon name="key" size={15} />
          <span className="mono truncate">{key || value}</span>
          {creds === null ? null : exists ? <Badge tone="ok">{t('auth.stored')}</Badge> : <Badge tone="warn">{t('auth.missing')}</Badge>}
        </div>
      ) : (
        <select className="input secret-select" value="" aria-label={t('auth.pick')} onChange={(e) => e.target.value && onChange(credentialRef(e.target.value))}>
          <option value="">{t('auth.pick')}</option>
          {others.map((c) => (
            <option key={c.key} value={c.key}>
              {c.key}
            </option>
          ))}
        </select>
      )}
      <div className="row secret-actions">
        {value ? (
          <>
            <Button size="sm" onClick={() => setMode('replace')} disabled={!key}>
              {exists ? t('auth.replace') : t('auth.rewrite')}
            </Button>
            {others.length > 0 && (
              <select className="input sm secret-select" value="" aria-label={t('auth.switch')} onChange={(e) => e.target.value && onChange(credentialRef(e.target.value))}>
                <option value="">{t('auth.switch')}</option>
                {others.map((c) => (
                  <option key={c.key} value={c.key}>
                    {c.key}
                  </option>
                ))}
              </select>
            )}
            <Button size="sm" variant="ghost" onClick={() => onChange('')}>
              {t('auth.unlink')}
            </Button>
          </>
        ) : (
          <Button size="sm" onClick={openNew}>
            <Icon name="plus" size={14} /> {t('auth.new')}
          </Button>
        )}
      </div>
      {value && !exists && creds !== null && <span className="err-text">{t('auth.missing_hint', { key: key || value })}</span>}
    </div>
  );
}

interface AuthSectionProps {
  sourceId: string;
  plan: SourcePlan;
  setPlan: (fn: (p: SourcePlan) => void, key?: string) => void;
  creds: api.CredentialMeta[] | null;
  reloadCreds: () => Promise<void>;
  errs: Record<string, string>;
}

export function AuthSection({ sourceId, plan, setPlan, creds, reloadCreds, errs }: AuthSectionProps) {
  const { t } = useI18n();
  const derived = authModeOf(plan);
  const [mode, setMode] = useState<AuthMode>(derived);
  // Follow undo/redo and external edits; keep a chosen-but-empty mode otherwise.
  useEffect(() => {
    if (derived !== 'none') setMode(derived);
  }, [derived]);
  const keys = (creds ?? []).map((c) => c.key);
  const secret = plan.header_secret_refs ?? {};
  const secretCount = Object.keys(secret).length;

  const pick = (m: AuthMode) => {
    setMode(m);
    setPlan((p) => {
      if (m !== 'bearer') p.auth_token_ref = '';
      if (m !== 'headers') p.header_secret_refs = {};
    }, 'authmode');
  };

  return (
    <div className="auth-section">
      <div className="auth-head">
        <span className="label-strong">{t('auth.title')}</span>
        <Segmented<AuthMode> value={mode} onChange={pick} label={t('auth.title')} options={AUTH_MODES.map((m) => ({ value: m, label: t(`auth.mode.${m}` as DictKey) }))} />
      </div>
      <p className="dim auth-note">
        <Icon name="shield" size={13} /> {t('auth.note')}
      </p>
      {mode === 'bearer' && (
        <div className="auth-body">
          <SecretField
            value={plan.auth_token_ref ?? ''}
            onChange={(ref) => setPlan((p) => (p.auth_token_ref = ref), 'auth')}
            creds={creds}
            reloadCreds={reloadCreds}
            suggest={suggestCredentialKey(sourceId, keys)}
            secretLabel={t('auth.token')}
          />
          <span className="hint">{t('auth.bearer_hint')}</span>
          {errs.auth_token_ref && <span className="err-text">{errs.auth_token_ref}</span>}
        </div>
      )}
      {mode === 'headers' && (
        <SecretHeaders
          sourceId={sourceId}
          value={secret}
          plainCount={Object.keys(plan.headers ?? {}).length}
          onChange={(v) => setPlan((p) => (p.header_secret_refs = v), 'hsr')}
          creds={creds}
          reloadCreds={reloadCreds}
          keys={keys}
          error={errs.header_secret_refs}
        />
      )}
      {mode === 'none' && <span className="hint">{t('auth.none_hint')}</span>}
      {mode === 'bearer' && secretCount > 0 && <span className="hint">{t('auth.also_headers', { n: secretCount })}</span>}
    </div>
  );
}

function SecretHeaders({
  sourceId,
  value,
  plainCount,
  onChange,
  creds,
  reloadCreds,
  keys,
  error,
}: {
  sourceId: string;
  value: Record<string, string>;
  plainCount: number;
  onChange: (v: Record<string, string>) => void;
  creds: api.CredentialMeta[] | null;
  reloadCreds: () => Promise<void>;
  keys: string[];
  error?: string;
}) {
  const { t } = useI18n();
  // Rows keep transient empty/duplicate names while typing.
  const [rows, setRows] = useState<[string, string][]>(() => (Object.keys(value).length ? Object.entries(value) : [['X-Api-Key', '']]));
  const sig = JSON.stringify(value);
  const [lastSig, setLastSig] = useState(sig);
  if (sig !== lastSig) {
    setLastSig(sig);
    const mine = JSON.stringify(Object.fromEntries(rows.filter(([k, r]) => k && r)));
    if (mine !== sig) setRows(Object.entries(value));
  }
  const commit = (next: [string, string][]) => {
    setRows(next);
    const obj: Record<string, string> = {};
    for (const [k, r] of next) if (k && r) obj[k] = r;
    const s = JSON.stringify(obj);
    setLastSig(s);
    onChange(obj);
  };
  const errs = secretHeaderErrors(Object.fromEntries(rows.map(([k, r]) => [k, r || 'x'])));
  const full = rows.length + plainCount >= MAX_HEADERS;
  return (
    <div className="auth-body col">
      {rows.map(([name, ref], i) => {
        const e = name ? errs[name] : undefined;
        return (
          <div key={i} className="secret-header">
            <div className="secret-header-name">
              <TextInput
                label={t('auth.header_name')}
                value={name}
                placeholder="X-Api-Key"
                spellCheck={false}
                maxLength={64}
                error={e && e !== 'ref_missing' ? t(`auth.herr.${e}` as DictKey) : !name && ref ? t('auth.herr.name_invalid') : undefined}
                onChange={(v) => commit(rows.map((r, j) => (j === i ? [v.trim(), r[1]] : r)))}
              />
              <Button size="sm" variant="ghost" icon aria-label={t('common.delete')} title={t('common.delete')} onClick={() => commit(rows.filter((_, j) => j !== i))}>
                <Icon name="trash" size={15} />
              </Button>
            </div>
            <SecretField
              compact
              value={ref}
              onChange={(r) => commit(rows.map((x, j) => (j === i ? [x[0], r] : x)))}
              creds={creds}
              reloadCreds={reloadCreds}
              suggest={suggestCredentialKey(sourceId, keys, name)}
              secretLabel={t('auth.header_value')}
            />
          </div>
        );
      })}
      <div className="row">
        <Button size="sm" disabled={full} onClick={() => setRows([...rows, ['', '']])}>
          <Icon name="plus" size={14} /> {t('auth.add_header')}
        </Button>
        <span className="hint">{t('auth.headers_hint')}</span>
      </div>
      {error && <span className="err-text">{error}</span>}
    </div>
  );
}
