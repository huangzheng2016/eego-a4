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
  // Field errors show after the field was left (or after a device validation
  // attempt), never on a freshly added / switched variable.
  const [touched, setTouched] = useState<Set<string>>(() => new Set());
  const touch = (i: number, f: string) => () => setTouched((s) => (s.has(`${i}:${f}`) ? s : new Set(s).add(`${i}:${f}`)));
  const shown = (i: number, f: string) => d.validity === 'invalid' || touched.has(`${i}:${f}`);
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
      {!vars.length && <div className="dim">{t('var.empty')}</div>}
      <div className="col">
        {vars.map((v, i) => {
          const errs = variableErrors(vars, i, sourceIds);
          const users = variableUsers(d.draft, v.name);
          const expanded = open === i;
          const nErr = Object.keys(errs).length;
          const fe = (f: keyof Variable) => (errs[f] && shown(i, f) ? t(errs[f]!) : undefined);
          const toggle = () => setOpen(expanded ? null : i);
          return (
            <div key={i} className="var-row">
              <div className="row nowrap var-head" role="button" tabIndex={0} aria-expanded={expanded} onClick={toggle} onKeyDown={(e) => (e.key === 'Enter' || e.key === ' ') && (e.preventDefault(), toggle())}>
                <code className="expr">{'${' + v.name + '}'}</code>
                <span className="grow truncate">{v.label || <span className="dim">{t(`var.kind.${v.kind}` as DictKey)}</span>}</span>
                {v.default_value && <span className="dim truncate var-default">= {v.default_value}</span>}
                {users.length > 0 && <Badge tone="accent">{t('var.used_by', { list: users.join(', ') })}</Badge>}
                {nErr > 0 && <Badge tone="danger">{t('var.err_count', { n: nErr })}</Badge>}
                <Icon name={expanded ? 'chevd' : 'chevr'} size={15} />
              </div>
              {expanded && (
                <div className="form-grid two var-body">
                  <TextInput label={t('var.name')} value={v.name} maxLength={64} spellCheck={false} error={fe('name')} onBlur={touch(i, 'name')} hint={t('var.name_hint')} onChange={(x) => set(i, (o) => (o.name = x.replace(/[^A-Za-z0-9_]/g, '')), 'n')} />
                  <TextInput label={t('var.label')} value={v.label} maxLength={160} onChange={(x) => set(i, (o) => (o.label = x), 'l')} />
                  <Select
                    label={t('var.kind')}
                    hint={v.kind === 'text' ? undefined : t(`var.kind_h.${v.kind}` as DictKey)}
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
                    error={fe('default_value')}
                    onBlur={touch(i, 'default_value')}
                    onChange={(x) => set(i, (o) => (o.default_value = x), 'd')}
                  />
                  {v.kind === 'query' && (
                    <>
                      <Select label={t('var.source')} value={v.source_id ?? ''} error={fe('source_id')} onBlur={touch(i, 'source_id')} options={[{ value: '', label: '—' }, ...sourceIds.map((s) => ({ value: s, label: s }))]} onChange={(x) => set(i, (o) => (o.source_id = x), 's')} />
                      <TextInput label={t('var.value_path')} value={v.value_path} placeholder="$.items[*].name" spellCheck={false} maxLength={256} error={fe('value_path')} onBlur={touch(i, 'value_path')} onChange={(x) => set(i, (o) => (o.value_path = x), 'p')} />
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
                            <Button icon variant="ghost" aria-label={t('common.delete')} title={t('common.delete')} onClick={() => set(i, (x) => x.options!.splice(oi, 1), 'od')}>
                              <Icon name="x" />
                            </Button>
                          </div>
                        ))}
                        <div className="row">
                          <Button
                            size="sm"
                            disabled={(v.options?.length ?? 0) >= 64}
                            title={(v.options?.length ?? 0) >= 64 ? t('var.options_full', { n: 64 }) : undefined}
                            onClick={() => set(i, (x) => (x.options = [...(x.options ?? []), { label: '', value: '' }]), 'oa')}
                          >
                            <Icon name="plus" /> {t('var.add_option')}
                          </Button>
                        </div>
                      </div>
                    </div>
                  )}
                  <div className="wide row">
                    <span className="spacer" />
                    <Button
                      size="sm"
                      variant="ghost"
                      onClick={() => {
                        d.update((b) => b.dashboard.variables.splice(i, 1));
                        setOpen(null);
                        setTouched(new Set());
                      }}
                    >
                      <Icon name="trash" size={15} /> {t('common.delete')}
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
