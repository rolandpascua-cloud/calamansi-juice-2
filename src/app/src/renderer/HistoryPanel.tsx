import React, { useCallback, useEffect, useMemo, useState } from 'react';
import { serverFetch } from './utils/serverConfig';
import { useConfirmDialog } from './ConfirmDialog';

// Calamansi Juice 2 addition: Telemetry History Viewer GUI panel. Reads from
// the SQLite-backed history store (telemetry_history_store.h) via
// GET /telemetry/history and /telemetry/history/summary, added in
// server.cpp. See docs/calamansi/history.md and MERGING.md.

interface HistoryRecord {
  id: number;
  request_id: string;
  timestamp_ms: number;
  model_name: string;
  route: string;
  route_decision: string | null;
  prompt_tokens: number | null;
  completion_tokens: number | null;
  latency_ms: number | null;
  ttft_seconds: number | null;
  tokens_per_second: number | null;
  backend: string;
  device: string;
  success: boolean;
  error: string;
  preview: string;
}

interface HistoryResponse {
  records: HistoryRecord[];
  total: number;
  limit: number;
  offset: number;
}

interface SummaryResponse {
  avg_tokens_per_second_by_model: { model: string; avg_tokens_per_second: number; request_count: number }[];
  requests_by_day: { date: string; count: number; errors: number; error_rate: number }[];
  overall: { total_requests: number; total_errors: number; error_rate: number };
}

type SortKey = 'timestamp_ms' | 'model_name' | 'tokens_per_second';
type SortDir = 'asc' | 'desc';

const PAGE_SIZE = 50;

interface HistoryPanelProps {
  searchQuery: string;
}

