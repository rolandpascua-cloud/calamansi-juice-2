import React, { useCallback, useEffect, useMemo, useState } from 'react';
import { ChevronRight } from './components/Icons';
import { fetchSystemState, Field, SystemStateSnapshot } from './utils/systemStateData';
import { writeClipboard } from './utils/clipboardUtils';

// Calamansi Juice 2 addition: System State Viewer GUI panel. A point-in-time
// snapshot (hardware/OS/kernel/firmware/AI-stack), refreshed on tab open and
// via an explicit button only - never auto-polled, unlike the live Resources
// dashboard. See docs/calamansi/system-state.md.

interface SystemStatePanelProps {
  searchQuery: string;
}

const FieldRow: React.FC<{ label: string; field?: Field; render?: (field: Field) => React.ReactNode }> = ({
  label,
  field,
  render,
}) => {
  if (!field) return null;
  return (
    <div className="system-state-row">
      <span className="system-state-row-label">{label}</span>
      {field.available ? (
        <span className="system-state-row-value">
          {render ? render(field) : String(field.value ?? '—')}
        </span>
      ) : (
        <span className="system-state-row-value unavailable" title={field.reason || 'Unavailable'}>
          Unavailable
        </span>
      )}
    </div>
  );
};

const Card: React.FC<{
  id: string;
  title: string;
  expanded: boolean;
  onToggle: (id: string) => void;
  children: React.ReactNode;
}> = ({ id, title, expanded, onToggle, children }) => (
  <div className="model-category system-state-card">
    <div className="model-category-header" onClick={() => onToggle(id)}>
      <span className={`category-chevron ${expanded ? 'expanded' : ''}`}>
        <ChevronRight size={11} strokeWidth={2.1} />
      </span>
      <span className="category-label">{title}</span>
    </div>
    {expanded && <div className="settings-category-content system-state-card-content">{children}</div>}
  </div>
);

