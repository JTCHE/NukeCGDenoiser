// Copyright (c) 2021-2026 Mateusz Wojt

#include "denoiser.h"

#include <iostream>
#include <algorithm>

static const char *const _deviceTypeNames[] = {
	"CPU",
	"CUDA",
	0};

// Maps enum index → OIDN device type constant
static const OIDNDeviceType _deviceTypeValues[] = {
	OIDN_DEVICE_TYPE_CPU,
	OIDN_DEVICE_TYPE_CUDA,
};

static const char *const _qualityNames[] = {
	"Balanced",
	"High",
	0};

static const oidn::Quality _qualityValues[] = {
	oidn::Quality::Balanced,
	oidn::Quality::High,
};

// Module-level OIDN device state, protected by a Windows CRITICAL_SECTION.
//
// We use CRITICAL_SECTION (initialized in DllMain) instead of std::mutex because
// std::mutex global constructors do not reliably run when a DLL is loaded via
// LoadLibrary inside Nuke's process — locking an uninitialized std::mutex segfaults.
// DllMain is guaranteed to run on DLL_PROCESS_ATTACH before any plugin callbacks.
//
// The device itself is also global (shared across all DenoiserIop instances) to avoid
// per-instance construction/destruction races in a multi-threaded Nuke render context.
static CRITICAL_SECTION g_cs;
static oidn::DeviceRef g_device;
static bool g_deviceReady = false;
static int g_deviceType = -1;

// DllMain: initialize and destroy the CRITICAL_SECTION at DLL attach/detach.
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		InitializeCriticalSection(&g_cs);
		break;
	case DLL_PROCESS_DETACH:
		// Release device before CRT teardown to avoid use-after-free during shutdown
		if (g_deviceReady)
		{
			g_device = nullptr;
			g_deviceReady = false;
		}
		DeleteCriticalSection(&g_cs);
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
	m_deviceType = 0; // CPU (index 0 in _deviceTypeNames)
	m_numThreads = 0;
	m_maxMem = 0.f;
	m_quality = 0; // Balanced

	m_beautyChannels = Mask_RGB;
	m_albedoChannels = ChannelSet();
	m_normalChannels = ChannelSet();
};

// RAII wrapper for CRITICAL_SECTION
struct CsLock
{
	CRITICAL_SECTION &cs;
	CsLock(CRITICAL_SECTION &c) : cs(c) { EnterCriticalSection(&cs); }
	~CsLock() { LeaveCriticalSection(&cs); }
};

// static
void DenoiserIop::setupDevice(int deviceType, int numThreads, bool affinity)
{
	CsLock lock(g_cs);

	deviceType = std::clamp(deviceType, 0, (int)(sizeof(_deviceTypeValues) / sizeof(_deviceTypeValues[0])) - 1);

	// If device type changed, tear down existing device
	if (g_deviceReady && g_deviceType != deviceType)
	{
		g_device = nullptr;
		g_deviceReady = false;
	}

	if (g_deviceReady)
		return;

	int tryType = deviceType;
	for (int attempt = 0; attempt < 2; attempt++)
	{
		try
		{
			OIDNDeviceType devType = _deviceTypeValues[tryType];
			g_device = oidnNewDevice(devType);

			const char *errorMessage;
			if (g_device.getError(errorMessage) != oidn::Error::None)
				throw std::runtime_error(errorMessage);

			// CPU-only settings — OIDN silently ignores these on GPU devices
			if (tryType == 0)
			{
				g_device.set("numThreads", numThreads);
				g_device.set("setAffinity", affinity);
			}
			g_device.commit();

			if (g_device.getError(errorMessage) != oidn::Error::None)
				throw std::runtime_error(errorMessage);

			g_deviceType = tryType;
			g_deviceReady = true;

			if (tryType != deviceType)
				std::cerr << "[Denoiser] CUDA unavailable — fell back to CPU" << std::endl;
			else
				std::cerr << "[Denoiser] OIDN device ready (type=" << tryType << ")" << std::endl;
			return;
		}
		catch (const std::exception &e)
		{
			g_device = nullptr;
			g_deviceReady = false;
			std::cerr << "[Denoiser] Device init failed (type=" << tryType << "): " << e.what() << std::endl;

			// If a non-CPU device failed, retry with CPU
			if (tryType != 0)
			{
				tryType = 0;
				continue;
			}
		}
	}
}

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
			   "Set to 'none' to run without albedo guidance.");

	Input_ChannelSet_knob(f, &m_normalChannels, 0, "normal_layer", "Normal layer");
	Tooltip(f, "World-space normal layer for auxiliary-guided denoising.\n"
			   "Select from the beauty input when no normal input is connected.\n"
			   "Set to 'none' to run without normal guidance.");

	Bool_knob(f, &m_bCleanAux, "prefilter_aux", "Prefilter Auxiliary Passes");
	Tooltip(f, "When enabled, OIDN will denoise your albedo and normal passes before using them to guide beauty denoising. Use this when your auxiliary passes are noisy (e.g. from a low sample count render).\n"
			   "Disable if your auxiliary passes are already clean for higher quality output.");
	SetFlags(f, Knob::STARTLINE);
}

