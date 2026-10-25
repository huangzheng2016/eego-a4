// Dashboard draft store: applied (device) bundle vs local draft, undo/redo,
// validate/apply with If-Match revision handling, and editor selection.
import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from 'react';
import * as api from '../api';
import { ApiError } from '../api';
import { clone, emptyBundle, parseBundle, serializeBundle } from '../model/bundle';
import type { FieldError } from '../model/errors';
import type { ObservedMetric, ObservedMetrics } from '../model/metrics';
import type { Bundle } from '../model/types';
import { useI18n } from '../i18n';
import { useToast } from './toast';
import { errorText } from './errors';
import { commit, initHistory, redo as redoH, reset, undo as undoH, type History } from './history';

const LOCAL_KEY = 'eego_dashboard_draft';

interface Saved {
  base_revision: number;
  bundle: Bundle;
}

export interface DraftCtx {
  loading: boolean;
  loadError: string | null;
  configured: boolean;
  applied: Bundle | null;
  draft: Bundle;
  dirty: boolean;
  errors: FieldError[];
  validity: 'unknown' | 'valid' | 'invalid';
  busy: 'validate' | 'apply' | null;
  conflict: { remoteRevision: number } | null;
  restorable: Saved | null;
  canUndo: boolean;
  canRedo: boolean;
  observed: ObservedMetrics;
  pageId: string;
  panelId: string | null;
  setPageId: (id: string) => void;
  setPanelId: (id: string | null) => void;
  /** Produce the next draft from a mutable clone. */
  update: (fn: (b: Bundle) => void, coalesceKey?: string) => void;
  replace: (b: Bundle) => void;
  undo: () => void;
  redo: () => void;
  reload: () => Promise<void>;
  validate: () => Promise<boolean>;
  /** Apply the draft (or `override`, which also becomes the draft). */
  apply: (override?: Bundle) => Promise<boolean>;
  discard: () => void;
  resolveConflict: (choice: 'reload' | 'overwrite' | 'cancel') => Promise<void>;
  restore: (yes: boolean) => void;
  recordObserved: (sourceId: string, metrics: ObservedMetric[]) => void;
  /** Increments after every successful apply (drives the live device preview). */
  appliedSeq: number;
  /** Date.now() of the last successful apply (0 = none this session). */
  appliedAt: number;
  /** Show device validation errors (e.g. from render-preview) on the draft. */
  reportErrors: (fields: FieldError[]) => void;
}

const Ctx = createContext<DraftCtx | null>(null);

function readSaved(): Saved | null {
  try {
    const raw = localStorage.getItem(LOCAL_KEY);
    return raw ? (JSON.parse(raw) as Saved) : null;
  } catch {
    return null;
  }
}
function writeSaved(s: Saved | null) {
  try {
    if (s) localStorage.setItem(LOCAL_KEY, JSON.stringify(s));
    else localStorage.removeItem(LOCAL_KEY);
  } catch {
    /* quota or disabled storage: draft persistence is best effort */
  }
}

const same = (a: Bundle | null, b: Bundle | null) =>
  !!a && !!b && JSON.stringify({ ...a, revision: 0 }) === JSON.stringify({ ...b, revision: 0 });

