// Small UI primitives. Keep these dependency-free.
import {
  useEffect,
  useId,
  useRef,
  useState,
  type ButtonHTMLAttributes,
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

export function Checkbox({ label, checked, onChange, disabled, error }: { label: ReactNode; checked: boolean | undefined; onChange: (v: boolean) => void; disabled?: boolean; error?: string }) {
  return (
    <div className={`field${error ? ' has-error' : ''}`}>
      <label className="check">
        <input type="checkbox" checked={!!checked} disabled={disabled} onChange={(e) => onChange(e.target.checked)} />
        <span>{label}</span>
      </label>
      {error && <span className="err-text">{error}</span>}
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
          <h2>{title}</h2>
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

export function Badge({ tone, children }: { tone?: 'ok' | 'warn' | 'danger' | 'accent'; children: ReactNode }) {
  return <span className={`badge ${tone ?? ''}`}>{children}</span>;
}

export function Spinner() {
  return <span className="spin" aria-label="loading" />;
}

export function Empty({ children }: { children: ReactNode }) {
  return <div className="empty">{children}</div>;
}

/** Minimal inline SVG icons (stroke-based, 16px). */
const ICONS: Record<string, string> = {
  undo: 'M4 7h7a4 4 0 0 1 0 8H7M4 7l3-3M4 7l3 3',
  redo: 'M12 7H5a4 4 0 0 0 0 8h4M12 7L9 4m3 3L9 10',
  plus: 'M8 3v10M3 8h10',
  trash: 'M3 4h10M6 4V2.5h4V4M4.5 4l.7 9.5h5.6l.7-9.5',
  copy: 'M5 5h8v8H5zM3 11V3h8',
  up: 'M8 13V3M4 7l4-4 4 4',
  down: 'M8 3v10M4 9l4 4 4-4',
  menu: 'M2 4h12M2 8h12M2 12h12',
  check: 'M3 8.5l3 3 7-7',
  x: 'M4 4l8 8M12 4l-8 8',
  grid: 'M2 2h12v12H2zM2 6h12M2 10h12M6 2v12M10 2v12',
  layout: 'M2 2h12v12H2zM2 7h12M8 7v7',
  db: 'M3 4c0-1.1 2.2-2 5-2s5 .9 5 2-2.2 2-5 2-5-.9-5-2zm0 0v8c0 1.1 2.2 2 5 2s5-.9 5-2V4M3 8c0 1.1 2.2 2 5 2s5-.9 5-2',
  key: 'M10 2a4 4 0 1 0 0 8 4 4 0 0 0 0-8zM7 8l-5 5v1.5h2v-1.5h1.5V11H7',
  gear: 'M8 5.5a2.5 2.5 0 1 0 0 5 2.5 2.5 0 0 0 0-5zM8 1v2M8 13v2M1 8h2M13 8h2M3 3l1.4 1.4M11.6 11.6L13 13M3 13l1.4-1.4M11.6 4.4L13 3',
  bell: 'M4 11V7a4 4 0 0 1 8 0v4l1.5 1.5h-11zM6.5 14h3',
  cpu: 'M4 4h8v8H4zM6 1v3M10 1v3M6 12v3M10 12v3M1 6h3M1 10h3M12 6h3M12 10h3',
  eye: 'M1 8s2.5-5 7-5 7 5 7 5-2.5 5-7 5-7-5-7-5zM8 6a2 2 0 1 0 0 4 2 2 0 0 0 0-4z',
  play: 'M5 3l8 5-8 5z',
  download: 'M8 2v8M4.5 6.5L8 10l3.5-3.5M3 13h10',
  upload: 'M8 10V2M4.5 5.5L8 2l3.5 3.5M3 13h10',
  sun: 'M8 5a3 3 0 1 0 0 6 3 3 0 0 0 0-6zM8 1v1.5M8 13.5V15M1 8h1.5M13.5 8H15',
  globe: 'M8 1.5a6.5 6.5 0 1 0 0 13 6.5 6.5 0 0 0 0-13zM1.5 8h13M8 1.5c2 2 2.5 4 2.5 6.5S10 12.5 8 14.5C6 12.5 5.5 10.5 5.5 8S6 3.5 8 1.5',
  logout: 'M6 2H3v12h3M10 5l3 3-3 3M13 8H6',
  zoomin: 'M7 2a5 5 0 1 0 0 10A5 5 0 0 0 7 2zM14 14l-3.5-3.5M5 7h4M7 5v4',
  zoomout: 'M7 2a5 5 0 1 0 0 10A5 5 0 0 0 7 2zM14 14l-3.5-3.5M5 7h4',
  fetch: 'M2 8a6 6 0 0 1 10.5-4M14 8a6 6 0 0 1-10.5 4M12.5 1v3h-3M3.5 15v-3h3',
};
export function Icon({ name, size = 16 }: { name: keyof typeof ICONS | string; size?: number }) {
  return (
    <svg width={size} height={size} viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" aria-hidden>
      <path d={ICONS[name] ?? ''} />
    </svg>
  );
}

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
