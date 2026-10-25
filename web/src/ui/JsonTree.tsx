// Collapsible JSON tree. Clicking a row reports its concrete path segments.
import { memo, useState } from 'react';
import { toJsonPath, type Segment } from '../model/jsonpath';

const MAX_CHILDREN = 100;

interface NodeProps {
  name: Segment | null;
  value: unknown;
  path: Segment[];
  depth: number;
  onPick: (path: Segment[], value: unknown) => void;
  isMatch?: (path: Segment[]) => boolean;
}

function preview(v: unknown): { cls: string; text: string } {
  if (v === null) return { cls: 'jt-lit', text: 'null' };
  if (typeof v === 'string') return { cls: 'jt-str', text: JSON.stringify(v.length > 80 ? v.slice(0, 80) + '…' : v) };
  if (typeof v === 'number') return { cls: 'jt-num', text: String(v) };
  if (typeof v === 'boolean') return { cls: 'jt-lit', text: String(v) };
  if (Array.isArray(v)) return { cls: 'jt-meta', text: `[${v.length}]` };
  return { cls: 'jt-meta', text: `{${Object.keys(v as object).length}}` };
}

const Node = memo(function Node({ name, value, path, depth, onPick, isMatch }: NodeProps) {
  const container = value !== null && typeof value === 'object';
  const [open, setOpen] = useState(depth < 2);
  const p = preview(value);
  const entries: [Segment, unknown][] = container ? (Array.isArray(value) ? value.map((v, i) => [i, v] as [Segment, unknown]) : Object.entries(value as object)) : [];
  return (
    <div>
      <div
        className={`jt-row${isMatch?.(path) ? ' match' : ''}`}
        title={toJsonPath(path)}
        onClick={() => onPick(path, value)}
        role="treeitem"
        aria-expanded={container ? open : undefined}
        tabIndex={0}
        onKeyDown={(e) => {
          if (e.key === 'Enter') onPick(path, value);
          if (e.key === 'ArrowRight' && container) setOpen(true);
          if (e.key === 'ArrowLeft' && container) setOpen(false);
        }}
      >
        <span
          className="jt-toggle"
          onClick={(e) => {
            if (!container) return;
            e.stopPropagation();
            setOpen(!open);
          }}
        >
          {container ? (open ? '▾' : '▸') : ''}
        </span>
        {name !== null && <span className="jt-key">{typeof name === 'number' ? `[${name}]` : name}:</span>}
        <span className={p.cls}>{p.text}</span>
      </div>
      {container && open && (
        <div className="jt-children" role="group">
          {entries.slice(0, MAX_CHILDREN).map(([k, v]) => (
            <Node key={String(k)} name={k} value={v} path={[...path, k]} depth={depth + 1} onPick={onPick} isMatch={isMatch} />
          ))}
          {entries.length > MAX_CHILDREN && <div className="jt-meta">… +{entries.length - MAX_CHILDREN}</div>}
        </div>
      )}
    </div>
  );
});

export function JsonTree({ value, onPick, isMatch }: { value: unknown; onPick: (path: Segment[], value: unknown) => void; isMatch?: (path: Segment[]) => boolean }) {
  return (
    <div className="jt" role="tree">
      <Node name={null} value={value} path={[]} depth={0} onPick={onPick} isMatch={isMatch} />
    </div>
  );
}
