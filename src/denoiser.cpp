// Copyright (c) 2021-2026 Mateusz Wojt

#include "denoiser.h"

#include "core/aov_patterns.h"
#include "core/channel_io.h"
#include "core/limits.h"
#include "core/oidn_device.h"
#include "core/temporal.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <iostream>
#include <set>
#include <string>
#include <vector>

using namespace denoiser;

static const char *const _deviceTypeNames[] = {
	"CPU",
	"CUDA",
	0};

static const char *const _qualityNames[] = {
	"Balanced",
	"High",
	0};

static const oidn::Quality _qualityValues[] = {
	oidn::Quality::Balanced,
	oidn::Quality::High,
};
static constexpr int kNumQualities = (int)(sizeof(_qualityValues) / sizeof(_qualityValues[0]));
static constexpr int kNumDeviceTypes = 2;

// DllMain: bring the device lock up before any plugin callback can run.
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		device::initLock();
		break;
	case DLL_PROCESS_DETACH:
		device::destroyLock();
		break;
	default:
		break;
	}
	return TRUE;
}

DenoiserIop::DenoiserIop(Node *node) : PlanarIop(node)
{
	m_bHDR = true;
	m_bAffinity = false;
	m_bCleanAux = false;
	m_numRuns = 1;
	m_deviceType = 0; // CPU
	m_numThreads = 0;
	m_maxMem = 0.f;
	m_quality = 0; // Balanced

	m_bTemporal = false;
	m_bFlipV = false;
	m_temporalRadius = kMinTemporalRadius;
	m_temporalStrength = 1.f;
	m_mvScale = 1.f;

	m_beautyChannels = Mask_RGB;
	m_albedoChannels = ChannelSet();
	m_normalChannels = ChannelSet();
	m_motionChannels = ChannelSet();
}

// ---------------------------------------------------------------------------
// Temporal input plumbing
// ---------------------------------------------------------------------------

int DenoiserIop::temporalRadius() const
{
	return std::clamp(m_temporalRadius, kMinTemporalRadius, kMaxTemporalRadius);
}

bool DenoiserIop::temporalActive() const
{
	// A motion layer with fewer than a u and a v has nothing to warp with, so
	// temporal degrades to spatial-only rather than erroring out.
	return m_bTemporal && (int)m_motionChannels.size() >= kMinMotionChannels;
}

namespace {

// Copies one channel out of a format-sized interleaved buffer into a plane channel.
//
// This is the single place allowed to make that transfer, because it is the one
// place that knows the two coordinate spaces disagree: the OIDN buffers are sized
// by info().format(), but plane.bounds() is NOT clamped to the format — Nuke hands
// over a larger plane whenever an upstream node expands the bounding box. Indexing
// the buffer with raw plane coordinates then reads past its end. That is a heap
// out-of-bounds read, and because it corrupts allocator state rather than faulting
// immediately, it surfaces as a segfault somewhere unrelated several frames later.
//
// Anything outside the buffer is written as 0 rather than skipped, so the output is
// deterministic instead of inheriting whatever the plane held.
void writeChannelFromBuffer(DD::Image::ImagePlane &plane, int planeIdx,
							const float *src, unsigned int width, unsigned int height,
							int srcChans, int srcChan)
{
	const DD::Image::Box b = plane.bounds();
	const int px = b.x(), py = b.y(), pw = b.w(), ph = b.h();
	float *out = &plane.writable()[plane.chanStride() * planeIdx];

	for (int j = 0; j < ph; j++)
	{
		const int sy = py + j;
		const bool rowInside = (sy >= 0 && sy < (int)height);
		for (int i = 0; i < pw; i++)
		{
			const int sx = px + i;
			const bool inside = rowInside && sx >= 0 && sx < (int)width;
			out[(size_t)j * pw + i] =
				inside ? src[((size_t)sy * width + sx) * srcChans + srcChan] : 0.f;
		}
	}
}

// Index of `target` within a plane's channel list, or -1.
int planeChannelIndex(const DD::Image::ImagePlane &plane, DD::Image::Channel target)
{
	int idx = 0;
	foreach (z, plane.channels())
	{
		if (z == target)
			return idx;
		idx++;
	}
	return -1;
}

} // namespace

