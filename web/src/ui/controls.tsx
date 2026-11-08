// Small shared controls used by the editor-group pages.
import { useEffect, useState, type ReactNode } from 'react';
import { Button, Icon, TextInput } from './index';

/** ‹ label › pager row. */
export function Pager({ label, onPrev, onNext, prevDisabled, nextDisabled, prevLabel, nextLabel, children }: {
  label: ReactNode;
  onPrev: () => void;
  onNext: () => void;
  prevDisabled?: boolean;
  nextDisabled?: boolean;
  prevLabel: string;
  nextLabel: string;
  children?: ReactNode;
}) {
  return (
    <div className="row pager">
      <Button size="sm" icon disabled={prevDisabled} onClick={onPrev} aria-label={prevLabel} title={prevLabel}>
        <Icon name="chevl" size={16} />
      </Button>
      <span className="muted pager-label">
        {label}
      </span>
      <Button size="sm" icon disabled={nextDisabled} onClick={onNext} aria-label={nextLabel} title={nextLabel}>
        <Icon name="chevr" size={16} />
      </Button>
      {children}
    </div>
  );
}

/** Text input with a leading search icon. */
export function SearchInput({ value, onChange, placeholder, width = 200 }: { value: string; onChange: (v: string) => void; placeholder: string; width?: number }) {
  return (
    <div className="search-input">
      <input
        className="input"
        type="search"
        style={{ width }}
        placeholder={placeholder}
        aria-label={placeholder}
        value={value}
        onChange={(e) => onChange(e.target.value)}
        onKeyDown={(e) => e.key === 'Escape' && value && (e.preventDefault(), onChange(''))}
      />
      <span className="search-icon">
        <Icon name="search" size={16} />
      </span>
    </div>
  );
}

/**
 * Identifier field: keeps the typed text locally and commits on blur / Enter,
 * so clearing the field or passing through a taken id doesn't freeze the input
 * or remount the editor on every keystroke. Esc reverts.
 */
export function IdInput({ label, value, error, validate, onCommit, wide, maxLength = 64 }: {
  label: ReactNode;
  maxLength?: number;
  value: string;
  error?: string;
  /** Returns an error message for a candidate id, or undefined when OK. */
  validate: (id: string) => string | undefined;
  onCommit: (id: string) => void;
  wide?: boolean;
}) {
  const [text, setText] = useState(value);
  const [touched, setTouched] = useState(false);
  useEffect(() => setText(value), [value]);
  const clean = text.replace(/[^A-Za-z0-9_-]/g, '');
  const local = clean === value ? undefined : validate(clean);
  const commit = () => {
    setTouched(true);
    if (clean === value) return setText(value);
    if (!local) onCommit(clean);
  };
  return (
    <TextInput
      wide={wide}
      label={label}
      value={text}
      maxLength={maxLength}
      spellCheck={false}
      error={(touched && local) || error}
      onChange={(v) => {
        setTouched(false);
        setText(v.replace(/[^A-Za-z0-9_-]/g, ''));
      }}
      onBlur={commit}
      onKeyDown={(e) => {
        if (e.key === 'Enter') commit();
        else if (e.key === 'Escape') {
          setText(value);
          setTouched(false);
        }
      }}
    />
  );
}
