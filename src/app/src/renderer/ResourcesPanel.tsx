import React, { useEffect, useRef, useState } from 'react';
import {
  fetchResourcesInference,
  fetchResourcesSystem,
  formatBytes,
  InferenceServiceStatus,
  ResourcesInference,
  ResourcesSystem,
} from './utils/resourceData';

// Calamansi Juice 2 addition: live Resource Dashboard GUI panel. Polls
// GET /resources/system every ~2s and GET /resources/inference every ~4-5s
// (that one shells out more, per the endpoint's own design). Polling stops
// whenever the browser/app window is backgrounded (document.hidden) and,
// separately, whenever this component unmounts - which already happens for
// free every time the user switches to a different left-panel tab, since
// ModelManager only mounts the active tab's panel.

const SYSTEM_POLL_MS = 2000;
const INFERENCE_POLL_MS = 4500;
const SPARKLINE_HISTORY_LENGTH = 30;

interface ResourcesPanelProps {
  searchQuery: string;
}

const Sparkline: React.FC<{ values: number[]; max?: number; color?: string }> = ({ values, max = 100, color }) => {
  const width = 120;
  const height = 28;
  if (values.length < 2) {
    return <svg viewBox={`0 0 ${width} ${height}`} className="resources-sparkline" />;
  }
  const points = values
    .map((v, i) => {
      const x = (i / (values.length - 1)) * width;
      const y = height - Math.min(1, Math.max(0, v / max)) * height;
      return `${x.toFixed(1)},${y.toFixed(1)}`;
    })
    .join(' ');
  return (
    <svg viewBox={`0 0 ${width} ${height}`} className="resources-sparkline" preserveAspectRatio="none">
      <polyline points={points} fill="none" stroke={color || 'var(--info-color)'} strokeWidth={1.5} />
    </svg>
  );
};

const Gauge: React.FC<{ label: string; percent: number | null | undefined; sub?: string; history: number[] }> = ({
  label,
  percent,
  sub,
  history,
}) => {
  const pct = percent == null ? null : Math.max(0, Math.min(100, percent));
  const color = pct == null ? 'var(--text-aaa)' : pct > 85 ? 'var(--error-color)' : pct > 60 ? 'var(--warning-color)' : 'var(--success-color)';
  return (
    <div className="resources-gauge">
      <div className="resources-gauge-header">
        <span className="resources-gauge-label">{label}</span>
        <span className="resources-gauge-value" style={{ color }}>
          {pct == null ? '—' : `${pct.toFixed(1)}%`}
        </span>
      </div>
      <div className="resources-gauge-bar-track">
        <div className="resources-gauge-bar-fill" style={{ width: `${pct ?? 0}%`, background: color }} />
      </div>
      {sub && <div className="resources-gauge-sub">{sub}</div>}
      <Sparkline values={history} color={color} />
    </div>
  );
};

const serviceLabel: Record<string, string> = {
  lmstudio: 'LM Studio',
  llama_server: 'llama-server',
  vllm: 'vLLM',
  fastflowlm: 'FastFlowLM',
};

const ServiceRow: React.FC<{ id: string; status: InferenceServiceStatus }> = ({ id, status }) => {
  const running = Boolean(status.process_running);
  const detail =
    status.error ? status.error :
    status.note ? status.note :
    running ? (typeof status.port === 'number' || typeof status.port === 'string' ? `port ${status.port}` : 'running') :
    status.cli_found ? 'installed, not running' : 'not detected';
  return (
    <div className="system-state-row">
      <span className="system-state-row-label">{serviceLabel[id] || id}</span>
      <span className={`resources-service-status ${running ? 'running' : 'idle'}`} title={detail}>
        {running ? '● Running' : '○ Not running'} — {detail}
      </span>
    </div>
  );
};

