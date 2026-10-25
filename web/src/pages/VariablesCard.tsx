// Dashboard variables (dashboard.variables) referenced as ${name} in source
// URLs, headers and request bodies. Rules follow dashboard_core validate.cpp
// collectVariables(): name [A-Za-z_][A-Za-z0-9_]{0,63}, query variables need a
// source + value path, locale/time_range defaults are checked.
import { useState } from 'react';
import { useI18n, type DictKey } from '../i18n';
import { useDraft } from '../state/draft';
import { VARIABLE_KINDS, type Bundle, type Variable, type VariableKind } from '../model/types';
import { Badge, Button, Card, Checkbox, Icon, Select, TextInput } from '../ui';

const NAME_RE = /^[A-Za-z_][A-Za-z0-9_]{0,63}$/;
const TIME_RE = /^\s*(now|(now-|-)?\d+(\.\d+)?(ms|s|m|h|d|w))\s*$/;
const LOCALE_RE = /^[A-Za-z0-9_-]{2,32}$/;

/** Sources whose URL/headers/body mention ${name}. */
export function variableUsers(b: Bundle, name: string): string[] {
  const needle = '${' + name + '}';
  return b.source_plans
    .filter((p) => [p.url ?? '', p.body ?? '', ...Object.values(p.headers ?? {})].some((s) => s.includes(needle)))
    .map((p) => p.id);
}

export function variableErrors(vars: Variable[], i: number, sourceIds: string[]): Partial<Record<keyof Variable, DictKey>> {
  const v = vars[i];
  const out: Partial<Record<keyof Variable, DictKey>> = {};
  if (!NAME_RE.test(v.name)) out.name = 'var.err.name';
  else if (vars.some((o, j) => j < i && o.name === v.name)) out.name = 'var.err.dup';
  if (v.kind === 'query') {
    if (!v.source_id || !sourceIds.includes(v.source_id)) out.source_id = 'var.err.source';
    if (!v.value_path) out.value_path = 'var.err.path';
  }
  if (v.kind === 'time_range' && v.default_value && !TIME_RE.test(v.default_value)) out.default_value = 'var.err.time';
  if (v.kind === 'locale' && !LOCALE_RE.test(v.default_value ?? '')) out.default_value = 'var.err.locale';
  return out;
}

