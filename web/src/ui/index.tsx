// Small UI primitives. Keep these dependency-free.
import { Icon } from './icons';
import {
  useEffect,
  useId,
  useRef,
  useState,
  type ButtonHTMLAttributes,
  type CSSProperties,
  type InputHTMLAttributes,
  type ReactNode,
  type SelectHTMLAttributes,
  type TextareaHTMLAttributes,
} from 'react';

type BtnVariant = 'default' | 'primary' | 'danger' | 'ghost';
export function Button({
  variant = 'default',
  size,
  icon,
  loading,
  className,
  children,
  ...rest
}: ButtonHTMLAttributes<HTMLButtonElement> & { variant?: BtnVariant; size?: 'sm'; icon?: boolean; loading?: boolean }) {
  const cls = ['btn', variant !== 'default' ? variant : '', size ?? '', icon ? 'icon' : '', className ?? ''].filter(Boolean).join(' ');
  return (
    <button type="button" className={cls} {...rest} disabled={rest.disabled || loading}>
      {loading && <span className="spin" aria-hidden />}
      {children}
    </button>
  );
}

export function Field({
  label,
  error,
  hint,
  children,
  wide,
  htmlFor,
}: {
  label?: ReactNode;
  error?: string;
  hint?: ReactNode;
  children: ReactNode;
  wide?: boolean;
  htmlFor?: string;
}) {
  return (
    <div className={`field${error ? ' has-error' : ''}${wide ? ' wide' : ''}`}>
      {label !== undefined && (
        <label className="label" htmlFor={htmlFor}>
          {label}
        </label>
      )}
      {children}
      {error ? <span className="err-text">{error}</span> : hint ? <span className="hint">{hint}</span> : null}
    </div>
  );
}

interface BaseProps {
  label?: ReactNode;
  error?: string;
  hint?: ReactNode;
  wide?: boolean;
}

export function TextInput({
  label,
  error,
  hint,
  wide,
  value,
  onChange,
  list,
  ...rest
}: BaseProps & Omit<InputHTMLAttributes<HTMLInputElement>, 'value' | 'onChange'> & { value: string | undefined; onChange: (v: string) => void }) {
  const id = useId();
  return (
    <Field label={label} error={error} hint={hint} wide={wide} htmlFor={id}>
      <input id={id} className="input" value={value ?? ''} onChange={(e) => onChange(e.target.value)} list={list} {...rest} />
    </Field>
  );
}

/**
 * Number input that keeps the user's text while typing and only commits
 * finite numbers. `allowEmpty` commits undefined for an empty field.
 */
export function NumberInput({
  label,
  error,
  hint,
  wide,
  value,
  onChange,
  allowEmpty,
  ...rest
}: BaseProps &
  Omit<InputHTMLAttributes<HTMLInputElement>, 'value' | 'onChange'> & {
    value: number | null | undefined;
    onChange: (v: number | undefined) => void;
    allowEmpty?: boolean;
  }) {
  const id = useId();
  const [text, setText] = useState(value == null ? '' : String(value));
  const last = useRef(value);
  useEffect(() => {
    if (value !== last.current) {
      last.current = value;
      setText(value == null ? '' : String(value));
    }
  }, [value]);
  return (
    <Field label={label} error={error} hint={hint} wide={wide} htmlFor={id}>
      <input
        id={id}
        className="input"
        inputMode="decimal"
        type="number"
        value={text}
        onChange={(e) => {
          setText(e.target.value);
          if (e.target.value.trim() === '') {
            if (allowEmpty) {
              last.current = undefined;
              onChange(undefined);
            }
            return;
          }
          const n = Number(e.target.value);
          if (Number.isFinite(n)) {
            last.current = n;
            onChange(n);
          }
        }}
        {...rest}
      />
    </Field>
  );
}

export function TextArea({
  label,
  error,
  hint,
  wide = true,
  value,
  onChange,
  ...rest
}: BaseProps & Omit<TextareaHTMLAttributes<HTMLTextAreaElement>, 'value' | 'onChange'> & { value: string; onChange: (v: string) => void }) {
  const id = useId();
  return (
    <Field label={label} error={error} hint={hint} wide={wide} htmlFor={id}>
      <textarea id={id} className="input" value={value} onChange={(e) => onChange(e.target.value)} spellCheck={false} {...rest} />
    </Field>
  );
}

