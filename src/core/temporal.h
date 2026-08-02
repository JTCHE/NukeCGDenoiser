// Motion-compensated temporal accumulation.
//
// Deliberately free of any DDImage, OIDN or Windows dependency: this operates on
// plain interleaved float buffers so the algorithm can be exercised without
// launching Nuke (see src/test/test_temporal.cpp). Nothing in here knows what a
// knob or an Iop is; the Nuke layer is responsible for fetching pixels and
// handing them over.
#pragma once

#include <vector>

namespace denoiser {

struct TemporalParams
{
	float strength = 1.f; // 0 = leave the frame untouched, 1 = full accumulation
	float mvScale = 1.f;  // renderers disagree about motion vector units
	bool flipV = false;   // ...and about which way Y points
};

// Accumulates neighbouring frames onto a current frame, one at a time so the
// caller never has to hold the whole window in memory.
//
// Every frame is warped into the current frame using the *current* frame's motion
// vectors, then clamped into the local 3x3 colour range of the current frame
// before it is allowed to contribute. That clamp is what stops a disocclusion
// from ghosting: a sample that lands on unrelated geometry gets pulled back to
// something plausible instead of smearing across the edge.
//
// Safe defaults, per the isolation rule: if no frame is ever added, or the motion
// vectors are unusable, or strength is 0, finish() reproduces the input exactly.
class TemporalAccumulator
{
public:
	// `colour` is width*height*3 interleaved and must outlive this object.
	// `mv` is width*height*mvChans interleaved: [fu, fv] or [fu, fv, bu, bv].
	// Passing mv == nullptr, or mvChans < 2, makes every addFrame() a no-op.
	TemporalAccumulator(const float* colour, unsigned width, unsigned height,
	                    const float* mv, int mvChans, const TemporalParams& params);

	// `delta` is the frame offset (negative = past, positive = future, never 0).
	// `rgb` is width*height*3 interleaved, or nullptr to skip this frame — which
	// is how the head and tail of a sequence degrade smoothly to spatial-only
	// instead of blending in black.
	void addFrame(int delta, const float* rgb);

	// Blends the accumulated result over the original by `strength` and writes it
	// to `out` (width*height*3). Writing back over the input buffer is allowed.
	void finish(float* out) const;

	// True when the motion data was usable at construction time. Callers can skip
	// the whole fetch loop when this is false.
	bool usable() const { return m_usable; }

private:
	const float* m_colour;
	const float* m_mv;
	unsigned m_width;
	unsigned m_height;
	int m_mvChans;
	bool m_usable;
	TemporalParams m_params;

	std::vector<float> m_acc;  // running sum, width*height*3
	std::vector<float> m_wsum; // contributing frame count per pixel
};

} // namespace denoiser
