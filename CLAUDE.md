# NukeCGDenoiser

Nuke plugin for Windows and macOS. Spatial + temporal CG denoising via Intel Open Image
Denoise (OIDN 2.1.0 on Windows, 2.3.3 on macOS), built from source against whatever Nuke SDK
is installed.

## Build and install

```bash
install.bat
```

Then restart Nuke. That is the whole loop — the script detects a Nuke SDK, downloads and
caches CMake / VS2022 Build Tools / OIDN if missing, builds, and stages into
`~/.nuke/nuke-denoiser/`. Idempotent, safe to re-run. First run takes 10-20 min if it has
to fetch Build Tools; ~1 min after that.

Pass `-NukePath "C:\Program Files\NukeX.Yvz"` to skip the SDK picker, `-Unattended` to
install Build Tools without prompting.

On macOS: `./install.sh [--nuke-path /Applications/Nuke17.1v2/Nuke17.1v2.app]`. It needs the
Xcode Command Line Tools and CMake, fetches OIDN 2.3.3 (2.1.0 has no arm64 build) and stages
`denoiser.dylib` plus `oidn/lib/`. It does not stage OIDN's `libtbb`: OIDN resolves
`@rpath/libtbb.12.dylib` to Nuke's own copy. Never pick an OIDN whose TBB is newer than the
`libtbb.12.*.dylib` in Nuke's `Contents/MacOS`.

Both installers also run from the web (`irm ... | iex`, `bash -c "$(curl ...)"`): with no repo
next to the script, they download `main` into the temp folder and run that copy.

### Iterating on C++ (the fast loop, ~30 s)

1. Edit `src/`
2. `cmake --build build --config Release`
3. `cp build/lib/Release/Denoiser.dll ~/.nuke/nuke-denoiser/denoiser.dll`
4. Restart Nuke

### Manual first-time configure

```bash
cmake -B build -G "Visual Studio 17 2022" -DOIDN_ROOT="c:/bin/oidn-2.1.0" -DCMAKE_PREFIX_PATH="C:/Program Files/NukeX.Yvz"
```

Two hard constraints, both non-obvious:

- **Visual Studio 2022 generator only.** MinGW/GCC cannot link the MSVC `.lib` import
  libraries Nuke ships.
- **OIDN 2.1.0 exactly.** 2.4.x ships TBB v2022.3, which collides with Nuke's v2020.3 and
  crashes on startup.

### Uninstall

```bash
python uninstall.py
```

## Where code goes

`src/denoiser.cpp` holds **only** the DDImage glue: knobs, validation, requests, and
orchestration of `renderStripe`. Everything else has exactly one owner:

| File | Owns |
|---|---|
| `src/core/limits.h` | Every bound the plugin enforces (radius, runs, channel counts) |
| `src/core/aov_patterns.*` | Renderer AOV name heuristics |
| `src/core/channel_io.*` | Channel ↔ buffer transfer, and the channel-ordering rule |
| `src/core/temporal.*` | Temporal accumulation. Pure — no DDImage, OIDN or Win32 |
| `src/core/oidn_device.*` | The shared OIDN device, its lock, and the device fallback order |
| `src/test/test_temporal.cpp` | Standalone assertions over `core/temporal` |

Rules when adding code:

1. A limit or constant goes in `limits.h`, never inline at a use site.
2. Anything that can avoid a Nuke dependency should — that is what makes it testable.
3. An inactive or blocked path returns its input unchanged, never an error or zeros.

## Testing

### Fast: the pure core, no Nuke, ~1 s

```bash
cmake --build build --config Release --target temporal_test
```

Runs 12 assertions over sequence boundaries, degenerate motion vectors, and the
safe-default contract. Run this first — it caught a segfault that looked like a threading
bug and was neither.

### Full: headless Nuke edge cases, ~2 min

`src/test/temporal_suite.py` generates a noisy multi-layer EXR sequence with known motion,
then checks RMSE against the clean reference: spatial beats noisy, temporal beats spatial on
every frame (sequence ends included), render order does not change pixels, inactive paths
equal spatial-only, wild vectors stay finite, an expanded bbox and three concurrent
Denoisers render a full range. Current state: 25/25 on Windows (CPU, CUDA) and macOS
(CPU, Metal; run it with `-t -i` there).

```bash
SUITE_DIR=<empty folder> SUITE_DEVICES=CPU,CUDA "C:/Program Files/Nuke16.0v8/Nuke16.0.exe" -ti src/test/temporal_suite.py
```

Install into a scratch `NUKE_PATH` first, so the run does not touch your own `~/.nuke`.

Three things will waste an hour if you do not know them:

1. **`-ti`, not `-t`.** Terminal mode needs a render licence; `-i` borrows the interactive
   one. Plain `-t` fails with `ENT_STATUS_RLM_LICENSE_NOPRODUCT`.
2. **Do not pass bare numeric args after the script** — Nuke eats them as a frame range.
   Use environment variables, and not `TEMP`, which Windows already defines.
3. **Call `forceValidate()` on a Read before setting layer knobs.** Until it evaluates,
   its channels do not exist and `setValue("mylayer")` silently does nothing.

### Manual, in the GUI

- Multi-layer EXR into a Denoiser, check albedo/normal auto-detected
- Frame range render for device state leaks
- CUDA path (needs driver 522.06+)

## Temporal denoising

**The one design decision everything follows from: the temporal stage is stateless per
frame.** Frames t±N are pulled on demand; nothing is carried between `renderStripe` calls.