const HistoryPanel: React.FC<HistoryPanelProps> = ({ searchQuery }) => {
  const [view, setView] = useState<'table' | 'charts'>('table');
  const [records, setRecords] = useState<HistoryRecord[]>([]);
  const [total, setTotal] = useState(0);
  const [offset, setOffset] = useState(0);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  const [routeFilter, setRouteFilter] = useState('all');
  const [backendFilter, setBackendFilter] = useState('all');
  const [statusFilter, setStatusFilter] = useState<'all' | 'success' | 'error'>('all');

  const [sortKey, setSortKey] = useState<SortKey>('timestamp_ms');
  const [sortDir, setSortDir] = useState<SortDir>('desc');

  const [summary, setSummary] = useState<SummaryResponse | null>(null);
  const [summaryLoading, setSummaryLoading] = useState(false);
  const [clearing, setClearing] = useState(false);
  const [downloading, setDownloading] = useState(false);

  const { confirm, ConfirmDialog: ClearConfirmDialog } = useConfirmDialog();

  const fetchHistory = useCallback(async (nextOffset: number) => {
    setLoading(true);
    setError(null);
    try {
      const params = new URLSearchParams();
      params.set('limit', String(PAGE_SIZE));
      params.set('offset', String(nextOffset));
      if (routeFilter !== 'all') params.set('route', routeFilter);
      if (backendFilter !== 'all') params.set('backend', backendFilter);

      const response = await serverFetch(`/telemetry/history?${params.toString()}`);
      if (!response.ok) {
        throw new Error(`HTTP ${response.status}`);
      }
      const data: HistoryResponse = await response.json();
      setRecords(Array.isArray(data.records) ? data.records : []);
      setTotal(data.total || 0);
      setOffset(nextOffset);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to load telemetry history');
    } finally {
      setLoading(false);
    }
  }, [routeFilter, backendFilter]);

  const fetchSummary = useCallback(async () => {
    setSummaryLoading(true);
    try {
      const response = await serverFetch('/telemetry/history/summary');
      if (!response.ok) {
        throw new Error(`HTTP ${response.status}`);
      }
      const data: SummaryResponse = await response.json();
      setSummary(data);
    } catch (err) {
      // Charts are supplementary; a summary failure shouldn't block the table view.
      console.error('Failed to load telemetry history summary:', err);
    } finally {
      setSummaryLoading(false);
    }
  }, []);

  useEffect(() => {
    fetchHistory(0);
  }, [fetchHistory]);

  useEffect(() => {
    if (view === 'charts' && !summary) {
      fetchSummary();
    }
  }, [view, summary, fetchSummary]);

  // Server-side status filtering isn't part of the API (success/error is a
  // small enough set that filtering the current page client-side is simpler
  // than adding another query param); searchQuery similarly matches within
  // the currently-loaded page across model/route/backend/device.
  const visibleRecords = useMemo(() => {
    let rows = records;
    if (statusFilter !== 'all') {
      rows = rows.filter((r) => (statusFilter === 'success' ? r.success : !r.success));
    }
    const query = searchQuery.trim().toLowerCase();
    if (query) {
      rows = rows.filter((r) =>
        r.model_name.toLowerCase().includes(query) ||
        r.route.toLowerCase().includes(query) ||
        r.backend.toLowerCase().includes(query) ||
        r.device.toLowerCase().includes(query)
      );
    }
    const dir = sortDir === 'asc' ? 1 : -1;
    return [...rows].sort((a, b) => {
      const av = a[sortKey];
      const bv = b[sortKey];
      if (av == null && bv == null) return 0;
      if (av == null) return 1;
      if (bv == null) return -1;
      if (typeof av === 'string' || typeof bv === 'string') {
        return dir * String(av).localeCompare(String(bv));
      }
      return dir * ((av as number) - (bv as number) === 0 ? 0 : (av as number) > (bv as number) ? 1 : -1);
    });
  }, [records, statusFilter, searchQuery, sortKey, sortDir]);

  const routeOptions = useMemo(() => Array.from(new Set(records.map((r) => r.route))).sort(), [records]);
  const backendOptions = useMemo(() => Array.from(new Set(records.map((r) => r.backend).filter(Boolean))).sort(), [records]);

  const toggleSort = (key: SortKey) => {
    if (sortKey === key) {
      setSortDir((d) => (d === 'asc' ? 'desc' : 'asc'));
    } else {
      setSortKey(key);
      setSortDir('desc');
    }
  };

  const handleClear = async () => {
    const ok = await confirm({
      title: 'Clear Telemetry History',
      message: 'This permanently deletes all stored request history from this device. This cannot be undone.',
      confirmText: 'Clear History',
      danger: true,
    });
    if (!ok) return;

    setClearing(true);
    try {
      // Every POST endpoint on this server 400s cpp-httplib's own body parser
      // if it receives no Content-Type/body at all (confirmed against the
      // pre-existing /internal/telemetry/flush endpoint too - not specific to
      // this one) - so send an explicit (unused) JSON body, matching every
      // other no-payload POST call in this app (e.g. ModelManager.tsx's
      // /internal/pin).
      const response = await serverFetch('/internal/telemetry/history/clear', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({}),
      });
      if (!response.ok) {
        throw new Error(`HTTP ${response.status}`);
      }
      setRecords([]);
      setTotal(0);
      setSummary(null);
      await fetchHistory(0);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to clear history');
    } finally {
      setClearing(false);
    }
  };

  const csvEscape = (value: string | number | boolean | null) => {
    const s = value == null ? '' : String(value);
    return /[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
  };

  const handleDownload = async () => {
    setDownloading(true);
    setError(null);
    try {
      // Full history, not just the currently-loaded page — same route/backend
      // filters as the table, but ignores the client-side-only status/search
      // filters so the export is a complete record, not a narrowed view.
      const DOWNLOAD_PAGE_SIZE = 500;
      const all: HistoryRecord[] = [];
      let downloadOffset = 0;
      for (;;) {
        const params = new URLSearchParams();
        params.set('limit', String(DOWNLOAD_PAGE_SIZE));
        params.set('offset', String(downloadOffset));
        if (routeFilter !== 'all') params.set('route', routeFilter);
        if (backendFilter !== 'all') params.set('backend', backendFilter);
        const response = await serverFetch(`/telemetry/history?${params.toString()}`);
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        const data: HistoryResponse = await response.json();
        const page = Array.isArray(data.records) ? data.records : [];
        all.push(...page);
        downloadOffset += DOWNLOAD_PAGE_SIZE;
        if (page.length < DOWNLOAD_PAGE_SIZE || downloadOffset >= data.total) break;
      }

      const header = [
        'timestamp', 'model', 'route', 'route_decision', 'backend', 'device',
        'input_tokens', 'output_tokens', 'ttft_seconds', 'tokens_per_second',
        'latency_ms', 'status', 'error', 'preview',
      ];
      const rows = all.map((r) => [
        new Date(r.timestamp_ms).toISOString(),
        r.model_name,
        r.route,
        r.route_decision ?? '',
        r.backend,
        r.device,
        r.prompt_tokens ?? '',
        r.completion_tokens ?? '',
        r.ttft_seconds ?? '',
        r.tokens_per_second ?? '',
        r.latency_ms ?? '',
        r.success ? 'ok' : 'error',
        r.error,
        r.preview,
      ]);
      const csv = [header, ...rows].map((row) => row.map(csvEscape).join(',')).join('\n');

      const blob = new Blob([csv], { type: 'text/csv;charset=utf-8;' });
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = `calamansi-history-${new Date().toISOString().slice(0, 19).replace(/[:T]/g, '-')}.csv`;
      document.body.appendChild(a);
      a.click();
      a.remove();
      URL.revokeObjectURL(url);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to download history');
    } finally {
      setDownloading(false);
    }
  };

  const formatTime = (ms: number) => new Date(ms).toLocaleString();
  const formatNum = (n: number | null, digits = 1) => (n == null ? '—' : n.toFixed(digits));

  // The table only has room for Time/Model/Tokens-per-second in the left
  // panel's fixed width; the rest of each record is available on hover
  // (and in full via Download History) rather than as its own column.
  const rowTooltip = (r: HistoryRecord) => {
    const lines = [
      `Route: ${r.route || '—'}`,
      `Backend: ${r.backend || '—'}`,
      `Device: ${r.device || '—'}`,
      `Input tokens: ${r.prompt_tokens ?? '—'}`,
      `Output tokens: ${r.completion_tokens ?? '—'}`,
      `TTFT: ${formatNum(r.ttft_seconds, 2)}s`,
      `Latency: ${formatNum(r.latency_ms, 0)}ms`,
      `Status: ${r.success ? 'OK' : `Error — ${r.error || 'unknown'}`}`,
    ];
    return lines.join('\n');
  };

  return (
    <div className="history-panel">
      <ClearConfirmDialog />
      <div className="history-panel-toolbar">
        <div className="history-panel-view-toggle">
          <button
            className={`history-panel-view-btn ${view === 'table' ? 'active' : ''}`}
            onClick={() => setView('table')}
          >
            Table
          </button>
          <button
            className={`history-panel-view-btn ${view === 'charts' ? 'active' : ''}`}
            onClick={() => setView('charts')}
          >
            Charts
          </button>
        </div>

        {view === 'table' && (
          <div className="history-panel-filters">
            <select value={routeFilter} onChange={(e) => setRouteFilter(e.target.value)}>
              <option value="all">All routes</option>
              {routeOptions.map((r) => (
                <option key={r} value={r}>{r}</option>
              ))}
            </select>
            <select value={backendFilter} onChange={(e) => setBackendFilter(e.target.value)}>
              <option value="all">All backends</option>
              {backendOptions.map((b) => (
                <option key={b} value={b}>{b}</option>
              ))}
            </select>
            <select value={statusFilter} onChange={(e) => setStatusFilter(e.target.value as 'all' | 'success' | 'error')}>
              <option value="all">All statuses</option>
              <option value="success">Success only</option>
              <option value="error">Errors only</option>
            </select>
          </div>
        )}

        <button
          className="history-panel-download-btn"
          onClick={handleDownload}
          disabled={downloading || total === 0}
          title="Download the full history (all columns) as a CSV file"
        >
          {downloading ? 'Downloading…' : 'Download History'}
        </button>

        <button
          className="history-panel-clear-btn"
          onClick={handleClear}
          disabled={clearing || total === 0}
          title="Delete all stored telemetry history"
        >
          {clearing ? 'Clearing…' : 'Clear History'}
        </button>
      </div>

      <div className="history-panel-note">
        Prompt/response previews are <strong>off by default</strong> and only stored if
        <code> telemetry.history.store_previews</code> is explicitly enabled in config.json. Only token counts,
        latency, and routing metadata are recorded by default.
      </div>

      {error && <div className="left-panel-empty-state">Error: {error}</div>}

      {view === 'table' ? (
        <>
          {loading && records.length === 0 ? (
            <div className="left-panel-empty-state">Loading history…</div>
          ) : visibleRecords.length === 0 ? (
            <div className="left-panel-empty-state">No requests match the current filters.</div>
          ) : (
            <div className="history-panel-table-wrap">
              <table className="history-panel-table">
                <thead>
                  <tr>
                    <th onClick={() => toggleSort('timestamp_ms')}>Time</th>
                    <th onClick={() => toggleSort('model_name')}>Model</th>
                    <th onClick={() => toggleSort('tokens_per_second')}>Tokens/s</th>
                  </tr>
                </thead>
                <tbody>
                  {visibleRecords.map((r) => (
                    <tr
                      key={r.id}
                      className={r.success ? '' : 'history-panel-row-error'}
                      title={rowTooltip(r)}
                    >
                      <td>{formatTime(r.timestamp_ms)}</td>
                      <td>{r.model_name || '—'}</td>
                      <td>{formatNum(r.tokens_per_second)}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}

          <div className="history-panel-pagination">
            <button disabled={offset === 0 || loading} onClick={() => fetchHistory(Math.max(0, offset - PAGE_SIZE))}>
              Previous
            </button>
            <span>
              {total === 0 ? '0' : `${offset + 1}-${Math.min(offset + PAGE_SIZE, total)}`} of {total}
            </span>
            <button disabled={offset + PAGE_SIZE >= total || loading} onClick={() => fetchHistory(offset + PAGE_SIZE)}>
              Next
            </button>
          </div>
        </>
      ) : (
        <HistoryCharts summary={summary} loading={summaryLoading} />
      )}
    </div>
  );
};

// --- Hand-rolled SVG charts -------------------------------------------------
// No charting library is a dependency of this codebase; these are simple
// enough (a couple of bar charts) that adding one wasn't justified.

const HistoryCharts: React.FC<{ summary: SummaryResponse | null; loading: boolean }> = ({ summary, loading }) => {
  if (loading && !summary) {
    return <div className="left-panel-empty-state">Loading charts…</div>;
  }
  if (!summary) {
    return <div className="left-panel-empty-state">No summary data available.</div>;
  }
  if (summary.overall.total_requests === 0) {
    return <div className="left-panel-empty-state">No requests recorded yet.</div>;
  }

  return (
    <div className="history-panel-charts">
      <div className="history-panel-chart-card">
        <div className="history-panel-chart-title">Tokens/sec by model</div>
        <BarChart
          data={summary.avg_tokens_per_second_by_model.map((d) => ({ label: d.model, value: d.avg_tokens_per_second }))}
          valueFormat={(v) => v.toFixed(1)}
        />
      </div>

      <div className="history-panel-chart-card">
        <div className="history-panel-chart-title">Requests per day</div>
        <BarChart
          data={summary.requests_by_day.map((d) => ({ label: d.date, value: d.count }))}
          valueFormat={(v) => String(Math.round(v))}
        />
      </div>

      <div className="history-panel-chart-card">
        <div className="history-panel-chart-title">
          Error rate per day (overall: {(summary.overall.error_rate * 100).toFixed(1)}%)
        </div>
        <BarChart
          data={summary.requests_by_day.map((d) => ({ label: d.date, value: d.error_rate * 100 }))}
          valueFormat={(v) => `${v.toFixed(1)}%`}
          barColor="var(--error-color)"
        />
      </div>
    </div>
  );
};

const BarChart: React.FC<{
  data: { label: string; value: number }[];
  valueFormat: (v: number) => string;
  barColor?: string;
}> = ({ data, valueFormat, barColor }) => {
  if (data.length === 0) {
    return <div className="history-panel-chart-empty">No data</div>;
  }
  const max = Math.max(...data.map((d) => d.value), 0.0001);
  const width = 320;
  const height = 120;
  const barGap = 4;
  const barWidth = Math.max(2, width / data.length - barGap);

  return (
    <svg viewBox={`0 0 ${width} ${height}`} className="history-panel-chart-svg" preserveAspectRatio="none">
      {data.map((d, i) => {
        const barHeight = max > 0 ? (d.value / max) * (height - 20) : 0;
        const x = i * (barWidth + barGap);
        const y = height - 16 - barHeight;
        return (
          <g key={`${d.label}-${i}`}>
            <rect
              x={x}
              y={y}
              width={barWidth}
              height={Math.max(1, barHeight)}
              fill={barColor || 'var(--info-color)'}
              rx={1.5}
            >
              <title>{`${d.label}: ${valueFormat(d.value)}`}</title>
            </rect>
          </g>
        );
      })}
    </svg>
  );
};

export default HistoryPanel;
