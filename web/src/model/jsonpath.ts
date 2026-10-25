// JSONPath helpers for the bounded subset accepted by
// data_pipeline/json_path.cpp: $, .child, ['quoted'], [index], [*] / .*

export type Segment = string | number;
export type PathToken = { kind: 'child'; key: string } | { kind: 'index'; index: number } | { kind: 'wildcard' };

const IDENT = /^[A-Za-z_$][A-Za-z0-9_$-]*$/;

export function quoteKey(key: string): string {
  return "['" + key.replace(/\\/g, '\\\\').replace(/'/g, "\\'") + "']";
}

/** Build a JSONPath from concrete tree segments. */
export function toJsonPath(segments: Segment[]): string {
  let out = '$';
  for (const s of segments) {
    if (typeof s === 'number') out += `[${s}]`;
    else if (IDENT.test(s) && !s.includes('.') && !s.includes('[')) out += `.${s}`;
    else out += quoteKey(s);
  }
  return out;
}

/** Parse a JSONPath of the supported subset. Returns null when unsupported. */
export function parseJsonPath(path: string): PathToken[] | null {
  if (!path || path[0] !== '$') return null;
  const tokens: PathToken[] = [];
  let i = 1;
  while (i < path.length) {
    const c = path[i];
    if (c === '.') {
      i++;
      if (path[i] === '*') {
        tokens.push({ kind: 'wildcard' });
        i++;
        continue;
      }
      const start = i;
      while (i < path.length && path[i] !== '.' && path[i] !== '[') i++;
      if (i === start) return null;
      tokens.push({ kind: 'child', key: path.slice(start, i) });
    } else if (c === '[') {
      i++;
      if (path[i] === '*') {
        if (path[i + 1] !== ']') return null;
        tokens.push({ kind: 'wildcard' });
        i += 2;
      } else if (path[i] === "'" || path[i] === '"') {
        const q = path[i++];
        let key = '';
        while (i < path.length && path[i] !== q) {
          if (path[i] === '\\') i++;
          key += path[i++];
        }
        if (path[i] !== q || path[i + 1] !== ']') return null;
        i += 2;
        tokens.push({ kind: 'child', key });
      } else {
        const m = /^\d+/.exec(path.slice(i));
        if (!m || path[i + m[0].length] !== ']') return null;
        tokens.push({ kind: 'index', index: Number(m[0]) });
        i += m[0].length + 1;
      }
    } else {
      return null;
    }
  }
  return tokens;
}

/** Number of segments of `segments` matched by pattern `path`, or -1. */
export function prefixMatch(path: string, segments: Segment[]): number {
  const tokens = parseJsonPath(path);
  if (!tokens || tokens.length > segments.length) return -1;
  for (let i = 0; i < tokens.length; i++) {
    const t = tokens[i];
    const s = segments[i];
    if (t.kind === 'wildcard') continue;
    if (t.kind === 'index' && s !== t.index) return -1;
    if (t.kind === 'child' && s !== t.key) return -1;
  }
  return tokens.length;
}

/** Path to the nearest enclosing array, generalised with [*] (items_path). */
export function itemsPathFor(segments: Segment[]): string {
  for (let i = segments.length - 1; i >= 0; i--) {
    if (typeof segments[i] === 'number') {
      return toJsonPath(segments.slice(0, i)) + '[*]';
    }
  }
  return '$';
}

/**
 * JSONPath to assign to a mapping field when a tree node is clicked.
 * `items_path` is generalised to the enclosing array; other fields are made
 * relative to the mapping's items_path when the node lies inside it.
 */
export function pathForField(field: string, segments: Segment[], itemsPath?: string): string {
  if (field === 'items_path') return itemsPathFor(segments);
  if (itemsPath && itemsPath !== '$') {
    const n = prefixMatch(itemsPath, segments);
    if (n >= 0) return toJsonPath(segments.slice(n));
  }
  return toJsonPath(segments);
}

/** Resolve a concrete path (no wildcards needed) against a value. */
export function getAt(root: unknown, segments: Segment[]): unknown {
  let cur: unknown = root;
  for (const s of segments) {
    if (cur == null || typeof cur !== 'object') return undefined;
    cur = (cur as Record<string | number, unknown>)[s];
  }
  return cur;
}
