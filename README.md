# Nuke Denoiser

![Noisy CG render next to the same render after the Denoiser node](denoiser_comparison.png)

**<p align="center">Denoise CG renders in Nuke with Intel Open Image Denoise.</p>**

## Requirements

- **Windows 10/11 x64** (this fork is Windows-only)
- **Nuke 16** (tested on 16.0v8; other recent versions likely work)
- **CUDA** drivers for Nvidia GPUs (**Optional**, defaults to CPU denoising)

---

## Install

Close Nuke first.

### Windows

In PowerShell:

```powershell
irm https://raw.githubusercontent.com/JTCHE/NukeCGDenoiser/main/install.ps1 | iex
```

Then restart Nuke.

The installer:

- Downloads the source and finds your Nuke installs; pick one from the menu
- Fetches CMake, the VS2022 Build Tools and OIDN 2.1.0 if missing (first run: 10-20 min)
- Builds the plugin and copies it to `~/.nuke/nuke-denoiser/`
- Adds a `pluginAddPath` entry to `~/.nuke/init.py` and removes entries from older installs
- Is safe to run again

<details>
<summary>Manual</summary>

Clone and run the installer, with optional flags:

```bat
git clone https://github.com/JTCHE/NukeCGDenoiser
cd NukeCGDenoiser
install.bat -NukePath "C:\Program Files\Nuke16.0v8" -Unattended
```

`-NukePath` skips the Nuke picker. `-Unattended` installs the Build Tools without a prompt.

Or build by hand with [Visual Studio 2022](https://visualstudio.microsoft.com/) and
[CMake 3.10+](https://cmake.org/). **MinGW/GCC does not work**: Nuke ships MSVC import
libraries that GCC cannot link.

```bat
cmake -B build -G "Visual Studio 17 2022" -DOIDN_ROOT="c:/bin/oidn-2.1.0" -DCMAKE_PREFIX_PATH="C:/Program Files/Nuke16.0v8"
cmake --build build --config Release
copy build\lib\Release\Denoiser.dll %USERPROFILE%\.nuke\nuke-denoiser\denoiser.dll
```

Set `OIDN_ROOT` to the extracted [OIDN 2.1.0 release](https://github.com/RenderKit/oidn/releases/tag/v2.1.0).
**Do not use a newer OIDN**: see [What's different from the original](#whats-different-from-the-original).

</details>

### Uninstall

```
python uninstall.py
```

Removes the `init.py` entry and optionally sends the plugin folder to the Recycle Bin.

---

## Usage

1. In Nuke, Tab-search **`Denoiser`** _(or go to Nodes > MW > Denoiser)_
2. Connect your render and choose a workflow:

### Workflow A — Multi-layer EXR (recommended)

Place the Denoiser directly on a multi-layer EXR Read node. Use the **layer picker knobs** to select which passes to use:

- **Beauty layer** — the layer to denoise (default: `rgb`). The denoised result is written to the `rgb` channels; the original layer is left untouched.
- **Albedo layer** — albedo or diffuse pass for auxiliary-guided denoising. Set to `none` to skip.
- **Normal layer** — world-space normals for auxiliary-guided denoising. Set to `none` to skip. Must be accompanied by Albedo layer.

All other layers pass through unchanged, so the node works like a Shuffle + Denoise in one step.

### Workflow B — Separate inputs

![Denoiser Usage - Workflow B](denoiser_node_usage.png)

Connect separate images to each input:

| Input      | Content           | Required |
| ---------- | ----------------- | -------- |
| 0 - Beauty | Beauty pass (RGB) | ✅       |
| 1 - Albedo | Albedo AOV (RGB)  | ❌       |
| 2 - Normal | Normal AOV (RGB)  | ❌       |

When albedo or normal inputs are connected, the corresponding layer picker knobs are disabled — the node reads `RGB` directly from the connected input. This is useful for split EXRs, PNGs, or renders that output each AOV as a separate file.

### Alpha denoising

Enable the alpha checkbox (A) in the **Beauty layer** knob to also denoise the alpha channel. Alpha is denoised in a separate pass since OIDN processes 3 channels at a time.

### Properties

| Knob                           | Description                                                                                                                            |
| ------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------- |
| Device Type                    | **CPU** (default) or **CUDA**                                                                                                          |
| Quality                        | **Balanced** (default) or **High**                                                                                                     |
| HDR                            | Enable for high-dynamic-range input (recommended)                                                                                      |
| Enable thread affinity         | Pins OIDN threads to hardware threads for performance                                                                                  |
| Memory limit (MB)              | Cap OIDN memory usage; 0 = no limit                                                                                                    |
| Number of runs                 | Feed the image through the filter N times                                                                                              |
| Beauty / Albedo / Normal layer | Select which layer from the input EXR to use for each role. Albedo and Normal are disabled when their inputs are connected             |
| Prefilter Auxiliary Passes     | Tells OIDN to prefilter noisy albedo/normal before using them for guidance. Leave unchecked if your auxiliary passes are already clean |

---

## What's different from the original

The original plugin could freeze or crash the host machine when used with animation sequences or multiple concurrent frames. This fork fixes that and adds a proper Windows distribution.

**Stability fixes (C++):**

- Global OIDN device shared across all node instances — eliminates per-instance construction/destruction races during multi-threaded Nuke renders
- `CRITICAL_SECTION` instead of `std::mutex` for thread synchronization — `std::mutex` global constructors don't reliably run when a DLL is loaded via `LoadLibrary` inside Nuke, causing segfaults on lock
- `DllMain` initializes the critical section on `DLL_PROCESS_ATTACH`, guaranteed to run before any plugin callbacks
- OIDN DLL search path resolved relative to the plugin DLL at load time (`GetModuleFileNameW`) — no hardcoded install paths

**Distribution:**

- OIDN 2.1.0 CPU runtime DLLs bundled in `oidn/bin/` — no separate OIDN installation needed
- CUDA device DLL (`OpenImageDenoise_device_cuda.dll`) not bundled due to size; drop it in `oidn/bin/` to enable GPU denoising
- `install.py` / `install.bat` — one-click installer that copies the plugin and patches `~/.nuke/init.py`
- `uninstall.py` — clean removal

**Why OIDN 2.1.0 and not newer:**
OIDN 2.4.x ships with `tbb12.dll` v2022.3, which conflicts with Nuke's own `tbb.dll` v2020.3 when both are in the same process, causing immediate crashes. OIDN 2.1.0's `tbb12.dll` (v2021.10) is compatible with Nuke's TBB version.

---

## Known limitations

- **CUDA denoising is experimental.** Requires an NVIDIA GPU and driver 522.06+. Drop `OpenImageDenoise_device_cuda.dll` from the [OIDN 2.1.0 release](https://github.com/RenderKit/oidn/releases/tag/v2.1.0) into `oidn/bin/` to enable it. HIP and SYCL are not supported.
- **Windows only.** The original plugin supports Linux and macOS; this fork's distribution system is Windows-specific. The C++ source itself is cross-platform.
- **Inputs must match resolution.** If beauty, albedo, and normal are different sizes, reformat them to match before connecting.

---

## Acknowledgements

Forked from [NukeCGDenoiser](https://github.com/mateuszwojt/NukeCGDenoiser) by [Mateusz Wojt](https://github.com/mateuszwojt), licensed under the original project's license (see `LICENSE`).

Uses [Intel Open Image Denoise](https://github.com/RenderKit/oidn), © Intel Corporation.
