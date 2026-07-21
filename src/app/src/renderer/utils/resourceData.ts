import { serverFetch } from './serverConfig';

// Calamansi Juice 2 addition: fetch helpers for the live Resource Dashboard
// (GET /resources/system, GET /resources/inference). Response shapes
// closely mirror the amd-ai-max-dashboard reference project's /api/system
// and /api/inference JSON, per the feature's own design goal of reusable
// frontend polling/chart code.

export interface ResourcesSystem {
  cpu: {
    per_core_percent?: (number | null)[];
    aggregate_percent?: number | null;
    core_count_logical?: number | null;
    core_count_physical?: number | null;
    current_mhz?: number | null;
    min_mhz?: number | null;
    max_mhz?: number | null;
    load_average?: { '1m': number; '5m': number; '15m': number } | null;
    error?: string;
  };
  memory: {
    total?: number;
    used?: number;
    free?: number;
    available?: number;
    percent?: number | null;
    uma?: {
      amdgpu_gttsize: string | null;
      amdgpu_gttsize_set_on_cmdline: boolean;
      ttm_pages_limit: string | null;
      ttm_pages_limit_set_on_cmdline: boolean;
      note: string;
      error?: string;
    };
    error?: string;
  };
  gpu: {
    source?: string;
    utilization_percent?: number | null;
    vram_used_percent?: number | null;
    vram_total_bytes?: number | null;
    vram_used_bytes?: number | null;
    temperature_c?: number | null;
    power_watts?: number | null;
    error?: string;
  };
  npu: {
    amd_iommu_ok?: boolean | null;
    amd_iommu_note?: string;
    amdxdna_module_loaded?: boolean | null;
    present?: boolean | null;
    utilization_available?: boolean;
    utilization_note?: string;
    error?: string;
  };
  disk: {
    total?: number;
    used?: number;
    free?: number;
    percent?: number | null;
    btrfs_status?: string;
    btrfs_error?: string;
    error?: string;
  };
  sensors: Record<string, { label: string; current: number; high: number | null; critical: number | null }[]> & {
    note?: string;
    error?: string;
  };
  timestamp: number;
}

export interface InferenceServiceStatus {
  process_running?: boolean;
  cli_found?: boolean;
  note?: string;
  error?: string;
  [key: string]: unknown;
}

export interface ResourcesInference {
  lmstudio: InferenceServiceStatus;
  llama_server: InferenceServiceStatus;
  vllm: InferenceServiceStatus;
  fastflowlm: InferenceServiceStatus;
  timestamp: number;
}

export async function fetchResourcesSystem(): Promise<ResourcesSystem> {
  const response = await serverFetch('/resources/system');
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  return response.json();
}

export async function fetchResourcesInference(): Promise<ResourcesInference> {
  const response = await serverFetch('/resources/inference');
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  return response.json();
}

export function formatBytes(bytes?: number | null): string {
  if (bytes == null) return '—';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let value = bytes;
  let unitIndex = 0;
  while (value >= 1024 && unitIndex < units.length - 1) {
    value /= 1024;
    unitIndex += 1;
  }
  return `${value.toFixed(1)} ${units[unitIndex]}`;
}