const SystemStatePanel: React.FC<SystemStatePanelProps> = () => {
  const [snapshot, setSnapshot] = useState<SystemStateSnapshot | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [copied, setCopied] = useState(false);
  const [expanded, setExpanded] = useState<Set<string>>(
    new Set(['hardware', 'os_kernel', 'firmware', 'ai_stack', 'driver_stack'])
  );

  const load = useCallback(async (refresh: boolean) => {
    setLoading(true);
    setError(null);
    try {
      const data = await fetchSystemState(refresh);
      setSnapshot(data);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to load system state');
    } finally {
      setLoading(false);
    }
  }, []);

  // Refresh on tab open only - this is a snapshot/inspector, not a live
  // dashboard (that's the Resources tab), so there is deliberately no
  // polling interval here.
  useEffect(() => {
    load(false);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const toggle = (id: string) => {
    setExpanded((prev) => {
      const next = new Set(prev);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });
  };

  const asText = useMemo(() => {
    if (!snapshot) return '';
    return JSON.stringify(snapshot, null, 2);
  }, [snapshot]);

  const handleCopy = async () => {
    if (!snapshot) return;
    await writeClipboard(asText);
    setCopied(true);
    setTimeout(() => setCopied(false), 1500);
  };

  const handleExport = () => {
    if (!snapshot) return;
    const blob = new Blob([asText], { type: 'application/json' });
    const url = window.URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href = url;
    link.download = `calamansi-system-state-${snapshot.generated_at_ms}.json`;
    document.body.appendChild(link);
    link.click();
    link.remove();
    window.URL.revokeObjectURL(url);
  };

  if (loading && !snapshot) {
    return <div className="left-panel-empty-state">Loading system state…</div>;
  }
  if (error && !snapshot) {
    return <div className="left-panel-empty-state">Error: {error}</div>;
  }
  if (!snapshot) {
    return null;
  }

  const { hardware, os_kernel, firmware, ai_stack, driver_stack } = snapshot;

  return (
    <div className="system-state-panel">
      <div className="history-panel-toolbar">
        <span className="system-state-generated-at">
          {snapshot.cached ? 'Cached snapshot' : 'Fresh snapshot'} — generated{' '}
          {new Date(snapshot.generated_at_ms).toLocaleString()}
        </span>
        <button className="history-panel-clear-btn system-state-refresh-btn" onClick={() => load(true)} disabled={loading}>
          {loading ? 'Refreshing…' : 'Refresh'}
        </button>
        <button className="history-panel-view-btn" onClick={handleCopy}>
          {copied ? 'Copied!' : 'Copy as text'}
        </button>
        <button className="history-panel-view-btn" onClick={handleExport}>
          Export JSON
        </button>
      </div>

      <Card id="hardware" title="Hardware" expanded={expanded.has('hardware')} onToggle={toggle}>
        <FieldRow
          label="CPU"
          field={{ available: hardware.cpu.available, reason: hardware.cpu.error }}
          render={() => `${hardware.cpu.name} (${hardware.cpu.cores}c/${hardware.cpu.threads}t, ${hardware.cpu.family})`}
        />
        <FieldRow
          label="GPU"
          field={{ available: hardware.gpu.available, reason: hardware.gpu.reason }}
          render={() =>
            `${hardware.gpu.name ?? 'Unknown'} (${hardware.gpu.family ?? '?'}${hardware.gpu.integrated ? ', integrated' : ''})`
          }
        />
        <FieldRow
          label="NPU"
          field={{ available: hardware.npu.available, reason: hardware.npu.reason }}
          render={() =>
            `${hardware.npu.family ?? 'Unknown'}${hardware.npu.tops_max_int ? `, ${hardware.npu.tops_max_int} TOPS` : ''}`
          }
        />
        <FieldRow label="System RAM" field={hardware.memory} render={(f) => String(f.total)} />
        <FieldRow
          label="UMA (unified memory) config"
          field={hardware.uma}
          render={(f) =>
            f.active_param ? `${f.active_param} = ${f.value}` : String(f.note ?? 'module defaults apply')
          }
        />
      </Card>

      <Card id="os_kernel" title="OS / Kernel" expanded={expanded.has('os_kernel')} onToggle={toggle}>
        <FieldRow label="Distro" field={os_kernel.distro} render={(f) => String(f.pretty_name ?? f.name ?? '—')} />
        <FieldRow label="Kernel" field={os_kernel.kernel_version} />
        <FieldRow label="Boot mode" field={os_kernel.boot_mode} />
        <FieldRow
          label="amd_iommu"
          field={os_kernel.amd_iommu}
          render={(f) => `${f.state}${f.disables_npu ? ' (⚠ disables NPU)' : ''}`}
        />
      </Card>

      <Card id="firmware" title="Firmware" expanded={expanded.has('firmware')} onToggle={toggle}>
        <FieldRow label="BIOS/UEFI" field={firmware.bios} render={(f) => `${f.version ?? ''} ${f.date ? `(${f.date})` : ''}`.trim()} />
        <FieldRow label="GPU VBIOS" field={firmware.gpu_vbios} />
      </Card>

      <Card id="ai_stack" title="AI Software Stack" expanded={expanded.has('ai_stack')} onToggle={toggle}>
        <FieldRow label="ROCm" field={ai_stack.rocm_version} />
        <FieldRow label="HIP runtime" field={ai_stack.hip_version} />
        <FieldRow label="Mesa" field={ai_stack.mesa_version} />
        <FieldRow label="Python" field={ai_stack.python_version} />
        {Object.entries(ai_stack.backends).map(([name, field]) => (
          <FieldRow key={name} label={name} field={field} />
        ))}
        <div className="system-state-row">
          <span className="system-state-row-label">This app</span>
          <span className="system-state-row-value">
            Calamansi Juice 2 {ai_stack.app_version.calamansi_version ?? ai_stack.app_version.full_version}
            {' '}(tracks upstream Lemonade {ai_stack.app_version.tracks_upstream_lemonade})
          </span>
        </div>
      </Card>

      <Card id="driver_stack" title="Driver Stack (Strix Halo Z13)" expanded={expanded.has('driver_stack')} onToggle={toggle}>
        {Object.entries(driver_stack).map(([name, info]) => (
          <React.Fragment key={name}>
            <div className="system-state-row">
              <span className="system-state-row-label">{name}</span>
              <span className={`system-state-row-value ${info.installed ? '' : 'unavailable'}`}>
                {info.installed ? 'Installed' : 'Not installed'}
              </span>
            </div>
            {info.active_profile && <FieldRow label={`${name} active profile`} field={info.active_profile} />}
            {info.status && (
              info.status.available ? (
                <pre className="system-state-driver-detail">{String(info.status.value ?? '')}</pre>
              ) : (
                <div className="system-state-row">
                  <span className="system-state-row-label">{name} status</span>
                  <span className="system-state-row-value unavailable" title={info.status.reason || 'Unavailable'}>
                    Unavailable
                  </span>
                </div>
              )
            )}
          </React.Fragment>
        ))}
      </Card>
    </div>
  );
};

export default SystemStatePanel;