// Split offset 0 is deliberately the current frame, so every plain `input(0)`
// call site keeps reading frame t. Offsets 1..N are +1..+N, N+1..2N are -1..-N.
static int offsetToDelta(int offset, int radius)
{
	if (offset <= 0)
		return 0;
	if (offset <= radius)
		return offset;
	return -(offset - radius);
}

// Keyed on the temporal toggle alone, not temporalActive(): the motion layer knob
// is not EARLY_STORE so split_input() cannot see it. Allocating splits that go
// unused is free; getting the count wrong is not.
int DenoiserIop::split_input(int n) const
{
	if (n == 0 && m_bTemporal)
		return 1 + 2 * temporalRadius();
	return 1;
}

const OutputContext &DenoiserIop::inputContext(int n, int offset, OutputContext &ctx) const
{
	ctx = outputContext();
	if (n == 0 && m_bTemporal)
		ctx.setFrame(ctx.frame() + offsetToDelta(offset, temporalRadius()));
	return ctx;
}

// Keep the upstream node's UI on the current frame rather than an offset one.
const OutputContext *DenoiserIop::inputUIContext(int, int) const
{
	return &outputContext();
}

int DenoiserIop::availableSplits() const
{
	// split_input() is what we *want*; inputs() is what Nuke actually allocated
	// when the graph was last built. Changing the radius knob does not
	// necessarily rebuild the input array, so the two can disagree and only the
	// smaller of them is safe to index. Input 0's splits start at index 0.
	return std::min(split_input(0), inputs());
}

Op *DenoiserIop::neighbourInput(int offset) const
{
	if (offset < 0 || offset >= availableSplits())
		return nullptr;
	return input(0, offset);
}

// ---------------------------------------------------------------------------
// Knobs
// ---------------------------------------------------------------------------