const ResourcesPanel: React.FC<ResourcesPanelProps> = () => {
  const [system, setSystem] = useState<ResourcesSystem | null>(null);
  const [inference, setInference] = useState<ResourcesInference | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [paused, setPaused] = useState(document.hidden);

  const cpuHistory = useRef<number[]>([]);
  const gpuHistory = useRef<number[]>([]);
  const memHistory = useRef<number[]>([]);
  const [, forceRender] = useState(0);

  useEffect(() => {
    const handleVisibility = () => setPaused(document.hidden);
    document.addEventListener('visibilitychange', handleVisibility);
    return () => document.removeEventListener('visibilitychange', handleVisibility);
  }, []);

  useEffect(() => {
    if (paused) return undefined;
    let cancelled = false;

    const poll = async () => {
      try {
        const data = await fetchResourcesSystem();
        if (cancelled) return;
        setSystem(data);
        setError(null);

        const pushHistory = (ref: React.MutableRefObject<number[]>, value: number | null | undefined) => {
          if (value == null) return;
          ref.current = [...ref.current, value].slice(-SPARKLINE_HISTORY_LENGTH);
        };
        pushHistory(cpuHistory, data.cpu?.aggregate_percent);
        pushHistory(gpuHistory, data.gpu?.utilization_percent);
        pushHistory(memHistory, data.memory?.percent);
        forceRender((n) => n + 1);
      } catch (err) {
        if (!cancelled) setError(err instanceof Error ? err.message : 'Failed to load resources');
      }
    };

    poll();
    const interval = setInterval(poll, SYSTEM_POLL_MS);
    return () => {
      cancelled = true;
      clearInterval(interval);
    };
  }, [paused]);

  useEffect(() => {
    if (paused) return undefined;
    let cancelled = false;

    const poll = async () => {
      try {
        const data = await fetchResourcesInference();
        if (!cancelled) setInference(data);
      } catch {
        // Inference-service probing is supplementary; a failure here
        // shouldn't blank the whole panel.
      }
    };

    poll();
    const interval = setInterval(poll, INFERENCE_POLL_MS);
    return () => {
      cancelled = true;
      clearInterval(interval);
    };
  }, [paused]);

  if (!system && !error) {
    return <div className="left-panel-empty-state">Loading resources…</div>;
  }

  const uma = system?.memory?.uma;
  const npu = system?.npu;

  return (
    <div className="system-state-panel resources-panel">
      {error && <div className="left-panel-empty-state">Error: {error}</div>}

      {system && (
        <>
          <div className="resources-gauge-grid">
            <Gauge
              label="CPU"
              percent={system.cpu?.aggregate_percent}
              sub={system.cpu?.load_average ? `load ${system.cpu.load_average['1m'].toFixed(2)}` : undefined}
              history={cpuHistory.current}
            />
            <Gauge
              label="Memory"
              percent={system.memory?.percent}
              sub={system.memory?.used != null ? `${formatBytes(system.memory.used)} / ${formatBytes(system.memory.total)}` : undefined}
              history={memHistory.current}
            />
            <Gauge
              label="GPU"
              percent={system.gpu?.utilization_percent}
              sub={
                system.gpu?.temperature_c != null
                  ? `${system.gpu.temperature_c.toFixed(0)}°C, ${system.gpu.power_watts?.toFixed(1) ?? '?'}W`
                  : system.gpu?.error
              }
              history={gpuHistory.current}
            />
          </div>

          <div className="model-category system-state-card">
            <div className="settings-category-content system-state-card-content">
              <div className="system-state-row">
                <span className="system-state-row-label">UMA (unified memory)</span>
                <span className="system-state-row-value">{uma ? uma.note : '—'}</span>
              </div>
              <div className="system-state-row">
                <span className="system-state-row-label">NPU</span>
                <span className="system-state-row-value">
                  {npu?.present == null
                    ? '—'
                    : npu.present
                      ? `Present${npu.amdxdna_module_loaded ? ', driver loaded' : ', driver not loaded'}`
                      : 'Not detected'}
                </span>
              </div>
              <div className="system-state-row">
                <span className="system-state-row-label">amd_iommu</span>
                <span className={`system-state-row-value ${npu?.amd_iommu_ok === false ? 'unavailable' : ''}`} title={npu?.amd_iommu_note}>
                  {npu?.amd_iommu_ok == null ? '—' : npu.amd_iommu_ok ? 'OK' : 'Not OK'}
                </span>
              </div>
              {system.disk && (
                <div className="system-state-row">
                  <span className="system-state-row-label">Disk (/)</span>
                  <span className="system-state-row-value">
                    {system.disk.percent != null
                      ? `${system.disk.percent.toFixed(1)}% used (${formatBytes(system.disk.used)} / ${formatBytes(system.disk.total)})`
                      : system.disk.error}
                    {system.disk.btrfs_status && system.disk.btrfs_status !== 'not_installed' && system.disk.btrfs_status !== 'ok'
                      ? ` — btrfs: ${system.disk.btrfs_status}`
                      : ''}
                  </span>
                </div>
              )}
            </div>
          </div>

          <div className="model-category system-state-card">
            <div className="model-category-header"><span className="category-label">Other Local Inference Services</span></div>
            <div className="settings-category-content system-state-card-content">
              {inference ? (
                Object.entries(inference)
                  .filter(([key]) => key !== 'timestamp')
                  .map(([key, status]) => <ServiceRow key={key} id={key} status={status as InferenceServiceStatus} />)
              ) : (
                <div className="system-state-row">
                  <span className="system-state-row-label">Loading…</span>
                </div>
              )}
            </div>
          </div>
        </>
      )}
    </div>
  );
};

export default ResourcesPanel;
