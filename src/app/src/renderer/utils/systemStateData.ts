import { serverFetch } from './serverConfig';

// Calamansi Juice 2 addition: fetch helper for GET /system/state (the
// System State Viewer's point-in-time hardware/OS/firmware/AI-stack
// snapshot). Distinct from useSystem.tsx/systemData.ts, which cover the
// live /system-info + /system-stats endpoints.

export interface Field {
  available: boolean;
  reason?: string;
  value?: unknown;
  [key: string]: unknown;
}

export interface SystemStateSnapshot {
  generated_at_ms: number;
  cached: boolean;
  hardware: {
    cpu: { available: boolean; name?: string; cores?: number; threads?: number; family?: string; error?: string };
    gpu: { available: boolean; name?: string; family?: string; integrated?: boolean; vram_gb?: number; virtual_mem_gb?: number; reason?: string };
    npu: { available: boolean; name?: string; family?: string; tops_max_int?: number; utilization?: number; power_mode?: string; reason?: string };
    memory: Field;
    uma: Field;
  };
  os_kernel: {
    distro: Field;
    kernel_version: Field;
    boot_mode: Field;
    amd_iommu: Field;
  };
  firmware: {
    bios: Field;
    gpu_vbios: Field;
  };
  ai_stack: {
    rocm_version: Field;
    hip_version: Field;
    mesa_version: Field;
    python_version: Field;
    backends: Record<string, Field>;
    app_version: { full_version: string; calamansi_version: string | null; tracks_upstream_lemonade: string };
  };
  driver_stack: Record<string, { installed: boolean; status?: Field; active_profile?: Field }>;
}

export async function fetchSystemState(refresh = false): Promise<SystemStateSnapshot> {
  const response = await serverFetch(`/system/state${refresh ? '?refresh=true' : ''}`);
  if (!response.ok) {
    throw new Error(`HTTP ${response.status}`);
  }
  return response.json();
}
