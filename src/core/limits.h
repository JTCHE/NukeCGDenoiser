// Single source of truth for every bound the plugin enforces.
//
// These values are referenced by knob ranges, _validate() clamps, split_input()
// and the tooltips. Changing one here changes all of them; there is deliberately
// nowhere else in the codebase that hardcodes a limit.
#pragma once

namespace denoiser {

// Temporal window. Each extra frame is another full-res fetch of the input, so
// cost is linear in 2*radius+1 — radius 3 is already 7x. Past one frame the
// motion vectors are extrapolated linearly, which stops being believable first.
constexpr int kMinTemporalRadius = 1;
constexpr int kMaxTemporalRadius = 3;

// Number of times the image is pushed through the denoise filter.
constexpr int kMinRuns = 1;
constexpr int kMaxRuns = 32;

// A motion layer needs at least a u and a v to warp with.
constexpr int kMinMotionChannels = 2;
// Forward u/v plus backward u/v.
constexpr int kMaxMotionChannels = 4;

} // namespace denoiser