This rules out OIDN 3's and OptiX's native temporal modes, which need the previous
*denoised* frame and so require strict ascending frame order. Nuke does not promise that —
it scrubs backwards, renders out of order, and re-evaluates on knob changes. A node
carrying that state gives different pixels depending on how you reached the frame. Being
stateless also lets us use *future* frames, which the real-time models cannot.

Pipeline: warp t±N onto t using t's motion vectors → clamp each warped sample into the
local 3×3 colour range of t → average → hand to the spatial denoiser.

The clamp is the load-bearing part. A plain average ghosts on disocclusion; the clamp
pulls bad warps back to something plausible. Standard TAA neighbourhood clamping, and it
generalises past 3 frames where a median does not. Running temporal *before* the spatial
denoiser lowers input variance, so OIDN blurs less — that is where detail retention comes
from.

Frames arrive via `split_input()` / `inputContext()` over input 0 (the NDK's
`TemporalMedian.cpp` pattern). Split offset 0 is the current frame, so plain `input(0)`
call sites are unaffected.

Three traps, all of which have already bitten:

1. **Knobs read by `split_input()` need `Knob::EARLY_STORE`.** Without it `split_input()`
   sees the constructor default and never allocates the extra inputs.
2. **`Op::input(n, offset)` is unchecked.** Reading past the split array segfaults. Go
   through `neighbourInput()`, which bounds against `inputs()` — the knobs can ask for
   more splits than Nuke has actually allocated.
3. **Never call `request()` or `tryValidate()` from `renderStripe`.** Both mutate the
   graph, and doing it to split inputs from a render thread races. `getRequests()` is the
   only place allowed to ask for pixels.

`mv_scale` and `mv_flip_v` are calibration, not configuration: renderers disagree about
motion vector units and Y direction, and it cannot be detected from the data. Do not
remove them in the name of simplicity.

## Coordinate spaces (read before touching write-out)

`plane.bounds()` is **not** clamped to `info().format()`. Nuke hands over a plane larger
than the format whenever an upstream node expands the bounding box.

The OIDN buffers are sized by the format. Indexing them with plane coordinates therefore
reads off the end — a heap out-of-bounds that corrupts allocator state rather than
faulting, so Nuke dies later somewhere unrelated. This shipped for a long time and made
batch rendering crash roughly 60% of the time.

`writeChannelFromBuffer()` in `denoiser.cpp` is the only sanctioned way to move pixels
from a format-sized buffer into a plane. Use it.

## Channel ordering

A `ChannelSet` iterates in channel-number order, which is red/green/blue **only by luck**.
For a layer whose channels Nuke registered alphabetically it comes out blue/green/red.

`fetchInterleaved()` places each channel at its `colourIndex()` within its own layer, so
RGB stays RGB whatever the EXR did. `Packing::Sequential` exists for the one case that
wants iteration order — pulling a single arbitrary channel, where alpha's colour index of
3 would send it off the end of a one-channel buffer.

Before this was fixed, albedo and normal reached OIDN with R and B swapped. Silently.

## Node behaviour

**Inputs:** 0 beauty (required), 1 albedo (optional), 2 normal (optional). Connecting 1 or
2 disables the matching layer picker and reads `RGB` directly.

**Output:** beauty denoised into RGB, original layer preserved; alpha denoised in its own
pass (OIDN works 3 channels at a time); all other layers pass through untouched.

**Auto-detect:** `autoDetectAOVs()` fills the albedo/normal/motion pickers from renderer
AOV names, only while a knob is still untouched. Dispatched from `knob_changed()` on
`Knob::inputChange` / `Knob::showPanel` — GUI only, does not fire headless. Setting knob
values from `_validate()` was tried and rejected; Nuke logs "Setting knob values from
validate is not supported".

**Device fallback:** if the requested device fails, `device::ensure()` tries the other GPUs
(macOS: Metal, then CUDA), then CPU; the knob is set to the device in use, with a warning.
"CUDA" stays in the macOS device list so scripts saved on Windows still load.

## Known limitations

- **CUDA needs driver 522.06+.** No HIP or SYCL.
- **OIDN buffers are device-only; never call `getData()` on them.** Default
  `newBuffer()` storage on CUDA is managed memory, and on Windows the CPU may not touch
  managed memory while any kernel runs on the GPU. With several Denoisers rendering at
  once that crashed with 0xC0000006 (it was the "CUDA is unstable" reputation), and it
  ran 6-7x slower. Stage pixels in host vectors and move them with `write()`/`read()`.
- **No Linux installer.** The lock has a pthread path (recursive mutex, static
  initializer) used on macOS; Linux builds are untested.
- **Inputs must match resolution.** Reformat beauty/albedo/normal to match first.
- **Temporal costs 2N+1 full-res fetches per frame.** Radius 3 is 7×. Compounds with the
  lack of tiling — peak memory scales with the window.
- **TBB version lock.** See the OIDN 2.1.0 constraint above.

## Debugging

**Plugin will not load:** check `oidn/bin/` has every runtime DLL, confirm `init.py` runs
(it must call `nuke.load('denoiser')`, not `menu.py`, or terminal mode breaks), read the
script editor console.

**CUDA will not initialise:** `nvidia-smi` for the driver version, confirm
`OpenImageDenoise_device_cuda.dll` is staged. The plugin falls back to CPU and says so.

**Random crashes at unrelated points:** that signature is memory corruption, not logic —
if the failure point moves between runs while assertions keep passing, stop reading the
logic and go looking for an out-of-bounds write. Check the coordinate-space section above
first. `/fsanitize=address` on the plugin names the line immediately; guessing does not.

**Thread safety:** all access to the shared device goes through `core/oidn_device.h` and
requires holding a `device::Lock`.
