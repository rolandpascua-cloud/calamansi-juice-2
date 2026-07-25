# System State Viewer

!!! note "Calamansi Juice 2 addition"
    This page documents a fork-only addition on top of upstream Lemonade v11.0.0. See [MERGING.md](https://github.com/rolandpascua-cloud/calamansi-juice-2/blob/main/MERGING.md) for how fork-specific docs like this one are tracked against future `git merge upstream/main` operations.

The System State Viewer is a point-in-time "what am I running on, and what's installed" inspector — hardware, OS/kernel, firmware, and the AI software stack. It's distinct from the live-updating [Resource Dashboard](resources.md): this is a snapshot refreshed on demand, not a polled dashboard.

## API

### `GET /api/v1/system/state`

Cached server-side for ~60 seconds; pass `?refresh=true` to force a fresh snapshot.

Every leaf value follows one of two shapes:

- **Available**: `{"available": true, "value": ..., ...}` (extra fields vary by section — e.g. `hardware.uma` also carries `active_param`).
- **Unavailable**: `{"available": false, "reason": "<why>"}` — the `reason` distinguishes *why*: tool not installed, requires root, not applicable on this platform, or a detection exception. A degraded field is never a blank or an error — this is the whole point of the feature.

Top-level sections, each independently collected so one failing collector never blanks the rest of the response:

- **`hardware`**: `cpu`, `gpu`, `npu` (reused from the existing `/system-info` device-detection logic), `memory` (total RAM), and `uma` — the unified-memory config. On Strix Halo-class hardware, "how much VRAM" is a kernel-parameter question (`amdgpu.gttsize` vs `ttm.pages_limit` on `/proc/cmdline`), not a fixed number — `uma` surfaces which one is active and its value, or that module defaults apply if neither is set.
- **`os_kernel`**: `distro` (from `/etc/os-release`), `kernel_version` (from `/proc/version`), `boot_mode` (systemd-boot/BLS vs GRUB, detected from `/boot/loader/entries` / `/boot/grub` rather than assumed — this varies distro to distro), `amd_iommu` (cmdline state; flagged if `off`, since that disables the NPU).
- **`firmware`**: `bios` (version + release date via `dmidecode -t bios`, labeled `"unavailable - requires root"` when permission is denied rather than erroring), `gpu_vbios` (via `amd-smi static -I` / `rocm-smi --showvbios`, whichever is installed).
- **`ai_stack`**: `rocm_version`, `hip_version`, `mesa_version`, `python_version`, `backends` (best-effort installed versions for `llamacpp`, `vllm`, `fastflowlm`, `openmoss`, `trellis` — reused from the existing recipe/backend version-tracking already computed for `/system-info`), and `app_version` (this app's own version plus the upstream Lemonade version it tracks, parsed from the `<upstream>+cj<fork>` version string).
- **`driver_stack`**: best-effort presence checks for `asusctl`, `supergfxctl`, `z13ctl` (Strix Halo Z13-specific tooling) and `tuned-adm` — never a hard dependency for this feature. `z13ctl` additionally carries a `status` field with the raw `z13ctl status` output (fan speed/mode, TDP limits, power profile, battery charge limit), and `tuned-adm` carries `active_profile` with the currently-active TuneD profile name (parsed from `tuned-adm active`).

Example (abbreviated):

```json
{
  "generated_at_ms": 1732000000000,
  "cached": false,
  "hardware": {
    "cpu": {"available": true, "name": "AMD RYZEN AI MAX+ 395", "cores": 16, "threads": 32, "family": "x86_64"},
    "gpu": {"available": true, "name": "110501", "family": "gfx1151", "integrated": true, "vram_gb": 0.5, "virtual_mem_gb": 95.2},
    "npu": {"available": true, "family": "XDNA2", "tops_max_int": 58, "utilization": 0.0},
    "memory": {"available": true, "total": "124.95 GB"},
    "uma": {"available": true, "active_param": "ttm.pages_limit", "value": "24967037"}
  },
  "firmware": {
    "bios": {"available": false, "reason": "unavailable - requires root"},
    "gpu_vbios": {"available": true, "value": "00107962"}
  }
}
```

## Authentication

Same bearer-token auth as the rest of the REST API — no per-request access scoping (unlike [Telemetry History](history.md)), since this is a machine-wide snapshot rather than per-user data.

## GUI

A "System" tab in the left-panel rail, alongside History. Four collapsible cards (Hardware, OS/Kernel, Firmware, AI Software Stack) plus a Driver Stack card. Each unavailable field is shown as "Unavailable" with a hover tooltip explaining why. "Copy as text" and "Export JSON" buttons cover the pasting-into-a-bug-report use case. The panel refreshes on tab open and via an explicit "Refresh" button only — it is deliberately **not** polled automatically, since it's a snapshot/inspector, not a live dashboard.

## Installing the Driver Stack tools

`z13ctl` and `tuned` are both optional — the Driver Stack card just shows "Not installed" for either one if it's missing, same as `asusctl`/`supergfxctl`. Installing them gets you their live status (fan/TDP/power profile for `z13ctl`, the active TuneD profile for `tuned-adm`) instead.

### z13ctl

[`z13ctl`](https://github.com/dahui/z13ctl) is a CLI/daemon for ASUS ROG Flow Z13 hardware control (RGB lighting, performance profiles, battery charge limit, fan curves, TDP, undervolting). Debian/Ubuntu install:

```sh
# Download the latest z13ctl_*_linux_amd64.deb from:
# https://github.com/dahui/z13ctl/releases
sudo apt install ./z13ctl_*.deb

# The .deb installs udev rules, systemd units, and the battery permissions
# service automatically — you just need group membership to use it:
sudo usermod -aG users $USER
# log out and back in for the group membership to take effect

# Verify:
z13ctl list
```

See the [z13ctl installation guide](https://dahui.github.io/z13ctl/installation/) for Arch (AUR `z13ctl-bin`), Fedora/RHEL (`.rpm`), building from source, and the optional `ryzen_smu` kernel module (only needed for undervolting).

### tuned

[`tuned`](https://tuned-project.org/) is a general Linux system-tuning daemon (not Z13-specific) that manages performance/power profiles:

```sh
sudo apt update && sudo apt install tuned -y
sudo systemctl enable --now tuned
sudo tuned-adm profile accelerator-performance
tuned-adm active
```

`accelerator-performance` is a reasonable default for a machine doing local LLM inference; run `tuned-adm list` to see other available profiles (e.g. `balanced`, `powersave`) if you want to trade performance for battery life.
