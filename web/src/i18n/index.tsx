// Minimal i18n: one flat dictionary per language, Chinese first.
// Terminology (keep consistent): 仪表盘 dashboard · 页面 page · 面板 panel ·
// 数据源 source · 指标 metric · 凭据 credential · 草稿 draft · 已应用 applied ·
// 校验 validate · 应用 apply · 模板 template · 屏 screen.
import { createContext, useCallback, useContext, useMemo, useState, type ReactNode } from 'react';
import { zh } from './zh';
import { en } from './en';

export type Lang = 'zh' | 'en';
export type DictKey = keyof typeof zh;
const DICTS: Record<Lang, Record<DictKey, string>> = { zh, en };

function initialLang(): Lang {
  try {
    const saved = localStorage.getItem('eego_lang');
    if (saved === 'zh' || saved === 'en') return saved;
  } catch {
    /* ignore */
  }
  return 'zh';
}

export type TFn = (key: DictKey, vars?: Record<string, string | number>) => string;

interface I18nCtx {
  lang: Lang;
  setLang: (l: Lang) => void;
  t: TFn;
}

const Ctx = createContext<I18nCtx | null>(null);

export function translate(lang: Lang, key: DictKey, vars?: Record<string, string | number>): string {
  let s = DICTS[lang][key] ?? DICTS.zh[key] ?? key;
  if (vars) for (const [k, v] of Object.entries(vars)) s = s.split(`{${k}}`).join(String(v));
  return s;
}

export function I18nProvider({ children }: { children: ReactNode }) {
  const [lang, setLangState] = useState<Lang>(initialLang);
  const setLang = useCallback((l: Lang) => {
    setLangState(l);
    try {
      localStorage.setItem('eego_lang', l);
    } catch {
      /* ignore */
    }
    document.documentElement.lang = l === 'zh' ? 'zh-CN' : 'en';
  }, []);
  const t = useCallback<TFn>((key, vars) => translate(lang, key, vars), [lang]);
  const value = useMemo(() => ({ lang, setLang, t }), [lang, setLang, t]);
  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}

export function useI18n(): I18nCtx {
  const c = useContext(Ctx);
  if (!c) throw new Error('I18nProvider missing');
  return c;
}

/** Label for an enum value: looks up `${prefix}.${value}` and falls back to the raw value. */
export function useEnumLabel() {
  const { lang } = useI18n();
  return useCallback(
    (prefix: string, value: string) => {
      const key = `${prefix}.${value}` as DictKey;
      return DICTS[lang][key] ?? DICTS.zh[key] ?? value;
    },
    [lang],
  );
}
