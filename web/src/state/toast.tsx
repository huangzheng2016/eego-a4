import { createContext, useCallback, useContext, useMemo, useRef, useState, type ReactNode } from 'react';
import { Button, Icon } from '../ui';

export type ToastKind = 'info' | 'ok' | 'warn' | 'error';
interface Toast {
  id: number;
  kind: ToastKind;
  text: string;
}
type Push = (kind: ToastKind, text: string, ms?: number) => void;
const Ctx = createContext<Push>(() => undefined);

export function ToastProvider({ children }: { children: ReactNode }) {
  const [items, setItems] = useState<Toast[]>([]);
  const seq = useRef(0);
  const dismiss = useCallback((id: number) => setItems((l) => l.filter((t) => t.id !== id)), []);
  const push = useCallback<Push>(
    (kind, text, ms) => {
      const id = ++seq.current;
      setItems((l) => [...l.filter((t) => t.text !== text), { id, kind, text }].slice(-5));
      setTimeout(() => dismiss(id), ms ?? (kind === 'error' ? 8000 : 3500));
    },
    [dismiss],
  );
  const value = useMemo(() => push, [push]);
  return (
    <Ctx.Provider value={value}>
      {children}
      <div className="toasts" role="status" aria-live="polite">
        {items.map((t) => (
          <div key={t.id} className={`toast ${t.kind}`}>
            <div className="grow">{t.text}</div>
            <Button size="sm" icon variant="ghost" aria-label="close" onClick={() => dismiss(t.id)}>
              <Icon name="x" />
            </Button>
          </div>
        ))}
      </div>
    </Ctx.Provider>
  );
}

export const useToast = () => useContext(Ctx);