export interface Option {
  value: string;
  label: string;
  disabled?: boolean;
}
export function Select({
  label,
  error,
  hint,
  wide,
  value,
  onChange,
  options,
  ...rest
}: BaseProps & Omit<SelectHTMLAttributes<HTMLSelectElement>, 'value' | 'onChange'> & { value: string | undefined; onChange: (v: string) => void; options: Option[] }) {
  const id = useId();
  const has = options.some((o) => o.value === (value ?? ''));
  return (
    <Field label={label} error={error} hint={hint} wide={wide} htmlFor={id}>
      <select id={id} className="input" value={value ?? ''} onChange={(e) => onChange(e.target.value)} {...rest}>
        {!has && <option value={value ?? ''}>{value || '—'}</option>}
        {options.map((o) => (
          <option key={o.value} value={o.value} disabled={o.disabled}>
            {o.label}
          </option>
        ))}
      </select>
    </Field>
  );
}

export function Checkbox({
  label,
  checked,
  onChange,
  disabled,
  error,
  hint,
}: {
  label: ReactNode;
  checked: boolean | undefined;
  onChange: (v: boolean) => void;
  disabled?: boolean;
  error?: string;
  hint?: ReactNode;
}) {
  return (
    <div className={`field${error ? ' has-error' : ''}`}>
      <label className="check">
        <input type="checkbox" checked={!!checked} disabled={disabled} onChange={(e) => onChange(e.target.checked)} />
        <span>{label}</span>
      </label>
      {error ? <span className="err-text">{error}</span> : hint ? <span className="hint">{hint}</span> : null}
    </div>
  );
}

export function Modal({
  title,
  children,
  footer,
  onClose,
  size,
}: {
  title: ReactNode;
  children: ReactNode;
  footer?: ReactNode;
  onClose: () => void;
  size?: 'lg';
}) {
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => e.key === 'Escape' && onClose();
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [onClose]);
  return (
    <div className="modal-scrim" onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div className={`modal ${size ?? ''}`} role="dialog" aria-modal="true">
        <div className="modal-head">
          <h2 className="grow">{title}</h2>
          <button type="button" className="btn ghost icon sm" aria-label="close" onClick={onClose}>
            <Icon name="x" size={16} />
          </button>
        </div>
        <div className="modal-body">{children}</div>
        {footer && <div className="modal-foot">{footer}</div>}
      </div>
    </div>
  );
}

export function Card({ title, actions, children, className }: { title?: ReactNode; actions?: ReactNode; children: ReactNode; className?: string }) {
  return (
    <section className={`card ${className ?? ''}`}>
      {(title || actions) && (
        <div className="card-head">
          {title && <h3 className="grow">{title}</h3>}
          {actions}
        </div>
      )}
      <div className="card-body">{children}</div>
    </section>
  );
}

export function Badge({ tone, children, dot }: { tone?: 'ok' | 'warn' | 'danger' | 'accent'; children: ReactNode; dot?: boolean }) {
  return (
    <span className={`badge ${tone ?? ''}`}>
      {dot && <i className="badge-dot" />}
      {children}
    </span>
  );
}

export function Spinner() {
  return <span className="spin" aria-label="loading" />;
}

export function Empty({ children }: { children: ReactNode }) {
  return <div className="empty">{children}</div>;
}

/** Illustrated empty state with an optional call to action. */
export function EmptyState({ icon = 'sparkles', title, children, action }: { icon?: string; title: ReactNode; children?: ReactNode; action?: ReactNode }) {
  return (
    <div className="empty-state">
      <div className="empty-icon">
        <Icon name={icon} size={26} />
      </div>
      <div className="empty-title">{title}</div>
      {children && <div className="empty-desc">{children}</div>}
      {action && <div className="row" style={{ justifyContent: 'center', marginTop: 4 }}>{action}</div>}
    </div>
  );
}

/** Shimmering placeholder blocks while data loads. */
export function Skeleton({ w = '100%', h = 14, r, style }: { w?: number | string; h?: number | string; r?: number; style?: CSSProperties }) {
  return <span className="skeleton" style={{ width: w, height: h, borderRadius: r, ...style }} aria-hidden />;
}
export function SkeletonCard({ lines = 3 }: { lines?: number }) {
  return (
    <div className="card card-body col" aria-busy="true">
      <Skeleton w="40%" h={16} />
      {Array.from({ length: lines }, (_, i) => (
        <Skeleton key={i} w={`${90 - i * 12}%`} />
      ))}
    </div>
  );
}

