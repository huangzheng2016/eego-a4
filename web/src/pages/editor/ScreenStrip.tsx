// Screen strip above the canvas: a dashboard has 1..MAX_SCREENS screens and
// each page IS one device screen. Tabs "第 n 屏" (device order = visible pages
// in array order), drag to reorder, double-click to rename, plus add /
// duplicate / delete and "show this screen on the device".
import { useState, type DragEvent } from 'react';
import { useI18n } from '../../i18n';
import { MAX_SCREENS } from '../../model/grid';
import type { Page } from '../../model/types';
import { Badge, Button, Icon } from '../../ui';

export interface ScreenStripProps {
  pages: Page[];
  currentId: string;
  defaultId?: string;
  errorCount: (pageId: string) => number;
  onSelect: (pageId: string) => void;
  onAdd: () => void;
  onDuplicate: () => void;
  onDelete: () => void;
  onMove: (from: number, to: number) => void;
  onRename: (pageId: string, title: string) => void;
  /** null = not offered (e.g. no device) */
  onShowOnDevice: (() => void) | null;
  showBusy?: boolean;
}

export function ScreenStrip({ pages, currentId, defaultId, errorCount, onSelect, onAdd, onDuplicate, onDelete, onMove, onRename, onShowOnDevice, showBusy }: ScreenStripProps) {
  const { t } = useI18n();
  const [dragFrom, setDragFrom] = useState<number | null>(null);
  const [dropAt, setDropAt] = useState<number | null>(null);
  const [editing, setEditing] = useState<string | null>(null);
  const full = pages.length >= MAX_SCREENS;
  let visibleNo = 0;

  const drop = (e: DragEvent, to: number) => {
    e.preventDefault();
    if (dragFrom !== null) onMove(dragFrom, to);
    setDragFrom(null);
    setDropAt(null);
  };

  return (
    <div className="screen-strip">
      <div className="screen-tabs" role="tablist" aria-label={t('editor.screens')}>
        {pages.map((p, i) => {
          const hidden = p.visible === false;
          const no = hidden ? 0 : ++visibleNo;
          const errs = errorCount(p.id);
          const on = p.id === currentId;
          return (
            <div
              key={p.id}
              role="tab"
              aria-selected={on}
              tabIndex={0}
              draggable={editing !== p.id}
              className={['screen-tab', on ? 'on' : '', hidden ? 'hidden' : '', dropAt === i && dragFrom !== i ? 'drop' : ''].filter(Boolean).join(' ')}
              title={`${p.id} · ${t('editor.screen_drag_hint')}`}
              onClick={() => onSelect(p.id)}
              onDoubleClick={() => setEditing(p.id)}
              onKeyDown={(e) => {
                if (editing === p.id) return;
                if (e.key === 'Enter') onSelect(p.id);
                else if (e.key === 'F2') setEditing(p.id);
                else if (e.altKey && e.key === 'ArrowLeft' && i > 0) onMove(i, i - 1);
                else if (e.altKey && e.key === 'ArrowRight' && i < pages.length - 1) onMove(i, i + 1);
              }}
              onDragStart={(e) => {
                setDragFrom(i);
                e.dataTransfer.effectAllowed = 'move';
                e.dataTransfer.setData('text/plain', p.id);
              }}
              onDragOver={(e) => {
                if (dragFrom === null) return;
                e.preventDefault();
                setDropAt(i);
              }}
              onDragLeave={() => setDropAt((x) => (x === i ? null : x))}
              onDrop={(e) => drop(e, i)}
              onDragEnd={() => {
                setDragFrom(null);
                setDropAt(null);
              }}
            >
              <span className="screen-tab-no">{hidden ? t('editor.screen_hidden') : t('editor.screen', { n: no })}</span>
              {editing === p.id ? (
                <input
                  className="input screen-tab-input"
                  autoFocus
                  defaultValue={p.title ?? ''}
                  aria-label={t('editor.screen_rename')}
                  onClick={(e) => e.stopPropagation()}
                  onBlur={(e) => {
                    onRename(p.id, e.currentTarget.value.trim());
                    setEditing(null);
                  }}
                  onKeyDown={(e) => {
                    if (e.key === 'Enter') e.currentTarget.blur();
                    else if (e.key === 'Escape') setEditing(null);
                  }}
                />
              ) : (
                <span className="screen-tab-title truncate">{p.title || p.id}</span>
              )}
              {hidden && <Icon name="eyeoff" size={13} />}
              {defaultId === p.id && <span title={t('editor.default_page')}>★</span>}
              {errs > 0 && <Badge tone="danger">{errs}</Badge>}
            </div>
          );
        })}
        <Button size="sm" icon variant="ghost" disabled={full} title={full ? t('editor.max_screens', { n: MAX_SCREENS }) : t('editor.add_page')} aria-label={t('editor.add_page')} onClick={onAdd}>
          <Icon name="plus" size={16} />
        </Button>
      </div>
      <div className="screen-actions">
        <span className="dim hide-sm">{t('editor.screen_count', { n: pages.length, max: MAX_SCREENS })}</span>
        <Button size="sm" icon variant="ghost" title={t('editor.screen_rename')} onClick={() => setEditing(currentId)}>
          <Icon name="sliders" size={15} />
        </Button>
        <Button size="sm" icon variant="ghost" disabled={full} title={full ? t('editor.max_screens', { n: MAX_SCREENS }) : t('editor.screen_duplicate')} onClick={onDuplicate}>
          <Icon name="copy" size={15} />
        </Button>
        <Button size="sm" icon variant="ghost" disabled={pages.length <= 1} title={t('editor.delete_page')} onClick={onDelete}>
          <Icon name="trash" size={15} />
        </Button>
        {onShowOnDevice && (
          <Button size="sm" variant="ghost" loading={showBusy} title={t('editor.show_on_device_hint')} onClick={onShowOnDevice}>
            {!showBusy && <Icon name="device" size={15} />} <span className="hide-sm">{t('editor.show_on_device')}</span>
          </Button>
        )}
      </div>
    </div>
  );
}