export function DraftProvider({ children }: { children: ReactNode }) {
  const { t } = useI18n();
  const toast = useToast();
  const [loading, setLoading] = useState(true);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [applied, setApplied] = useState<Bundle | null>(null);
  const [revision, setRevision] = useState(0);
  const [hist, setHist] = useState<History<Bundle>>(() => initHistory(emptyBundle()));
  const [errors, setErrors] = useState<FieldError[]>([]);
  const [validity, setValidity] = useState<DraftCtx['validity']>('unknown');
  const [busy, setBusy] = useState<DraftCtx['busy']>(null);
  const [conflict, setConflict] = useState<DraftCtx['conflict']>(null);
  const [restorable, setRestorable] = useState<Saved | null>(null);
  const [observed, setObserved] = useState<ObservedMetrics>({});
  const [pageId, setPageIdState] = useState('');
  const [panelId, setPanelId] = useState<string | null>(null);
  const [appliedSeq, setAppliedSeq] = useState(0);
  const [appliedAt, setAppliedAt] = useState(0);

  const draft = hist.present;
  const dirty = applied ? !same(applied, draft) : true;

  // Keep the selected page valid.
  useEffect(() => {
    if (!draft.dashboard.pages.some((p) => p.id === pageId)) {
      setPageIdState(draft.dashboard.default_page_id && draft.dashboard.pages.some((p) => p.id === draft.dashboard.default_page_id) ? draft.dashboard.default_page_id : draft.dashboard.pages[0]?.id ?? '');
    }
    if (panelId && !draft.dashboard.pages.some((p) => p.panels.some((x) => x.id === panelId))) setPanelId(null);
  }, [draft, pageId, panelId]);

  const setPageId = useCallback((id: string) => {
    setPageIdState(id);
    setPanelId(null);
  }, []);

  const adopt = useCallback((b: Bundle, configured: boolean) => {
    const parsed = parseBundle(b);
    setApplied(configured ? parsed : null);
    setRevision(parsed.revision);
    setHist(reset(configured ? clone(parsed) : { ...emptyBundle(), revision: parsed.revision }));
    setErrors([]);
    setValidity('unknown');
  }, []);

  const reload = useCallback(async () => {
    setLoading(true);
    setLoadError(null);
    try {
      const b = await api.getDashboard();
      adopt(b, true);
      const saved = readSaved();
      if (saved && !same(saved.bundle, parseBundle(b))) setRestorable(saved);
    } catch (e) {
      if (e instanceof ApiError && e.status === 404) {
        const rev = (e.body as { revision?: number } | null)?.revision ?? 0;
        adopt({ ...emptyBundle(), revision: rev }, false);
        const saved = readSaved();
        if (saved) setRestorable(saved);
      } else {
        setLoadError(errorText(e, t));
      }
    } finally {
      setLoading(false);
    }
  }, [adopt, t]);

  useEffect(() => {
    void reload();
  }, [reload]);

  // Persist an unapplied draft locally (debounced).
  const saveTimer = useRef<ReturnType<typeof setTimeout>>();
  useEffect(() => {
    if (loading) return;
    clearTimeout(saveTimer.current);
    saveTimer.current = setTimeout(() => {
      if (dirty && hist.past.length) writeSaved({ base_revision: revision, bundle: draft });
      else if (!dirty) writeSaved(null);
    }, 800);
    return () => clearTimeout(saveTimer.current);
  }, [draft, dirty, revision, loading, hist.past.length]);

  const update = useCallback((fn: (b: Bundle) => void, key?: string) => {
    setHist((h) => {
      const next = clone(h.present);
      fn(next);
      return commit(h, next, key);
    });
    setValidity('unknown');
  }, []);

  const replace = useCallback((b: Bundle) => {
    setHist((h) => commit(h, clone(b)));
    setValidity('unknown');
  }, []);

  const validate = useCallback(async () => {
    setBusy('validate');
    try {
      const r = await api.validateDashboard(serializeBundle(draft));
      setErrors(r.errors ?? []);
      setValidity(r.valid ? 'valid' : 'invalid');
      if (r.valid) toast('ok', t('draft.valid'));
      else toast('error', t('draft.invalid', { n: r.errors?.length ?? 0 }));
      return r.valid;
    } catch (e) {
      toast('error', errorText(e, t));
      return false;
    } finally {
      setBusy(null);
    }
  }, [draft, toast, t]);

  const doApply = useCallback(
    async (rev: number, override?: Bundle) => {
      const body = serializeBundle({ ...(override ?? draft), revision: rev });
      const result = applied ? await api.putDashboard(body, rev) : await api.createDashboard(body);
      adopt(result, true);
      writeSaved(null);
      setAppliedSeq((n) => n + 1);
      setAppliedAt(Date.now());
      toast('ok', t('draft.applied', { rev: result.revision }));
    },
    [draft, applied, adopt, toast, t],
  );

  const apply = useCallback(async (override?: Bundle) => {
    if (override) setHist((h) => commit(h, clone(override)));
    setBusy('apply');
    try {
      await doApply(revision, override);
      return true;
    } catch (e) {
      if (e instanceof ApiError && e.status === 409) {
        let remote = revision;
        try {
          remote = (await api.getDashboard()).revision;
        } catch (e2) {
          if (e2 instanceof ApiError && e2.status === 404) remote = (e2.body as { revision?: number })?.revision ?? revision;
        }
        setConflict({ remoteRevision: remote });
      } else if (e instanceof ApiError && e.code === 'validation_failed') {
        setErrors(e.fields);
        setValidity('invalid');
        toast('error', t('draft.invalid', { n: e.fields.length || 1 }) + (e.fields.length ? '' : `\n${e.message}`));
      } else {
        toast('error', errorText(e, t));
      }
      return false;
    } finally {
      setBusy(null);
    }
  }, [doApply, revision, toast, t]);

  const resolveConflict = useCallback(
    async (choice: 'reload' | 'overwrite' | 'cancel') => {
      const c = conflict;
      setConflict(null);
      if (!c || choice === 'cancel') return;
      if (choice === 'reload') {
        writeSaved(null);
        await reload();
        return;
      }
      setBusy('apply');
      try {
        // Overwrite: the device may have been configured meanwhile, so PUT.
        const body = serializeBundle({ ...draft, revision: c.remoteRevision });
        const result = await api.putDashboard(body, c.remoteRevision);
        adopt(result, true);
        writeSaved(null);
        setAppliedSeq((n) => n + 1);
      setAppliedAt(Date.now());
        toast('ok', t('draft.applied', { rev: result.revision }));
      } catch (e) {
        if (e instanceof ApiError && e.code === 'validation_failed') {
          setErrors(e.fields);
          setValidity('invalid');
        }
        toast('error', errorText(e, t));
      } finally {
        setBusy(null);
      }
    },
    [conflict, draft, reload, adopt, toast, t],
  );

  const discard = useCallback(() => {
    setHist((h) => commit(h, applied ? clone(applied) : { ...emptyBundle(), revision }));
    setErrors([]);
    setValidity('unknown');
    writeSaved(null);
  }, [applied, revision]);

  const restore = useCallback(
    (yes: boolean) => {
      if (yes && restorable) setHist((h) => commit(h, { ...parseBundle(restorable.bundle), revision }));
      if (!yes) writeSaved(null);
      setRestorable(null);
    },
    [restorable, revision],
  );

  const reportErrors = useCallback((fields: FieldError[]) => {
    setErrors(fields);
    setValidity(fields.length ? 'invalid' : 'unknown');
  }, []);

  const recordObserved = useCallback((sourceId: string, metrics: ObservedMetric[]) => {
    setObserved((o) => ({ ...o, [sourceId]: metrics }));
  }, []);

  const value = useMemo<DraftCtx>(
    () => ({
      loading,
      loadError,
      configured: !!applied,
      applied,
      draft,
      dirty,
      errors,
      validity,
      busy,
      conflict,
      restorable,
      canUndo: hist.past.length > 0,
      canRedo: hist.future.length > 0,
      observed,
      pageId,
      panelId,
      setPageId,
      setPanelId,
      update,
      replace,
      undo: () => setHist(undoH),
      redo: () => setHist(redoH),
      reload,
      validate,
      apply,
      discard,
      resolveConflict,
      restore,
      recordObserved,
      appliedSeq,
      appliedAt,
      reportErrors,
    }),
    [loading, loadError, applied, draft, dirty, errors, validity, busy, conflict, restorable, hist.past.length, hist.future.length, observed, pageId, panelId, setPageId, update, replace, reload, validate, apply, discard, resolveConflict, restore, recordObserved, appliedSeq, appliedAt, reportErrors],
  );
  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}

export function useDraft(): DraftCtx {
  const c = useContext(Ctx);
  if (!c) throw new Error('DraftProvider missing');
  return c;
}
