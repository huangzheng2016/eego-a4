import { describe, expect, it } from 'vitest';
import { getAt, itemsPathFor, parseJsonPath, pathForField, prefixMatch, toJsonPath } from '../src/model/jsonpath';

describe('JSONPath generation from tree nodes', () => {
  it('builds dot and bracket segments', () => {
    expect(toJsonPath([])).toBe('$');
    expect(toJsonPath(['cpu', 'usage'])).toBe('$.cpu.usage');
    expect(toJsonPath(['disks', 1, 'used_pct'])).toBe('$.disks[1].used_pct');
    expect(toJsonPath(['a.b', "it's", 'x y'])).toBe("$['a.b']['it\\'s']['x y']");
  });
  it('round-trips through the parser', () => {
    for (const segs of [['cpu', 'load', 2], ['a.b', 0, 'c d'], ["q'uote"]]) {
      const tokens = parseJsonPath(toJsonPath(segs))!;
      expect(tokens.map((t) => (t.kind === 'child' ? t.key : t.kind === 'index' ? t.index : '*'))).toEqual(segs);
    }
    expect(parseJsonPath('$.metrics[*].value')).toEqual([{ kind: 'child', key: 'metrics' }, { kind: 'wildcard' }, { kind: 'child', key: 'value' }]);
    expect(parseJsonPath('metrics')).toBeNull();
  });
  it('generalises items_path to the enclosing array', () => {
    expect(itemsPathFor(['metrics', 3, 'value'])).toBe('$.metrics[*]');
    expect(itemsPathFor(['data', 'rows', 0, 'cells', 2])).toBe('$.data.rows[0].cells[*]');
    expect(itemsPathFor(['cpu'])).toBe('$');
  });
  it('makes field paths relative to items_path', () => {
    expect(pathForField('value_path', ['metrics', 3, 'value'], '$.metrics[*]')).toBe('$.value');
    expect(pathForField('name_path', ['metrics', 0, 'meta', 'name'], '$.metrics[*]')).toBe('$.meta.name');
    expect(pathForField('value_path', ['cpu', 'usage'], '$.metrics[*]')).toBe('$.cpu.usage');
    expect(pathForField('value_path', ['cpu', 'usage'], '$')).toBe('$.cpu.usage');
    expect(pathForField('items_path', ['disks', 0, 'used_pct'])).toBe('$.disks[*]');
  });
  it('matches prefixes with wildcards', () => {
    expect(prefixMatch('$.metrics[*]', ['metrics', 4, 'value'])).toBe(2);
    expect(prefixMatch('$.metrics[1]', ['metrics', 4])).toBe(-1);
    expect(getAt({ a: [{ b: 7 }] }, ['a', 0, 'b'])).toBe(7);
  });
});