int DenoiserIop::knob_changed(Knob *k)
{
	if (k->is("device"))
	{
		// Force device re-initialization on next render when device type changes
		CsLock lock(g_cs);
		g_device = nullptr;
		g_deviceReady = false;
		g_deviceType = -1;

		// Grey out CPU-only knobs when a GPU device is selected
		bool isCPU = (m_deviceType == 0);
		knob("affinity")->enable(isCPU);
		knob("maxmem")->enable(isCPU);
		return 1;
	}
	return 0;
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
	// No channel restriction — pass through all input channels.
	// Beauty channels will be overwritten with denoised output.

	// Disable layer knobs when their corresponding inputs are connected
	bool hasAlbedoInput = (node_inputs() > 1 && input(1) != nullptr);
	bool hasNormalInput = (node_inputs() > 2 && input(2) != nullptr);
	knob("albedo_layer")->enable(!hasAlbedoInput);
	knob("normal_layer")->enable(!hasNormalInput);

	m_numRuns = std::clamp(m_numRuns, 1, 32);
	m_quality = std::clamp(m_quality, 0, (int)(sizeof(_qualityValues) / sizeof(_qualityValues[0])) - 1);
	m_deviceType = std::clamp(m_deviceType, 0, (int)(sizeof(_deviceTypeValues) / sizeof(_deviceTypeValues[0])) - 1);
}

