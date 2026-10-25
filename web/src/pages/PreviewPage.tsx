import { useI18n } from '../i18n';
import { PageHeader } from '../ui';
import { LivePreview } from './LivePreview';

/** Full-page live e-ink frame. */
export function PreviewPage() {
  const { t } = useI18n();
  return (
    <div className="page">
      <PageHeader icon="device" title={t('preview.title')} subtitle={t('preview.subtitle')} />
      <div style={{ maxWidth: 440, margin: '0 auto' }}>
        <LivePreview compact />
      </div>
    </div>
  );
}