export function PageHeader({ title, subtitle, icon, actions }: { title: ReactNode; subtitle?: ReactNode; icon?: string; actions?: ReactNode }) {
  return (
    <header className="page-head">
      {icon && (
        <span className="page-icon">
          <Icon name={icon} size={20} />
        </span>
      )}
      <div className="grow">
        <h1>{title}</h1>
        {subtitle && <p className="page-sub">{subtitle}</p>}
      </div>
      {actions && <div className="row page-actions">{actions}</div>}
    </header>
  );
}

export function Segmented<T extends string>({ value, options, onChange, size, label }: { value: T; options: { value: T; label: ReactNode; icon?: string; title?: string }[]; onChange: (v: T) => void; size?: 'sm'; label?: string }) {
  return (
    <div className={`segmented${size ? ' ' + size : ''}`} role="radiogroup" aria-label={label}>
      {options.map((o) => (
        <button key={o.value} type="button" role="radio" aria-checked={o.value === value} title={o.title} className={o.value === value ? 'on' : ''} onClick={() => onChange(o.value)}>
          {o.icon && <Icon name={o.icon} size={15} />}
          {o.label}
        </button>
      ))}
    </div>
  );
}

export function Switch({ checked, onChange, label, disabled, hint }: { checked: boolean | undefined; onChange: (v: boolean) => void; label?: ReactNode; disabled?: boolean; hint?: ReactNode }) {
  return (
    <label className={`switch-row${disabled ? ' disabled' : ''}`}>
      <span className="switch">
        <input type="checkbox" role="switch" checked={!!checked} disabled={disabled} onChange={(e) => onChange(e.target.checked)} />
        <span className="switch-track" />
      </span>
      {label && (
        <span className="col" style={{ gap: 0 }}>
          <span>{label}</span>
          {hint && <span className="dim">{hint}</span>}
        </span>
      )}
    </label>
  );
}

export { Icon, Logo } from './icons';

/** Editable list of string key/value pairs (headers, label maps). */
export function KeyValueEditor({
  value,
  onChange,
  keyLabel,
  valueLabel,
  addLabel,
  renderValue,
}: {
  value: Record<string, string> | undefined;
  onChange: (v: Record<string, string>) => void;
  keyLabel: string;
  valueLabel: string;
  addLabel: string;
  renderValue?: (v: string, set: (v: string) => void) => ReactNode;
}) {
  // Keep rows as an array so empty/duplicate keys can be edited transiently.
  const [rows, setRows] = useState<[string, string][]>(() => Object.entries(value ?? {}));
  const sig = JSON.stringify(value ?? {});
  const lastSig = useRef(sig);
  useEffect(() => {
    if (sig !== lastSig.current) {
      lastSig.current = sig;
      setRows(Object.entries(JSON.parse(sig) as Record<string, string>));
    }
  }, [sig]);
  const commit = (next: [string, string][]) => {
    setRows(next);
    const obj: Record<string, string> = {};
    for (const [k, v] of next) if (k) obj[k] = v;
    lastSig.current = JSON.stringify(obj);
    onChange(obj);
  };
  return (
    <div className="col">
      {rows.map(([k, v], i) => (
        <div className="kv-row" key={i}>
          <input className="input" placeholder={keyLabel} aria-label={keyLabel} value={k} onChange={(e) => commit(rows.map((r, j) => (j === i ? [e.target.value, r[1]] : r)))} />
          {renderValue ? (
            renderValue(v, (nv) => commit(rows.map((r, j) => (j === i ? [r[0], nv] : r))))
          ) : (
            <input className="input" placeholder={valueLabel} aria-label={valueLabel} value={v} onChange={(e) => commit(rows.map((r, j) => (j === i ? [r[0], e.target.value] : r)))} />
          )}
          <Button icon aria-label="remove" onClick={() => commit(rows.filter((_, j) => j !== i))}>
            <Icon name="x" />
          </Button>
        </div>
      ))}
      <div>
        <Button size="sm" onClick={() => setRows([...rows, ['', '']])}>
          <Icon name="plus" /> {addLabel}
        </Button>
      </div>
    </div>
  );
}
