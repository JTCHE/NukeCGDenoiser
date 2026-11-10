// Copyright (c) 2021-2026 Mateusz Wojt
//
// Nuke integration layer. This file and denoiser.cpp own the DDImage glue only —
// knobs, validation, requests and plane I/O orchestration. The actual work lives
// in src/core/, which knows nothing about Nuke:
//
//   core/limits.h        every bound the plugin enforces
//   core/aov_patterns.h  renderer AOV name heuristics
//   core/channel_io.h    channel <-> buffer transfer and ordering
//   core/temporal.h      motion-compensated accumulation (pure, unit-tested)
//   core/oidn_device.h   the shared OIDN device and its lock

#include <DDImage/PlanarIop.h>
#include <DDImage/Interest.h>
#include <DDImage/Row.h>
#include <DDImage/Knobs.h>
#include <DDImage/Knob.h>
#include <DDImage/DDMath.h>

#include <vector>

static const char *const HELP = "CG render denoiser based on Intel's Open Image Denoise library";
static const char *const CLASS = "Denoiser";

using namespace DD::Image;

class DenoiserIop : public PlanarIop
{
public:
	DenoiserIop(Node *node);

	// Nuke internal methods
	int minimum_inputs() const { return 1; }
	int maximum_inputs() const { return 3; }

	PackedPreference packedPreference() const { return ePackedPreferenceUnpacked; }

	void knobs(Knob_Callback f);
	int knob_changed(Knob *k);
	void autoDetectAOVs();

	void _validate(bool);

	// Temporal denoising needs frames t-N..t+N of input 0. split_input() turns
	// input 0 into 2N+1 virtual inputs and inputContext() puts each at its own
	// frame — the TemporalMedian.cpp NDK sample pattern.
	int split_input(int n) const;
	const OutputContext *inputUIContext(int n, int offset) const;
	const OutputContext &inputContext(int n, int offset, OutputContext &ctx) const;

	void getRequests(const Box &box, const ChannelSet &channels, int count, RequestOutput &reqData) const;
	virtual void renderStripe(ImagePlane &plane);

	bool useStripes() const { return false; }
	bool renderFullPlanes() const { return true; }

	const char *input_label(int n, char *) const;
	static const Iop::Description d;

	const char *Class() const { return d.name; }
	const char *node_help() const { return HELP; }

private:
	bool m_bHDR;
	bool m_bAffinity;
	bool m_bCleanAux;

	float m_maxMem;

	int m_deviceType;
	int m_numThreads;
	int m_numRuns;
	int m_quality;

	// Temporal
	bool m_bTemporal;
	bool m_bFlipV;
	int m_temporalRadius;
	float m_temporalStrength;
	float m_mvScale;

	ChannelSet m_beautyChannels;
	ChannelSet m_albedoChannels;
	ChannelSet m_normalChannels;
	ChannelSet m_motionChannels;

	// --- temporal state, single owner of these rules ---------------------
	// The clamped radius. Never read m_temporalRadius directly.
	int temporalRadius() const;
	// True when temporal is on AND there is a usable motion layer.
	bool temporalActive() const;
	// The number of split inputs that actually exist. Nuke allocates these when
	// the graph is built, so it can lag behind what the knobs currently ask for.
	int availableSplits() const;
	// Input 0 at frame t+delta, or nullptr when that split does not exist.
	// The ONLY sanctioned way to reach an offset input: Op::input(n, offset) is
	// documented as unchecked, and reading past the array segfaults.
	Op *neighbourInput(int offset) const;
	// True when frame t+delta lies inside input 0's frame range. Outside it, a Read
	// set to "hold" repeats its end frame, which must not be accumulated.
	bool neighbourInSequence(int delta) const;

	// Runs the temporal stage over `colorBuf` in place. No-op when inactive.
	void applyTemporal(float *colorBuf, const Box &format,
					   unsigned int width, unsigned int height);
};