export function VariablesCard() {
  const { t } = useI18n();
  const d = useDraft();
  const vars = d.draft.dashboard.variables;
  const sourceIds = d.draft.dashboard.sources.map((s) => s.id);
  const [open, setOpen] = useState<number | null>(null);
  const set = (i: number, fn: (v: Variable) => void, key: string) =>
    d.update((b) => {
      const v = b.dashboard.variables[i];
      if (v) fn(v);
    }, `var${i}${key}`);

  const add = () => {
    const names = vars.map((v) => v.name);
    let n = 1;
    while (names.includes(`var${n}`)) n++;
    d.update((b) => b.dashboard.variables.push({ name: `var${n}`, label: '', kind: 'text', default_value: '', options: [], source_id: '', value_path: '', multi: false, include_all: false }));
    setOpen(vars.length);
  };

  return (
    <Card
      title={t('var.title')}
      actions={
        <Button size="sm" onClick={add}>
          <Icon name="plus" /> {t('var.add')}
        </Button>
      }
    >
      <p className="dim card-lead">{t('var.lead')}</p>
      {!vars.length && <div className="dim">{t('var.empty')}</div>}
      <div className="col">
        {vars.map((v, i) => {
          const errs = variableErrors(vars, i, sourceIds);
          const users = variableUsers(d.draft, v.name);
          const expanded = open === i || Object.keys(errs).length > 0;
          return (
            <div key={i} className="subcard">
              <div className="row nowrap var-head" role="button" tabIndex={0} onClick={() => setOpen(expanded && open === i ? null : i)} onKeyDown={(e) => e.key === 'Enter' && setOpen(open === i ? null : i)}>
                <code className="expr">{'${' + v.name + '}'}</code>
                <span className="grow truncate">{v.label || <span className="dim">{t(`var.kind.${v.kind}` as DictKey)}</span>}</span>
                <span className="dim truncate var-default">= {v.default_value || '""'}</span>
                {users.length > 0 ? <Badge tone="accent">{t('var.used_by', { list: users.join(', ') })}</Badge> : <Badge>{t('var.unused')}</Badge>}
                <Icon name={expanded ? 'chevd' : 'chevr'} size={15} />
              </div>
              {expanded && (
                <div className="form-grid two var-body">
                  <TextInput label={t('var.name')} value={v.name} maxLength={64} spellCheck={false} error={errs.name && t(errs.name)} hint={t('var.name_hint')} onChange={(x) => set(i, (o) => (o.name = x.replace(/[^A-Za-z0-9_]/g, '')), 'n')} />
                  <TextInput label={t('var.label')} value={v.label} maxLength={160} onChange={(x) => set(i, (o) => (o.label = x), 'l')} />
                  <Select
                    label={t('var.kind')}
                    hint={t(`var.kind_h.${v.kind}` as DictKey)}
                    value={v.kind}
                    options={VARIABLE_KINDS.map((k) => ({ value: k, label: t(`var.kind.${k}` as DictKey) }))}
                    onChange={(x) =>
                      set(i, (o) => {
                        o.kind = x as VariableKind;
                        if (o.kind !== 'query') {
                          o.source_id = '';
                          o.value_path = '';
                        }
                        if (o.kind === 'locale' && !LOCALE_RE.test(o.default_value ?? '')) o.default_value = 'zh-CN';
                      }, 'k')
                    }
                  />
                  <TextInput
                    label={t('var.default')}
                    value={v.default_value}
                    maxLength={512}
                    spellCheck={false}
                    placeholder={v.kind === 'time_range' ? 'now-1h' : v.kind === 'locale' ? 'zh-CN' : ''}
                    error={errs.default_value && t(errs.default_value)}
                    hint={t('var.default_hint')}
                    onChange={(x) => set(i, (o) => (o.default_value = x), 'd')}
                  />
                  {v.kind === 'query' && (
                    <>
                      <Select label={t('var.source')} value={v.source_id ?? ''} error={errs.source_id && t(errs.source_id)} options={[{ value: '', label: '—' }, ...sourceIds.map((s) => ({ value: s, label: s }))]} onChange={(x) => set(i, (o) => (o.source_id = x), 's')} />
                      <TextInput label={t('var.value_path')} value={v.value_path} placeholder="$.items[*].name" spellCheck={false} maxLength={256} error={errs.value_path && t(errs.value_path)} onChange={(x) => set(i, (o) => (o.value_path = x), 'p')} />
                      <Checkbox label={t('var.multi')} checked={v.multi} onChange={(x) => set(i, (o) => (o.multi = x), 'm')} />
                      <Checkbox label={t('var.include_all')} checked={v.include_all} onChange={(x) => set(i, (o) => (o.include_all = x), 'a')} />
                    </>
                  )}
                  {(v.kind === 'text' || v.kind === 'constant') && (
                    <div className="wide">
                      <span className="label-sm">{t('var.options')}</span>
                      <div className="col">
                        {(v.options ?? []).map((o, oi) => (
                          <div key={oi} className="kv-row">
                            <input className="input" aria-label={t('var.option_label')} placeholder={t('var.option_label')} value={o.label} maxLength={160} onChange={(e) => set(i, (x) => (x.options![oi].label = e.target.value), `o${oi}l`)} />
                            <input className="input" aria-label={t('var.option_value')} placeholder={t('var.option_value')} value={o.value} maxLength={512} onChange={(e) => set(i, (x) => (x.options![oi].value = e.target.value), `o${oi}v`)} />
                            <Button icon aria-label={t('common.delete')} onClick={() => set(i, (x) => x.options!.splice(oi, 1), 'od')}>
                              <Icon name="x" />
                            </Button>
                          </div>
                        ))}
                        <div className="row">
                          <Button size="sm" disabled={(v.options?.length ?? 0) >= 64} onClick={() => set(i, (x) => (x.options = [...(x.options ?? []), { label: '', value: '' }]), 'oa')}>
                            <Icon name="plus" /> {t('var.add_option')}
                          </Button>
                          <span className="hint">{t('var.options_hint')}</span>
                        </div>
                      </div>
                    </div>
                  )}
                  <div className="wide row">
                    <span className="spacer" />
                    <Button size="sm" variant="danger" onClick={() => d.update((b) => b.dashboard.variables.splice(i, 1))}>
                      {t('common.delete')}
                    </Button>
                  </div>
                </div>
              )}
            </div>
          );
        })}
      </div>
    </Card>
  );
}
