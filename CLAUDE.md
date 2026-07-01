## Project Overview

A build-from-source Windows-only Nuke plugin for denoising CG renders using Intel Open Image Denoise (OIDN 2.1.0). Includes:
- C++ plugin DLL (uses Nuke's DDImage SDK and OIDN C++ API)
- `install.ps1`/`install.bat`: detects a Nuke SDK, bootstraps CMake/VS2022 Build Tools/OIDN if missing, builds the plugin, and stages it into `~/.nuke/nuke-denoiser/`
- Python uninstaller
- Multi-layer EXR workflow with layer picker knobs for beauty/albedo/normal passes

**Fork improvements over original:** Stability fixes for multi-threaded rendering, Windows-specific distribution, and a self-contained build-from-source installer.

## Commands

### Build & Install (one step)

```bash
install.bat
```

or directly:

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1
```

This detects installed Nuke SDKs (prompting to pick one if more than one is found; pass
`-NukePath "C:\Program Files\NukeX.Yvz"` to skip the prompt), downloads/caches CMake and
OIDN 2.1.0 if not already present, bootstraps VS2022 C++ Build Tools if missing (prompts
before downloading — pass `-Unattended` for a silent install), builds `Denoiser.dll`, and
stages only the runtime files (`denoiser.dll`, `init.py`, `menu.py`, `oidn/bin/*.dll`) into
`~/.nuke/nuke-denoiser/` (respects `NUKE_PATH` like before). Patches `~/.nuke/init.py` with
a guarded block. Idempotent — safe to re-run.

After install, restart Nuke to load the plugin.

### Manual build (for contributors hacking on the source)

**Requires:** Visual Studio 2022 (not MinGW), CMake 3.10+, OIDN 2.1.0 SDK

```bash
cmake -B build -G "Visual Studio 17 2022" -DOIDN_ROOT="c:/bin/oidn-2.1.0" -DCMAKE_PREFIX_PATH="C:/Program Files/NukeX.Yvz"
cmake --build build --config Release
```

**Important:** Only Visual Studio 2022 generator works. MinGW/GCC cannot link MSVC `.lib` import libraries that Nuke ships with.

**OIDN 2.1.0 is mandatory.** Do not use OIDN 2.4.x — it ships with TBB v2022.3, which conflicts with Nuke's v2020.3 and causes immediate crashes.

### Uninstall

```bash
python uninstall.py
```

Removes the `init.py` entry and optionally deletes the plugin folder.

## Architecture

### C++ Plugin (`src/denoiser.cpp` / `src/denoiser.h`)

**Key Classes:**
- `DenoiserIop` — Main Nuke node class extending `PlanarIop`

**Critical Design Patterns:**

1. **Global OIDN device** — All `DenoiserIop` instances share a single `g_device` to avoid per-instance construction/destruction races during multi-threaded Nuke renders.

2. **`CRITICAL_SECTION` instead of `std::mutex`** — `std::mutex` global constructors do not reliably run when Nuke loads the DLL via `LoadLibrary`. This causes segfaults when locking an uninitialized mutex. `CRITICAL_SECTION g_cs` is initialized explicitly in `DllMain(DLL_PROCESS_ATTACH)` before any plugin code runs.

3. **`DllMain` initialization** — Initializes `g_cs` on attach, releases the device and deletes `g_cs` on detach. Guaranteed to run before any plugin callbacks.

4. **Device fallback logic** — If CUDA device fails, the plugin silently falls back to CPU and prints a warning. This lets users without CUDA drivers still use the plugin.

**Per-Instance State:**
- Device type, quality mode, threading options, memory limit, number of runs
- Beauty/albedo/normal channel selectors
- HDR, thread affinity, auxiliary prefilter flags

**Nuke Integration:**
- Extends `PlanarIop` (planar image operations in Nuke)
- `renderStripe()` calls `setupDevice()`, then encodes image data into OIDN buffers, runs filters, and decodes back
- Layer picker knobs: beauty/albedo/normal map EXR layer names to OIDN's three input types
- Auto-detect AOVs: `autoDetectAOVs()` scans input 0's layer names against a priority-ordered
  regex list (`kAlbedoPatterns`/`kNormalPatterns` in `denoiser.cpp`) covering common
  renderer AOV naming (Arnold `diffuse_albedo`/`N`, Cycles denoising passes, Karma,
  V-Ray, Redshift, RenderMan, Corona) and auto-fills the albedo/normal layer knobs on
  a match. Only fires while a knob is still at its untouched default, so it won't
  clobber a manual choice — though this means it can't distinguish "user explicitly
  chose none" from "never touched". Hooked via `knob_changed()`'s `Knob::inputChange`/
  `Knob::showPanel` pseudo-knobs (same pattern Cryptomatte uses) — this dispatch is
  GUI-only and does not fire in headless/terminal (`-t`) mode, confirmed by testing;
  setting knob values from `_validate()` was tried and rejected because Nuke logs
  "Setting knob values from validate is not supported and may cause unexpected behaviour".
- Alpha denoising: separate pass (OIDN processes 3 channels at a time)

### Distribution (`init.py`, `menu.py`, `install.ps1`, `install.bat`, `uninstall.py`)

**`init.py`** — Pre-loads OIDN runtime DLLs from the staged `oidn/bin/` folder (populated by `install.ps1` at install time, not tracked in git):
- Uses `SetDllDirectoryW()` to set the DLL search path relative to the plugin location (self-contained, no hardcoded paths)
- Loads CPU device DLLs unconditionally
- Loads CUDA device DLL if present (`OpenImageDenoise_device_cuda.dll`)
- Calls `nuke.load('denoiser')` to load the plugin (must be in init.py, not menu.py, for terminal mode support)

**`menu.py`** — Adds "MW/Denoiser" menu entry (GUI only).

**`install.ps1`** (invoked via `install.bat`) — Detects/bootstraps CMake, VS2022 Build Tools,
and OIDN 2.1.0; builds `Denoiser.dll` against a detected/selected Nuke SDK; stages the
runtime-only file set into `~/.nuke/nuke-denoiser/`; patches `~/.nuke/init.py` with a
guarded block (`# --- nuke-denoiser START/END ---`). Handles cleanup of old denoiser
entries. Idempotent.

**`uninstall.py`** — Removes the init.py block and optionally deletes the plugin folder.

## File Structure

```
NukeCGDenoiser/
├── CMakeLists.txt                 # Build config (Visual Studio 2022, links OIDN)
├── cmake/Modules/
│   └── FindOpenImageDenoise.cmake  # Custom CMake module for OIDN discovery
├── src/
│   ├── denoiser.cpp               # Main plugin implementation
│   └── denoiser.h                 # Plugin class definition
├── init.py                        # DLL pre-loading and nuke.load()
├── menu.py                        # Menu entry (GUI only)
├── install.ps1                    # Build + install: detects/bootstraps deps, builds, stages
├── install.bat                    # Thin wrapper that runs install.ps1
├── uninstall.py                   # Uninstaller script
└── example/
    └── test_render.py             # Example usage (denoises a test EXR)
```

`denoiser.dll` and `oidn/bin/*.dll` are no longer tracked in git — `install.ps1` builds/downloads
them into `build/` and stages copies into `~/.nuke/nuke-denoiser/`.

## Development Workflows

### Modifying C++ Code

1. Edit `src/denoiser.cpp` or `src/denoiser.h`
2. Rebuild: `cmake --build build --config Release`, then re-run `install.bat` to restage
   (or `copy build\lib\Release\Denoiser.dll ~\.nuke\nuke-denoiser\denoiser.dll` directly)
3. Restart Nuke to reload the DLL
4. Test in Nuke GUI (create a Denoiser node, connect test renders)

### Adding/Modifying Knobs

Edit `DenoiserIop::knobs()` in `src/denoiser.cpp`. The knob definition includes:
- Device type (CPU/CUDA, mapped to enum indices)
- Quality (Balanced/High, mapped to OIDN quality enum)
- HDR flag, thread affinity, memory limit, number of runs
- Layer picker knobs (beauty, albedo, normal)

### CUDA Support

`install.ps1` downloads the full OIDN 2.1.0 release package, which already includes
`OpenImageDenoise_device_cuda.dll`, and stages it into `oidn/bin/` alongside the CPU
device DLLs automatically — no manual step needed. GPU denoising still requires an
NVIDIA GPU and driver 522.06+ on the target machine. The `init.py` script checks if the
CUDA DLL exists in the staged folder and pre-loads it conditionally.

## Key Implementation Details

### Device Lifecycle

- `setupDevice(deviceType, numThreads, affinity)` is called once per render with the selected device type
- Double-check locking: check `g_deviceReady` outside the lock first, then again inside
- If device type changes between renders, the old device is released and a new one created
- Device parameters (numThreads, setAffinity) are CPU-only; OIDN silently ignores them on CUDA

### Input Mapping

- **Input 0:** Beauty (RGB, required)
- **Input 1:** Albedo (RGB, optional; layer picker disabled if connected)
- **Input 2:** Normal (RGB, optional; layer picker disabled if connected)

When all three are connected as separate images, layer pickers are disabled — the node reads `RGB` directly.

### Output

- Beauty layer is written in-place (original layer is preserved)
- Alpha is denoised in a separate pass and concatenated
- All other layers pass through unchanged (Shuffle-like behavior for multi-layer EXRs)

## Testing & Validation

No automated unit tests (plugin logic is tightly coupled to Nuke's DDImage API). Manual testing:

1. **Single-frame EXR:**
   - Load a beauty pass + optional albedo/normal in a Read node
   - Connect to Denoiser (or use Denoiser on a multi-layer EXR with layer pickers)
   - Set Device Type (CPU or CUDA if available)
   - Play or render a single frame; check the denoised output

2. **Animation/batch renders:**
   - Frame range tests to ensure no thread safety issues or device state leaks
   - Multi-threaded Nuke renders (test with `-x 4` for 4 concurrent threads)

3. **CUDA testing:**
   - Requires driver 522.06+ and CUDA DLL in `oidn/bin/`
   - Compare CUDA output quality and speed vs. CPU

## Known Limitations

- **CUDA is experimental.** Requires separate DLL and driver 522.06+. No HIP or SYCL support.
- **Windows only.** Installer is Windows-specific (PowerShell); C++ source is cross-platform but untested on other OSes.
- **Inputs must match resolution.** If beauty, albedo, and normal differ in size, reformat them first.
- **TBB version lock:** OIDN 2.1.0 uses TBB v2021.10, compatible with Nuke's v2020.3. Do not upgrade OIDN without checking TBB compatibility.

## Debugging

### DLL Loading Issues

If the plugin fails to load:
1. Check that all runtime DLLs are in `oidn/bin/` (use Windows API Monitor or Dependency Walker)
2. Verify `init.py` is being executed (add print statements; run Nuke in verbose mode)
3. Check Nuke's script editor console for error messages

### Device Init Failures

If CUDA device fails to initialize:
- Check driver version (`nvidia-smi` in cmd.exe)
- Verify `OpenImageDenoise_device_cuda.dll` exists in `oidn/bin/`
- The plugin falls back to CPU and prints `[Denoiser] CUDA unavailable — fell back to CPU`

### Thread Safety

Global OIDN device is protected by `CRITICAL_SECTION g_cs`. All access to `g_device` (read, write, teardown) must hold the lock. Check `setupDevice()` and `renderStripe()` for proper locking patterns.

## OIDN Version and Compatibility

- **Current:** OIDN 2.1.0 (uses `tbb12.dll` v2021.10)
- **Why not 2.4.x:** Newer versions ship with TBB v2022.3, which conflicts with Nuke's v2020.3 and crashes on startup
- **Why not older:** Older versions lack CUDA support or have other compatibility issues
- **Path:** `install.ps1` auto-downloads and caches it in `build/deps/oidn-2.1.0`; for a manual build, pass `-DOIDN_ROOT` pointing at any local OIDN 2.1.0 SDK extraction