void DenoiserIop::knobs(Knob_Callback f)
{
	Enumeration_knob(f, &m_deviceType, _deviceTypeNames, "device", "Device Type");
	Tooltip(f,
			"CPU: regular CPU denoising backend\n"
			"CUDA: NVIDIA GPU via CUDA (requires CUDA-capable GPU and drivers)");

	Enumeration_knob(f, &m_quality, _qualityNames, "quality", "Quality");
	Tooltip(f,
			"Balanced: balanced quality/performance — good default\n"
			"High: highest quality — use for final renders");

	Bool_knob(f, &m_bHDR, "hdr", "HDR");
	Tooltip(f, "Turn on if input image is high-dynamic range");
	SetFlags(f, Knob::STARTLINE);

	Bool_knob(f, &m_bAffinity, "affinity", "Enable thread affinity");
	Tooltip(f, "Enables thread affinitization (pinning software threads to hardware threads)\n"
			   "if it is necessary for achieving optimal performance");

	Float_knob(f, &m_maxMem, "maxmem", "Memory limit (MB)");
	Tooltip(f, "Limit the memory usage below the specified amount in megabytes.\n"
			   "0 = no memory limit.");

	Int_knob(f, &m_numRuns, "num_runs", "Number of runs");
	Tooltip(f, "Number of times the image will be fed into the denoise filter.");

	Divider(f, "Layers");

	Input_ChannelSet_knob(f, &m_beautyChannels, 0, "beauty_layer", "Beauty layer");
	Tooltip(f, "Which layer from the input to denoise.\n"
			   "Select the RGB channels of your beauty or render pass.");

	Input_ChannelSet_knob(f, &m_albedoChannels, 0, "albedo_layer", "Albedo layer");
	Tooltip(f, "Albedo / diffuse layer for auxiliary-guided denoising.\n"
			   "Select from the beauty input when no albedo input is connected.\n"
			   "Auto-detected from common renderer AOV names (Arnold, Cycles, Karma, "
			   "V-Ray, Redshift, RenderMan, Corona, ...) when this input's layers change.\n"
			   "Set to 'none' to run without albedo guidance.");

	Input_ChannelSet_knob(f, &m_normalChannels, 0, "normal_layer", "Normal layer");
	Tooltip(f, "World-space normal layer for auxiliary-guided denoising.\n"
			   "Select from the beauty input when no normal input is connected.\n"
			   "Auto-detected from common renderer AOV names (Arnold, Cycles, Karma, "
			   "V-Ray, Redshift, RenderMan, Corona, ...) when this input's layers change.\n"
			   "Set to 'none' to run without normal guidance.");

	Bool_knob(f, &m_bCleanAux, "prefilter_aux", "Prefilter Auxiliary Passes");
	Tooltip(f, "When enabled, OIDN will denoise your albedo and normal passes before using "
			   "them to guide beauty denoising. Use this when your auxiliary passes are noisy "
			   "(e.g. from a low sample count render).\n"
			   "Disable if your auxiliary passes are already clean for higher quality output.");
	SetFlags(f, Knob::STARTLINE);

	Divider(f, "Temporal");

	Bool_knob(f, &m_bTemporal, "temporal", "Temporal denoising");
	Tooltip(f, "Reproject neighbouring frames onto the current one using the motion vector "
			   "pass, then denoise the combined result. Removes the frame-to-frame flicker "
			   "that spatial-only denoising leaves behind.\n\n"
			   "Requires a motion vector layer. Costs one extra fetch of the input per frame "
			   "in the window.");
	// EARLY_STORE: split_input() runs before ordinary knob storage, so without this
	// it sees the constructor default and never allocates the extra frame inputs.
	SetFlags(f, Knob::STARTLINE | Knob::EARLY_STORE);

	Input_ChannelSet_knob(f, &m_motionChannels, 0, "motion_layer", "Motion layer");
	Tooltip(f, "Motion vector layer used to warp neighbouring frames.\n\n"
			   "4+ channels are read as forward u/v then backward u/v. 2 or 3 channels are "
			   "read as forward only, and the backward direction is approximated as its "
			   "negation.\n\n"
			   "Set to 'none' to disable temporal denoising.");

	Int_knob(f, &m_temporalRadius, "temporal_radius", "Frames each side");
	SetFlags(f, Knob::EARLY_STORE); // also read by split_input()
	SetRange(f, kMinTemporalRadius, kMaxTemporalRadius);
	Tooltip(f, "How many frames before and after the current one to blend in.\n"
			   "1 uses 3 frames, 2 uses 5, 3 uses 7. Each step costs another full fetch of "
			   "the input, and the motion vectors are extrapolated linearly beyond one "
			   "frame, so higher values ghost more on fast motion.");

	Float_knob(f, &m_temporalStrength, "temporal_strength", "Temporal strength");
	Tooltip(f, "Blend between the original frame (0) and the fully accumulated result (1).\n"
			   "Dial back on shots with fast or unreliable motion.");
	SetRange(f, 0.f, 1.f);

	Float_knob(f, &m_mvScale, "mv_scale", "Vector scale");
	Tooltip(f, "Multiplier applied to the motion vectors before warping.\n"
			   "Renderers disagree about units — some write pixels, some write normalised "
			   "screen space. If the warp over- or under-shoots, correct it here.");
	SetRange(f, 0.f, 2.f);

	Bool_knob(f, &m_bFlipV, "mv_flip_v", "Flip vector V");
	Tooltip(f, "Negate the vertical component of the motion vectors. Needed for renderers "
			   "whose Y axis points the other way.");
	SetFlags(f, Knob::STARTLINE);
}

int DenoiserIop::knob_changed(Knob *k)
{
	if (k->is("device"))
	{
		// Force device re-initialization on next render.
		{
			device::Lock lock;
			device::release();
		}
		// Grey out CPU-only knobs when a GPU device is selected.
		const bool isCPU = (m_deviceType == 0);
		knob("affinity")->enable(isCPU);
		knob("maxmem")->enable(isCPU);
		return 1;
	}
	if (k == &Knob::inputChange || k == &Knob::showPanel)
	{
		autoDetectAOVs();
		return 1;
	}
	return 0;
}

