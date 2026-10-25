// Schematic rendering of a panel inside the canvas / template thumbnails.
// It approximates the e-ink renderer (dashboard_ui/panels.cpp) for layout
// purposes only; the device snapshot (PreviewPage) shows the real output.
import { memo } from 'react';
import type { Panel } from '../../model/types';

const SPARK = [3, 5, 4, 6, 5, 8, 7, 9, 6, 8, 10, 9];

function Spark({ bars, fill }: { bars?: boolean; fill?: boolean }) {
  const max = 11;
  const pts = SPARK.map((v, i) => `${(i / (SPARK.length - 1)) * 100},${100 - (v / max) * 100}`).join(' ');
  return (
    <svg className="pv-svg" viewBox="0 0 100 100" preserveAspectRatio="none" aria-hidden>
      {bars ? (
        SPARK.map((v, i) => <rect key={i} x={i * (100 / SPARK.length) + 1} y={100 - (v / max) * 100} width={100 / SPARK.length - 2} height={(v / max) * 100} fill="#333" />)
      ) : (
        <>
          {fill && <polygon points={`0,100 ${pts} 100,100`} fill="#ccc" />}
          <polyline points={pts} fill="none" stroke="#222" strokeWidth="3" vectorEffect="non-scaling-stroke" />
        </>
      )}
    </svg>
  );
}

function Lines({ n }: { n: number }) {
  return (
    <div className="pv-lines">
      {Array.from({ length: n }, (_, i) => (
        <i key={i} style={{ width: `${55 + ((i * 37) % 40)}%` }} />
      ))}
    </div>
  );
}

export const PanelPreview = memo(function PanelPreview({ panel, sample, compact }: { panel: Panel; sample?: { value: number; unit?: string }; compact?: boolean }) {
  const variant = panel.style?.variant ?? 'default';
  const unit = sample?.unit || panel.style?.unit || '';
  const value = sample ? sample.value.toFixed(Math.min(panel.style?.precision ?? 1, 3)) : '42.0';
  const title = panel.title || panel.id;
  const kindTag = compact ? null : <span className="pv-kind">{panel.kind}{variant !== 'default' ? `·${variant}` : ''}</span>;
  switch (panel.kind) {
    case 'card':
      return (
        <div className="pv">
          {kindTag}
          <div className="pv-title">{title}</div>
          {variant === 'dual' || variant === 'triple' ? (
            <div className="row" style={{ marginTop: 'auto', gap: '10%' }}>
              {Array.from({ length: variant === 'dual' ? 2 : 3 }, (_, i) => (
                <div key={i} className="pv-value" style={{ fontSize: 18 }}>
                  {(Number(value) * (1 - i * 0.3)).toFixed(1)}
                </div>
              ))}
            </div>
          ) : (
            <div className="pv-value">
              {value}
              {unit && <small>{unit}</small>}
            </div>
          )}
          {variant === 'progress' && (
            <div className="pv-progress">
              <i />
            </div>
          )}
          {variant === 'delta' && <div className="pv-sub">▲ +3.2</div>}
          {variant === 'status' && <div className="pv-sub">● OK</div>}
          {panel.metric && variant === 'default' && <div className="pv-sub">{panel.metric}</div>}
        </div>
      );
    case 'chart':
      return (
        <div className="pv">
          {kindTag}
          <div className="pv-title">{title}</div>
          <Spark bars={panel.chart?.kind === 'bar'} fill={panel.chart?.fill} />
        </div>
      );
    case 'table':
      return (
        <div className="pv">
          {kindTag}
          <div className="pv-title">{title}</div>
          {variant === 'status_grid' || variant === 'heatmap' ? (
            <div className="pv-grid">
              {Array.from({ length: 18 }, (_, i) => (
                <i key={i} style={{ background: variant === 'heatmap' ? `rgba(0,0,0,${((i * 7) % 10) / 12 + 0.1})` : i % 5 === 0 ? '#222' : '#ccc' }} />
              ))}
            </div>
          ) : (
            <Lines n={6} />
          )}
        </div>
      );
    case 'alert':
      return (
        <div className="pv pv-alert">
          {kindTag}
          <div className="pv-title">⚠ {title}</div>
          <div className="pv-sub" style={{ color: '#ddd' }}>{panel.text?.content || 'alert'}</div>
        </div>
      );
    case 'button':
      return (
        <div className="pv pv-button">
          {kindTag}
          <span>{panel.actions?.[0]?.label || title}</span>
        </div>
      );
    case 'image':
      return (
        <div className="pv">
          {kindTag}
          <svg className="pv-svg" viewBox="0 0 100 60" aria-hidden>
            <rect x="1" y="1" width="98" height="58" fill="none" stroke="#999" strokeDasharray="4 3" />
            <circle cx="30" cy="22" r="8" fill="#bbb" />
            <polygon points="10,55 40,28 60,45 75,32 92,55" fill="#999" />
          </svg>
        </div>
      );
    case 'text':
    default:
      return (
        <div className="pv">
          {kindTag}
          <div className="pv-title">{title}</div>
          {panel.text?.content ? <div className="pv-sub" style={{ whiteSpace: 'normal' }}>{panel.text.content}</div> : <Lines n={variant === 'single' ? 1 : 4} />}
        </div>
      );
  }
});
