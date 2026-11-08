import { useCallback, useEffect, useRef, useState, type FormEvent } from 'react';
import * as api from '../api';
import { useI18n } from '../i18n';
import { useToast } from '../state/toast';
import { errorText, fieldMap } from '../state/errors';
import { useDraft } from '../state/draft';
import { credentialKeyError, credentialValueError, sanitizeCredentialKey } from '../model/credentials';
import { Badge, Button, Card, EmptyState, Icon, SecretInput, Spinner, TextInput, PageHeader } from '../ui';
import type { DictKey } from '../i18n';

export function CredentialsPage() {
  const { t } = useI18n();
  const toast = useToast();
  const d = useDraft();
  const [list, setList] = useState<api.CredentialMeta[] | null>(null);
  const [key, setKey] = useState('');
  const [value, setValue] = useState('');
  const [errs, setErrs] = useState<Record<string, string>>({});
  const [busy, setBusy] = useState(false);
  const [loading, setLoading] = useState(false);
  const [loadErr, setLoadErr] = useState<string | null>(null);
  const [removing, setRemoving] = useState<string | null>(null);
  const valueRef = useRef<HTMLDivElement>(null);
  const exists = !!list?.some((c) => c.key === key.trim());

  const load = useCallback(() => {
    setLoading(true);
    api.listCredentials()
      .then(
        (r) => {
          setList(r.credentials);
          setLoadErr(null);
        },
        (e) => setLoadErr(errorText(e, t)),
      )
      .finally(() => setLoading(false));
  }, [t]);
  useEffect(load, [load]);

  const submit = async (e: FormEvent) => {
    e.preventDefault();
    const ke = credentialKeyError(key.trim());
    const ve = credentialValueError(value);
    if (ke || ve) {
      setErrs({ ...(ke ? { key: t(`auth.err.${ke}` as DictKey) } : {}), ...(ve ? { value: t(`auth.err.${ve}` as DictKey) } : {}) });
      return;
    }
    setBusy(true);
    setErrs({});
    try {
      await api.writeCredential(key.trim(), value);
      setValue('');
      toast('ok', t('cred.written', { key: key.trim() }));
      load();
    } catch (x) {
      setErrs(fieldMap(x));
      toast('error', errorText(x, t));
    } finally {
      setBusy(false);
    }
  };
  const remove = async (k: string, users: string[]) => {
    if (removing) return;
    if (!confirm(users.length ? t('cred.delete_confirm_used', { key: k, sources: users.join('、') }) : t('cred.delete_confirm', { key: k }))) return;
    setRemoving(k);
    try {
      await api.deleteCredential(k);
      toast('ok', t('cred.deleted', { key: k }));
      load();
    } catch (x) {
      toast('error', errorText(x, t));
    } finally {
      setRemoving(null);
    }
  };
  const usedBy = (ref: string) =>
    d.draft.dashboard.sources.filter((s) => s.auth_token_ref === ref || Object.values(s.header_secret_refs ?? {}).includes(ref)).map((s) => s.id);

  return (
    <div className="page stack">
      <PageHeader icon="key" title={t('cred.title')} subtitle={t('cred.subtitle')} />
      <Card title={t('cred.write')}>
        <form className="form-grid two" onSubmit={submit} autoComplete="off">
          <TextInput label={t('auth.key')} value={key} onChange={(v) => setKey(sanitizeCredentialKey(v))} error={errs.key} hint={exists ? t('auth.key_overwrite') : t('auth.key_hint')} maxLength={15} spellCheck={false} required />
          <div ref={valueRef}>
            <SecretInput label={t('cred.value')} value={value} onChange={setValue} error={errs.value} maxLength={1024} showLabel={t('auth.show')} hideLabel={t('auth.hide')} placeholder={t('auth.secret_ph')} required />
          </div>
          <div className="wide row">
            <Button type="submit" variant="primary" loading={busy} disabled={!key || !value}>
              <Icon name="key" size={15} /> {exists ? t('cred.overwrite') : t('auth.save_to_device')}
            </Button>
            {(key || value) && (
              <Button variant="ghost" onClick={() => { setKey(''); setValue(''); setErrs({}); }}>
                {t('common.clear')}
              </Button>
            )}
          </div>
        </form>
      </Card>
      <Card title={t('cred.list')} actions={<Button size="sm" loading={loading} onClick={load}><Icon name="refresh" size={14} /> {t('common.refresh')}</Button>}>
        {loadErr && <div className="notice warn">{loadErr}</div>}
        {!list ? (
          !loadErr && <Spinner />
        ) : !list.length ? (
          <EmptyState icon="key" title={t('cred.empty')}>{t('cred.empty_h')}</EmptyState>
        ) : (
          <div className="scroll-x">
            <table className="table stack-sm">
              <thead>
                <tr>
                  <th>{t('auth.key')}</th>
                  <th>{t('cred.used_by')}</th>
                  <th />
                </tr>
              </thead>
              <tbody>
                {list.map((c) => {
                  const users = usedBy(c.ref);
                  return (
                    <tr key={c.key}>
                      <td className="mono">{c.key}</td>
                      <td>{users.length ? users.map((u) => <Badge key={u} tone="accent">{u}</Badge>) : <span className="dim">{t('cred.unused')}</span>}</td>
                      <td className="cell-actions">
                        <Button
                          size="sm"
                          onClick={() => {
                            setKey(c.key);
                            setValue('');
                            valueRef.current?.scrollIntoView({ block: 'center', behavior: 'smooth' });
                            valueRef.current?.querySelector('input')?.focus();
                          }}
                        >
                          {t('cred.replace')}
                        </Button>
                        <Button size="sm" variant="ghost" loading={removing === c.key} disabled={!!removing} onClick={() => void remove(c.key, users)}>
                          {t('common.delete')}
                        </Button>
                      </td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        )}
      </Card>
    </div>
  );
}