// Auto-fills the layer pickers from common renderer AOV names found on input 0.
// Only fires when the corresponding input isn't directly connected and the picker
// is still at its untouched default, so a deliberate choice is never clobbered —
// though that also means a deliberate "none" is indistinguishable from untouched.
void DenoiserIop::autoDetectAOVs()
{
	if (!input(0))
		return;
	ChannelSet available = input(0)->channels();

	std::set<std::string> exclude;
	foreach (z, m_beautyChannels)
	{
		const char *name = getLayerName(z);
		if (name)
			exclude.insert(name);
	}

	const bool hasAlbedoInput = (node_inputs() > 1 && input(1) != nullptr);
	const bool hasNormalInput = (node_inputs() > 2 && input(2) != nullptr);

	ChannelSet match;
	if (!hasAlbedoInput && m_albedoChannels.size() == 0 &&
		findLayer(available, exclude, AovKind::Albedo, match))
	{
		knob("albedo_layer")->set_text(getLayerName(match.first()));
	}
	if (!hasNormalInput && m_normalChannels.size() == 0 &&
		findLayer(available, exclude, AovKind::Normal, match))
	{
		knob("normal_layer")->set_text(getLayerName(match.first()));
	}
	if (m_motionChannels.size() == 0 &&
		findLayer(available, exclude, AovKind::Motion, match))
	{
		knob("motion_layer")->set_text(getLayerName(match.first()));
	}
}

const char *DenoiserIop::input_label(int n, char *) const
{
	switch (n)
	{
	case 0:
		return "beauty";
	case 1:
		return "albedo";
	case 2:
		return "normal";
	default:
		return 0;
	}
}

void DenoiserIop::_validate(bool for_real)
{
	copy_info();
	// No channel restriction — pass through all input channels. Beauty channels
	// will be overwritten with denoised output.

	const bool hasAlbedoInput = (node_inputs() > 1 && input(1) != nullptr);
	const bool hasNormalInput = (node_inputs() > 2 && input(2) != nullptr);
	knob("albedo_layer")->enable(!hasAlbedoInput);
	knob("normal_layer")->enable(!hasNormalInput);

	m_numRuns = std::clamp(m_numRuns, kMinRuns, kMaxRuns);
	m_temporalRadius = std::clamp(m_temporalRadius, kMinTemporalRadius, kMaxTemporalRadius);
	m_temporalStrength = std::clamp(m_temporalStrength, 0.f, 1.f);
	m_quality = std::clamp(m_quality, 0, kNumQualities - 1);
	m_deviceType = std::clamp(m_deviceType, 0, kNumDeviceTypes - 1);

	// The temporal knobs only mean anything once there is a motion layer to warp with.
	const bool temporalUsable = temporalActive();
	knob("temporal_radius")->enable(temporalUsable);
	knob("temporal_strength")->enable(temporalUsable);
	knob("mv_scale")->enable(temporalUsable);
	knob("mv_flip_v")->enable(temporalUsable);
}

void DenoiserIop::getRequests(const Box &box, const ChannelSet &channels, int count,
							  RequestOutput &reqData) const
{
	const bool hasAlbedoInput = (node_inputs() > 1 && input(1) != nullptr);
	const bool hasNormalInput = (node_inputs() > 2 && input(2) != nullptr);

	ChannelSet req = channels;
	req += m_beautyChannels;
	if (!hasAlbedoInput && m_albedoChannels.size() > 0)
		req += m_albedoChannels;
	if (!hasNormalInput && m_normalChannels.size() > 0)
		req += m_normalChannels;
	if (temporalActive())
		req += m_motionChannels;

	Iop *iop0 = dynamic_cast<Iop *>(input(0));
	if (iop0)
		iop0->request(box, req, count);

	// Each temporal split is the same upstream node at a different frame and needs
	// its own request, otherwise the offset frames fetch nothing.
	for (int offset = 1; offset < availableSplits(); offset++)
	{
		Iop *split = dynamic_cast<Iop *>(neighbourInput(offset));
		if (split && split != iop0)
			split->request(box, m_beautyChannels, count);
	}

	if (hasAlbedoInput)
	{
		Iop *iop1 = dynamic_cast<Iop *>(input(1));
		if (iop1)
			iop1->request(box, Mask_RGB, count);
	}
	if (hasNormalInput)
	{
		Iop *iop2 = dynamic_cast<Iop *>(input(2));
		if (iop2)
			iop2->request(box, Mask_RGB, count);
	}
}

// ---------------------------------------------------------------------------
// Temporal stage
// ---------------------------------------------------------------------------

