import { useCallback, useEffect, useState, type FormEvent } from 'react';
import * as api from '../api';
import { useI18n } from '../i18n';
import { useToast } from '../state/toast';
import { errorText, fieldMap } from '../state/errors';
import { useDraft } from '../state/draft';
import { Button, Card, Empty, Spinner, TextInput, PageHeader } from '../ui';

export function CredentialsPage() {
  const { t } = useI18n();
  const toast = useToast();
  const d = useDraft();
  const [list, setList] = useState<api.CredentialMeta[] | null>(null);
  const [key, setKey] = useState('');
  const [value, setValue] = useState('');
  const [errs, setErrs] = useState<Record<string, string>>({});
  const [busy, setBusy] = useState(false);

  const load = useCallback(() => {
    api.listCredentials().then((r) => setList(r.credentials), (e) => toast('error', errorText(e, t)));
  }, [toast, t]);
  useEffect(load, [load]);

  const submit = async (e: FormEvent) => {
    e.preventDefault();
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
  const remove = async (k: string) => {
    if (!confirm(t('cred.delete_confirm', { key: k }))) return;
    try {
      await api.deleteCredential(k);
      toast('ok', t('cred.deleted', { key: k }));
      load();
    } catch (x) {
      toast('error', errorText(x, t));
    }
  };
  const usedBy = (ref: string) =>
    d.draft.dashboard.sources.filter((s) => s.auth_token_ref === ref || Object.values(s.header_secret_refs ?? {}).includes(ref)).map((s) => s.id);

  return (
    <div className="page stack">
      <PageHeader icon="key" title={t('cred.title')} subtitle={t('cred.subtitle')} />
      <div className="notice">{t('cred.note')}</div>
      <Card title={t('cred.write')}>
        <form className="form-grid two" onSubmit={submit} autoComplete="off">
          <TextInput label={t('cred.key')} value={key} onChange={(v) => setKey(v.replace(/[^A-Za-z0-9_-]/g, '').slice(0, 15))} error={errs.key} maxLength={15} required />
          <TextInput label={t('cred.value')} type="password" autoComplete="new-password" value={value} onChange={setValue} error={errs.value} maxLength={1024} required />
          <div className="wide">
            <Button type="submit" variant="primary" loading={busy} disabled={!key || !value}>
              {t('cred.write')}
            </Button>
          </div>
        </form>
      </Card>
      <Card title={t('cred.list')} actions={<Button size="sm" onClick={load}>{t('common.refresh')}</Button>}>
        {!list ? (
          <Spinner />
        ) : !list.length ? (
          <Empty>{t('cred.empty')}</Empty>
        ) : (
          <table className="table">
            <thead>
              <tr>
                <th>key</th>
                <th>{t('cred.ref')}</th>
                <th>{t('nav.sources')}</th>
                <th />
              </tr>
            </thead>
            <tbody>
              {list.map((c) => (
                <tr key={c.key}>
                  <td className="mono">{c.key}</td>
                  <td className="mono">{c.ref}</td>
                  <td className="dim">{usedBy(c.ref).join(', ') || '—'}</td>
                  <td style={{ textAlign: 'right' }}>
                    <Button size="sm" onClick={() => setKey(c.key)}>
                      {t('common.edit')}
                    </Button>{' '}
                    <Button size="sm" variant="danger" onClick={() => void remove(c.key)}>
                      {t('common.delete')}
                    </Button>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </Card>
    </div>
  );
}