void DenoiserIop::getRequests(const Box &box, const ChannelSet &channels, int count, RequestOutput &reqData) const
{
	bool hasAlbedoInput = (node_inputs() > 1 && input(1) != nullptr);
	bool hasNormalInput = (node_inputs() > 2 && input(2) != nullptr);

	// Input 0: request passthrough channels + beauty + aux layers sourced from input 0
	Iop *iop0 = dynamic_cast<Iop *>(input(0));
	if (iop0)
	{
		ChannelSet req = channels;
		req += m_beautyChannels;
		if (!hasAlbedoInput && m_albedoChannels.size() > 0)
			req += m_albedoChannels;
		if (!hasNormalInput && m_normalChannels.size() > 0)
			req += m_normalChannels;
		iop0->request(box, req, count);
	}

	// Request from connected auxiliary inputs
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

void DenoiserIop::fetchToBuffer(int inputIdx, const ChannelSet &channels, const Box &format,
								float *buffer, unsigned int width, unsigned int height)
{
	Iop *inputIop = dynamic_cast<Iop *>(input(inputIdx));
	if (!inputIop || !inputIop->tryValidate(true))
		return;

	Box imageBounds = inputIop->info();
	imageBounds.intersect(format);
	if (imageBounds.w() <= 0 || imageBounds.h() <= 0)
		return;

	const int fx = imageBounds.x();
	const int fy = imageBounds.y();
	const int fr = imageBounds.r();
	const int ft = imageBounds.t();
	const int imgW = fr - fx;
	const int imgH = ft - fy;
	const int numChans = std::min((int)channels.size(), 3);

	inputIop->request(fx, fy, fr, ft, channels, 0);

	ImagePlane inputPlane(imageBounds, false, channels, channels.size());
	inputIop->fetchPlane(inputPlane);

	const auto chanStride = inputPlane.chanStride();
	for (int chanNo = 0; chanNo < numChans; chanNo++)
	{
		const float *indata = &inputPlane.readable()[chanStride * chanNo];
		for (int y = 0; y < imgH; y++)
		{
			for (int x = 0; x < imgW; x++)
			{
				const size_t srcIdx = (size_t)y * imgW + x;
				const size_t dstIdx = ((size_t)(fy + y) * width + (fx + x)) * 3 + chanNo;
				buffer[dstIdx] = indata[srcIdx];
			}
		}
	}
}

void DenoiserIop::renderStripe(ImagePlane &plane)
{
	if (aborted() || cancelled())
		return;

	// Lazy device initialization — only on first render, or after a device type change
	{
		bool ready;
		{
			CsLock lock(g_cs);
			ready = g_deviceReady;
		}
		if (!ready)
			setupDevice(m_deviceType, m_numThreads, m_bAffinity);
	}

	{
		CsLock lock(g_cs);
		if (!g_deviceReady)
		{
			error("[OIDN]: Device initialization failed");
			return;
		}
		// If device fell back to CPU, update the knob so the UI reflects reality
		if (g_deviceType != m_deviceType)
		{
			knob("device")->set_value(g_deviceType);
			knob("affinity")->enable(true);
			knob("maxmem")->enable(true);
			warning("[Denoiser] CUDA unavailable — fell back to CPU");
		}
	}

	const Box imageFormat = info().format();
	const unsigned int width = imageFormat.w();
	const unsigned int height = imageFormat.h();
	const auto bufferSize = width * height * 3 * sizeof(float);

	// Determine auxiliary sources
	bool hasAlbedoInput = (node_inputs() > 1 && input(1) != nullptr);
	bool hasNormalInput = (node_inputs() > 2 && input(2) != nullptr);
	bool useAlbedo = hasAlbedoInput || (m_albedoChannels.size() > 0);
	bool useNormal = hasNormalInput || (m_normalChannels.size() > 0);

	// Create per-render local buffers and filter (each renderStripe call has its own,
	// allowing thread-safe concurrent rendering across multiple Denoiser nodes)
	oidn::BufferRef colorBuffer, albedoBuffer, normalBuffer, outputBuffer;
	oidn::FilterRef filter;
	{
		CsLock lock(g_cs);
		colorBuffer = g_device.newBuffer(bufferSize);
		outputBuffer = g_device.newBuffer(bufferSize);
		if (useAlbedo)
			albedoBuffer = g_device.newBuffer(bufferSize);
		if (useNormal)
			normalBuffer = g_device.newBuffer(bufferSize);
		filter = g_device.newFilter("RT");
	}

	// Zero-initialize buffers
	{
		float *p;
		p = static_cast<float *>(colorBuffer.getData());
		if (p)
			memset(p, 0, bufferSize);
		p = static_cast<float *>(outputBuffer.getData());
		if (p)
			memset(p, 0, bufferSize);
		if (useAlbedo)
		{
			p = static_cast<float *>(albedoBuffer.getData());
			if (p)
				memset(p, 0, bufferSize);
		}
		if (useNormal)
		{
			p = static_cast<float *>(normalBuffer.getData());
			if (p)
				memset(p, 0, bufferSize);
		}
	}

	float *colorPtr = static_cast<float *>(colorBuffer.getData());
	float *outputPtr = static_cast<float *>(outputBuffer.getData());

	// Setup filter images
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
		CsLock lock(g_cs);
		const char *errMsg;
		if (g_device.getError(errMsg) != oidn::Error::None)
		{
			error("[OIDN] filter.commit: %s", errMsg);
			return;
		}
	}

	if (!colorPtr || !outputPtr)
	{
		error("[Denoiser] Buffer data is nullptr");
		return;
	}

	// Fetch beauty from input 0
	fetchToBuffer(0, m_beautyChannels, imageFormat, colorPtr, width, height);

	if (aborted() || cancelled())
		return;

	// Fetch albedo
	if (useAlbedo)
	{
		float *albedoPtr = static_cast<float *>(albedoBuffer.getData());
		if (hasAlbedoInput)
			fetchToBuffer(1, Mask_RGB, imageFormat, albedoPtr, width, height);
		else
			fetchToBuffer(0, m_albedoChannels, imageFormat, albedoPtr, width, height);
	}

	if (aborted() || cancelled())
		return;

	// Fetch normal
	if (useNormal)
	{
		float *normalPtr = static_cast<float *>(normalBuffer.getData());
		if (hasNormalInput)
			fetchToBuffer(2, Mask_RGB, imageFormat, normalPtr, width, height);
		else
			fetchToBuffer(0, m_normalChannels, imageFormat, normalPtr, width, height);
	}

	if (aborted() || cancelled())
		return;

	// Execute denoise filter
	try
	{
		for (unsigned int run = 0; run < (unsigned int)m_numRuns; run++)
			filter.execute();

		CsLock lock(g_cs);
		const char *errorMessage;
		if (g_device.getError(errorMessage) != oidn::Error::None)
		{
			error("[OIDN]: %s", errorMessage);
			return;
		}
	}
	catch (const std::exception &e)
	{
		std::string message = e.what();
		std::cerr << "[Denoiser] Exception in filter.execute: " << message << std::endl;
		error("[OIDN]: %s", message.c_str());
		return;
	}

	// Copy output to Nuke image plane
	const Box planeBounds = plane.bounds();
	const int px = planeBounds.x();
	const int py = planeBounds.y();
	const int pw = planeBounds.w();
	const int ph = planeBounds.h();

	// Step 1: Passthrough — copy all channels from input 0
	{
		Iop *inp0 = dynamic_cast<Iop *>(input(0));
		if (inp0)
		{
			ImagePlane passPlane(planeBounds, false, plane.channels(), plane.channels().size());
			inp0->fetchPlane(passPlane);
			const int numChans = (int)plane.channels().size();
			for (int c = 0; c < numChans; c++)
			{
				memcpy(&plane.writable()[plane.chanStride() * c],
					   &passPlane.readable()[passPlane.chanStride() * c],
					   (size_t)pw * ph * sizeof(float));
			}
		}
	}

	// Step 2: Write denoised output to RGB channels.
	// Acts like shuffle + denoise: the selected beauty layer gets denoised
	// into RGB, leaving the original layer untouched.
	{
		const Channel rgbChans[3] = {Chan_Red, Chan_Green, Chan_Blue};
		for (int bi = 0; bi < 3; bi++)
		{
			int planeIdx = 0;
			bool found = false;
			foreach (z, plane.channels())
			{
				if (z == rgbChans[bi])
				{
					found = true;
					break;
				}
				planeIdx++;
			}
			if (!found)
				continue;

			float *outdata = &plane.writable()[plane.chanStride() * planeIdx];
			for (int j = 0; j < ph; j++)
			{
				for (int i = 0; i < pw; i++)
				{
					outdata[(size_t)j * pw + i] = outputPtr[((size_t)(py + j) * width + (px + i)) * 3 + bi];
				}
			}
		}
	}

	// Step 3: Alpha denoising — if beauty layer includes a 4th channel (alpha),
	// denoise it separately (OIDN only supports Float3) and write to Chan_Alpha.
	{
		// Find the 4th channel in the beauty set (alpha)
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

		if (alphaChannel != Chan_Black && !aborted() && !cancelled())
		{
			// Reuse color and output buffers (main denoise is finished)
			float *alphaInPtr = static_cast<float *>(colorBuffer.getData());
			float *alphaOutPtr = static_cast<float *>(outputBuffer.getData());
			memset(alphaInPtr, 0, bufferSize);
			memset(alphaOutPtr, 0, bufferSize);

			// Fetch alpha channel from input 0 and replicate to all 3 Float3 channels
			Iop *inp0 = dynamic_cast<Iop *>(input(0));
			if (inp0 && inp0->tryValidate(true))
			{
				Box imageBounds = inp0->info();
				imageBounds.intersect(imageFormat);
				if (imageBounds.w() > 0 && imageBounds.h() > 0)
				{
					const int fx = imageBounds.x(), fy = imageBounds.y();
					const int fr = imageBounds.r(), ft = imageBounds.t();
					const int imgW = fr - fx, imgH = ft - fy;

					ChannelSet alphaChanSet;
					alphaChanSet += alphaChannel;
					inp0->request(fx, fy, fr, ft, alphaChanSet, 0);

					ImagePlane alphaPlane(imageBounds, false, alphaChanSet, 1);
					inp0->fetchPlane(alphaPlane);

					const float *indata = alphaPlane.readable();
					for (int y = 0; y < imgH; y++)
					{
						for (int x = 0; x < imgW; x++)
						{
							const float val = indata[(size_t)y * imgW + x];
							const size_t dst = ((size_t)(fy + y) * width + (fx + x)) * 3;
							alphaInPtr[dst + 0] = val;
							alphaInPtr[dst + 1] = val;
							alphaInPtr[dst + 2] = val;
						}
					}
				}
			}

			// Run a second denoise filter for alpha (no auxiliary guidance)
			oidn::FilterRef alphaFilter;
			{
				CsLock lock(g_cs);
				alphaFilter = g_device.newFilter("RT");
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
				CsLock lock(g_cs);
				const char *errMsg;
				if (g_device.getError(errMsg) != oidn::Error::None)
				{
					error("[OIDN] alpha filter.commit: %s", errMsg);
					return;
				}
			}

			try
			{
				for (unsigned int run = 0; run < (unsigned int)m_numRuns; run++)
					alphaFilter.execute();
			}
			catch (const std::exception &e)
			{
				std::cerr << "[Denoiser] Alpha denoise failed: " << e.what() << std::endl;
			}

			// Write denoised alpha (extract channel 0) to Chan_Alpha in plane
			int alphaPlaneIdx = 0;
			bool alphaFound = false;
			foreach (z, plane.channels())
			{
				if (z == Chan_Alpha)
				{
					alphaFound = true;
					break;
				}
				alphaPlaneIdx++;
			}

			if (alphaFound)
			{
				float *outdata = &plane.writable()[plane.chanStride() * alphaPlaneIdx];
				for (int j = 0; j < ph; j++)
				{
					for (int i = 0; i < pw; i++)
					{
						outdata[(size_t)j * pw + i] = alphaOutPtr[((size_t)(py + j) * width + (px + i)) * 3];
					}
				}
			}
		}
	}
}

static Iop *build(Node *node) { return new DenoiserIop(node); }
const Iop::Description DenoiserIop::d("Denoiser", "Filter/Denoiser", build);