void DenoiserIop::applyTemporal(float *colorBuf, const Box &format,
								unsigned int width, unsigned int height)
{
	if (!temporalActive())
		return;

	const int radius = temporalRadius();
	const int mvChans = std::min((int)m_motionChannels.size(), kMaxMotionChannels);
	const size_t numPixels = (size_t)width * height;


	std::vector<float> mv((size_t)numPixels * mvChans, 0.f);
	if (!fetchInterleaved(input(0), m_motionChannels, format, mv.data(), mvChans, width, height))
		return;

	TemporalParams params;
	params.strength = m_temporalStrength;
	params.mvScale = m_mvScale;
	params.flipV = m_bFlipV;

	TemporalAccumulator acc(colorBuf, width, height, mv.data(), mvChans, params);
	if (!acc.usable())
		return;

	// One neighbour buffer at a time keeps peak memory at 2 frames, not 2N+1.
	std::vector<float> neighbour(numPixels * 3, 0.f);
	for (int offset = 1; offset < availableSplits(); offset++)
	{
		if (aborted() || cancelled())
			return;

		const int delta = offsetToDelta(offset, radius);
		if (delta == 0)
			continue;

		// A frame outside the sequence contributes nothing: the per-pixel weight
		// stays lower there and the result degrades smoothly to spatial-only, so
		// the head and tail of a shot do not go black.
		if (!fetchInterleaved(neighbourInput(offset), m_beautyChannels, format,
							  neighbour.data(), 3, width, height))
			continue;

		acc.addFrame(delta, neighbour.data());
	}

	acc.finish(colorBuf);
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void DenoiserIop::renderStripe(ImagePlane &plane)
{
	if (aborted() || cancelled())
		return;

	if (!device::ensure(m_deviceType, m_numThreads, m_bAffinity))
	{
		error("[OIDN]: Device initialization failed");
		return;
	}

	{
		device::Lock lock;
		// Drain any stale error left on the shared device by a previously
		// cancelled run, otherwise Cancelled leaks into this fresh evaluation.
		device::drainError(nullptr);

		// If the device fell back, update the knob so the UI reflects reality.
		if (device::activeType() != m_deviceType)
		{
			knob("device")->set_value(device::activeType());
			knob("affinity")->enable(true);
			knob("maxmem")->enable(true);
			warning("[Denoiser] Requested device unavailable — fell back to CPU");
		}
	}

	const Box imageFormat = info().format();
	const unsigned int width = imageFormat.w();
	const unsigned int height = imageFormat.h();
	const auto bufferSize = (size_t)width * height * 3 * sizeof(float);

	const bool hasAlbedoInput = (node_inputs() > 1 && input(1) != nullptr);
	const bool hasNormalInput = (node_inputs() > 2 && input(2) != nullptr);
	const bool useAlbedo = hasAlbedoInput || (m_albedoChannels.size() > 0);
	const bool useNormal = hasNormalInput || (m_normalChannels.size() > 0);

	// Per-render local buffers and filter, so concurrent Denoiser nodes do not
	// share anything but the device itself.
	//
	// The OIDN buffers are device-only and the host never touches them: pixels are
	// staged in host vectors and moved with write()/read(). Default newBuffer()
	// storage on CUDA is managed memory, and on Windows the CPU may not touch managed
	// memory while any kernel runs on the GPU — another Denoiser's execute() is
	// enough — so getData() pointers crashed with 0xC0000006 under concurrent
	// renders. Device storage is also about 6x faster than managed or pinned host.
	const size_t numFloats = (size_t)width * height * 3;
	std::vector<float> colorHost(numFloats, 0.f), outputHost(numFloats, 0.f);
	std::vector<float> albedoHost(useAlbedo ? numFloats : 0, 0.f);
	std::vector<float> normalHost(useNormal ? numFloats : 0, 0.f);
	oidn::BufferRef colorBuffer, albedoBuffer, normalBuffer, outputBuffer;
	oidn::FilterRef filter;
	{
		device::Lock lock;
		colorBuffer = device::ref().newBuffer(bufferSize, oidn::Storage::Device);
		outputBuffer = device::ref().newBuffer(bufferSize, oidn::Storage::Device);
		if (useAlbedo)
			albedoBuffer = device::ref().newBuffer(bufferSize, oidn::Storage::Device);
		if (useNormal)
			normalBuffer = device::ref().newBuffer(bufferSize, oidn::Storage::Device);
		filter = device::ref().newFilter("RT");
	}

	float *colorPtr = colorHost.data();
	float *outputPtr = outputHost.data();

	filter.setImage("color", colorBuffer, oidn::Format::Float3, width, height);
	filter.setImage("output", outputBuffer, oidn::Format::Float3, width, height);
	if (useAlbedo)
		filter.setImage("albedo", albedoBuffer, oidn::Format::Float3, width, height);
	if (useNormal)
		filter.setImage("normal", normalBuffer, oidn::Format::Float3, width, height);
	filter.set("hdr", m_bHDR);
	filter.set("cleanAux", !m_bCleanAux);
	if (m_maxMem > 0.f)
		filter.set("maxMemoryMB", static_cast<int>(m_maxMem));
	filter.set("quality", _qualityValues[m_quality]);
	filter.setProgressMonitorFunction(
		[](void *ctx, double) -> bool
		{
			return !static_cast<DenoiserIop *>(ctx)->aborted();
		},
		this);
	filter.commit();

	{
		device::Lock lock;
		const char *errMsg = nullptr;
		auto err = device::drainError(&errMsg);
		if (err == oidn::Error::Cancelled)
			return; // user-cancelled — bail silently, not a node error
		if (err != oidn::Error::None)
		{
			error("[OIDN] filter.commit: %s", errMsg);
			return;
		}
	}

	fetchInterleaved(input(0), m_beautyChannels, imageFormat, colorPtr, 3, width, height);

	if (aborted() || cancelled())
		return;

	// Temporal runs before the spatial denoiser: lower input variance means the
	// denoiser blurs less, which is where the detail retention comes from.
	applyTemporal(colorPtr, imageFormat, width, height);

	if (aborted() || cancelled())
		return;

	if (useAlbedo)
	{
		float *albedoPtr = albedoHost.data();
		if (hasAlbedoInput)
			fetchInterleaved(input(1), Mask_RGB, imageFormat, albedoPtr, 3, width, height);
		else
			fetchInterleaved(input(0), m_albedoChannels, imageFormat, albedoPtr, 3, width, height);
	}

	if (aborted() || cancelled())
		return;

	if (useNormal)
	{
		float *normalPtr = normalHost.data();
		if (hasNormalInput)
			fetchInterleaved(input(2), Mask_RGB, imageFormat, normalPtr, 3, width, height);
		else
			fetchInterleaved(input(0), m_normalChannels, imageFormat, normalPtr, 3, width, height);
	}

	if (aborted() || cancelled())
		return;

	try
	{
		colorBuffer.write(0, bufferSize, colorPtr);
		if (useAlbedo)
			albedoBuffer.write(0, bufferSize, albedoHost.data());
		if (useNormal)
			normalBuffer.write(0, bufferSize, normalHost.data());

		for (int run = 0; run < m_numRuns; run++)
		{
			if (aborted() || cancelled())
				return;
			filter.execute();
		}
		outputBuffer.read(0, bufferSize, outputPtr);

		device::Lock lock;
		const char *errorMessage = nullptr;
		auto err = device::drainError(&errorMessage);
		if (err == oidn::Error::Cancelled)
			return;
		if (err != oidn::Error::None)
		{
			error("[OIDN]: %s", errorMessage);
			return;
		}
	}
	catch (const std::exception &e)
	{
		{
			device::Lock lock;
			device::drainError(nullptr);
		}
		// Cooperative cancellation (knob change, viewer re-evaluation, Esc) is not
		// a real error — bail silently and let Nuke schedule the next evaluation.
		if (aborted() || cancelled())
			return;
		std::string message = e.what();
		std::cerr << "[Denoiser] Exception in filter.execute: " << message << std::endl;
		error("[OIDN]: %s", message.c_str());
		return;
	}

	// --- write out -------------------------------------------------------
	const Box planeBounds = plane.bounds();
	const int pw = planeBounds.w();
	const int ph = planeBounds.h();

	// Step 1: passthrough — copy all channels from input 0.
	{
		Iop *inp0 = dynamic_cast<Iop *>(input(0));
		if (inp0)
		{
			ImagePlane passPlane(planeBounds, false, plane.channels(), plane.channels().size());
			inp0->fetchPlane(passPlane);
			const int numChans = (int)plane.channels().size();
			// Never copy more than either side actually owns. chanStride() is the
			// authority on how much space a channel has; pw*ph is only equal to it
			// when there is no row padding.
			const size_t copyFloats = std::min((size_t)pw * ph,
											   std::min((size_t)plane.chanStride(),
														(size_t)passPlane.chanStride()));
			for (int c = 0; c < numChans; c++)
			{
				memcpy(&plane.writable()[plane.chanStride() * c],
					   &passPlane.readable()[passPlane.chanStride() * c],
					   copyFloats * sizeof(float));
			}
		}
	}

	// Step 2: write denoised output to RGB. Acts like shuffle + denoise — the
	// selected beauty layer is denoised into RGB, leaving the original untouched.
	{
		const Channel rgbChans[3] = {Chan_Red, Chan_Green, Chan_Blue};
		for (int bi = 0; bi < 3; bi++)
		{
			const int planeIdx = planeChannelIndex(plane, rgbChans[bi]);
			if (planeIdx < 0)
				continue;
			writeChannelFromBuffer(plane, planeIdx, outputPtr, width, height, 3, bi);
		}
	}

	// Step 3: alpha, denoised in its own pass because OIDN works on 3 channels.
	{
		Channel alphaChannel = Chan_Black;
		int chCount = 0;
		foreach (z, m_beautyChannels)
		{
			if (chCount == 3)
			{
				alphaChannel = z;
				break;
			}
			chCount++;
		}

		if (alphaChannel == Chan_Black || aborted() || cancelled())
			return;

		// Reuse the colour and output buffers; the main denoise is finished.
		float *alphaInPtr = colorPtr;
		float *alphaOutPtr = outputPtr;
		memset(alphaInPtr, 0, bufferSize);
		memset(alphaOutPtr, 0, bufferSize);

		// Replicate alpha across all three channels of the Float3 buffer.
		ChannelSet alphaChanSet;
		alphaChanSet += alphaChannel;
		std::vector<float> alphaScratch((size_t)width * height, 0.f);
		// Sequential: alpha's colour index is 3, but here it is the only channel
		// and belongs at slot 0.
		if (fetchInterleaved(input(0), alphaChanSet, imageFormat, alphaScratch.data(), 1,
							 width, height, Packing::Sequential))
		{
			for (size_t p = 0; p < (size_t)width * height; p++)
			{
				const float val = alphaScratch[p];
				alphaInPtr[p * 3 + 0] = val;
				alphaInPtr[p * 3 + 1] = val;
				alphaInPtr[p * 3 + 2] = val;
			}
		}

		oidn::FilterRef alphaFilter;
		{
			device::Lock lock;
			alphaFilter = device::ref().newFilter("RT");
		}
		alphaFilter.setImage("color", colorBuffer, oidn::Format::Float3, width, height);
		alphaFilter.setImage("output", outputBuffer, oidn::Format::Float3, width, height);
		alphaFilter.set("hdr", m_bHDR);
		alphaFilter.set("quality", _qualityValues[m_quality]);
		alphaFilter.setProgressMonitorFunction(
			[](void *ctx, double) -> bool
			{
				return !static_cast<DenoiserIop *>(ctx)->aborted();
			},
			this);
		alphaFilter.commit();

		{
			device::Lock lock;
			const char *errMsg = nullptr;
			auto err = device::drainError(&errMsg);
			if (err == oidn::Error::Cancelled)
				return;
			if (err != oidn::Error::None)
			{
				error("[OIDN] alpha filter.commit: %s", errMsg);
				return;
			}
		}

		try
		{
			colorBuffer.write(0, bufferSize, alphaInPtr);
			for (int run = 0; run < m_numRuns; run++)
			{
				if (aborted() || cancelled())
					return;
				alphaFilter.execute();
			}
			outputBuffer.read(0, bufferSize, alphaOutPtr);
		}
		catch (const std::exception &e)
		{
			device::Lock lock;
			device::drainError(nullptr);
			if (aborted() || cancelled())
				return;
			std::cerr << "[Denoiser] Alpha denoise failed: " << e.what() << std::endl;
		}

		const int alphaPlaneIdx = planeChannelIndex(plane, Chan_Alpha);
		if (alphaPlaneIdx >= 0)
			writeChannelFromBuffer(plane, alphaPlaneIdx, alphaOutPtr, width, height, 3, 0);
	}
}

static Iop *build(Node *node) { return new DenoiserIop(node); }
const Iop::Description DenoiserIop::d("Denoiser", "Filter/Denoiser", build);
