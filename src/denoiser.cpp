// Copyright (c) 2021-2026 Mateusz Wojt

#include "denoiser.h"

#include <iostream>
#include <algorithm>

static const char *const _deviceTypeNames[] = {
	"CPU",
	"CUDA",
	0
};

// Maps enum index → OIDN device type constant
static const OIDNDeviceType _deviceTypeValues[] = {
	OIDN_DEVICE_TYPE_CPU,
	OIDN_DEVICE_TYPE_CUDA,
};

static const char *const _qualityNames[] = {
	"Balanced",
	"High",
	0
};

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
static CRITICAL_SECTION  g_cs;
static oidn::DeviceRef   g_device;
static bool              g_deviceReady = false;
static int               g_deviceType  = -1;

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
	m_numRuns = 1;
	m_deviceType = 0; // CPU (index 0 in _deviceTypeNames)
	m_numThreads = 0;
	m_maxMem = 0.f;
	m_quality = 0; // Balanced

	m_defaultChannels = Mask_RGB;
	m_defaultNumberOfChannels = m_defaultChannels.size();
};

// RAII wrapper for CRITICAL_SECTION
struct CsLock {
	CRITICAL_SECTION& cs;
	CsLock(CRITICAL_SECTION& c) : cs(c) { EnterCriticalSection(&cs); }
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

			g_deviceType  = tryType;
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
}

int DenoiserIop::knob_changed(Knob* k)
{
	if (k->is("device"))
	{
		// Force device re-initialization on next render when device type changes
		CsLock lock(g_cs);
		g_device = nullptr;
		g_deviceReady = false;
		g_deviceType  = -1;

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
	switch (n) {
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
	info_.channels(m_defaultChannels);

	m_numRuns = std::clamp(m_numRuns, 1, 32);
	m_quality = std::clamp(m_quality, 0, (int)(sizeof(_qualityValues) / sizeof(_qualityValues[0])) - 1);
	m_deviceType = std::clamp(m_deviceType, 0, (int)(sizeof(_deviceTypeValues) / sizeof(_deviceTypeValues[0])) - 1);
}

void DenoiserIop::getRequests(const Box & box, const ChannelSet & channels, int count, RequestOutput & reqData) const
{
	for (int i = 0; i < node_inputs(); i++) {
		Iop* iop = dynamic_cast<Iop*>(input(i));
		if (iop == nullptr)
			continue;
		iop->request(box, m_defaultChannels, count);
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
	const auto bufferSize = width * height * m_defaultNumberOfChannels * sizeof(float);

	// Create per-render local buffers and filter (each renderStripe call has its own,
	// allowing thread-safe concurrent rendering across multiple Denoiser nodes)
	oidn::BufferRef colorBuffer, albedoBuffer, normalBuffer, outputBuffer;
	oidn::FilterRef filter;
	{
		CsLock lock(g_cs);
		colorBuffer  = g_device.newBuffer(bufferSize);
		albedoBuffer = g_device.newBuffer(bufferSize);
		normalBuffer = g_device.newBuffer(bufferSize);
		outputBuffer = g_device.newBuffer(bufferSize);
		filter = g_device.newFilter("RT");
	}

	// Zero-initialize buffers to avoid garbage data on unconnected inputs
	{
		float* p;
		p = static_cast<float*>(colorBuffer.getData());  if (p) memset(p, 0, bufferSize);
		p = static_cast<float*>(albedoBuffer.getData()); if (p) memset(p, 0, bufferSize);
		p = static_cast<float*>(normalBuffer.getData()); if (p) memset(p, 0, bufferSize);
		p = static_cast<float*>(outputBuffer.getData()); if (p) memset(p, 0, bufferSize);
	}

	// Track which auxiliary inputs are actually connected
	bool hasAlbedo = (node_inputs() > 1 && input(1) != nullptr);
	bool hasNormal = (node_inputs() > 2 && input(2) != nullptr);

	// Setup filter images
	filter.setImage("color",  colorBuffer,  oidn::Format::Float3, width, height);
	filter.setImage("output", outputBuffer, oidn::Format::Float3, width, height);
	if (hasAlbedo) filter.setImage("albedo", albedoBuffer, oidn::Format::Float3, width, height);
	if (hasNormal) filter.setImage("normal", normalBuffer, oidn::Format::Float3, width, height);
	filter.set("hdr", m_bHDR);
	if (m_maxMem > 0.f)
		filter.set("maxMemoryMB", static_cast<int>(m_maxMem));
	filter.set("quality", _qualityValues[m_quality]);
	filter.setProgressMonitorFunction(
		[](void* ctx, double) -> bool {
			return !static_cast<DenoiserIop*>(ctx)->aborted();
		}, this);
	filter.commit();

	{
		CsLock lock(g_cs);
		const char* errMsg;
		if (g_device.getError(errMsg) != oidn::Error::None) {
			error("[OIDN] filter.commit: %s", errMsg);
			return;
		}
	}

	float* colorPtr  = static_cast<float*>(colorBuffer.getData());
	float* albedoPtr = static_cast<float*>(albedoBuffer.getData());
	float* normalPtr = static_cast<float*>(normalBuffer.getData());
	float* outputPtr = static_cast<float*>(outputBuffer.getData());

	if (!colorPtr || !outputPtr) {
		error("[Denoiser] Buffer data is nullptr");
		return;
	}

	// Fetch and copy pixel data from each input
	for (auto i = 0; i < node_inputs(); ++i) {
		if (aborted() || cancelled())
			return;

		Iop* inputIop = dynamic_cast<Iop*>(input(i));
		if (inputIop == nullptr)
			continue;

		if (!inputIop->tryValidate(true))
			continue;

		Box imageBounds = inputIop->info();
		imageBounds.intersect(imageFormat);

		if (imageBounds.w() <= 0 || imageBounds.h() <= 0)
			continue;

		const int fx = imageBounds.x();
		const int fy = imageBounds.y();
		const int fr = imageBounds.r();
		const int ft = imageBounds.t();
		const int imgW = fr - fx;
		const int imgH = ft - fy;

		inputIop->request(fx, fy, fr, ft, m_defaultChannels, 0);

		ImagePlane inputPlane(imageBounds, false, m_defaultChannels, m_defaultNumberOfChannels);
		inputIop->fetchPlane(inputPlane);

		const auto chanStride = inputPlane.chanStride();

		for (auto chanNo = 0; chanNo < m_defaultNumberOfChannels; chanNo++) {
			const float* indata = &inputPlane.readable()[chanStride * chanNo];
			for (auto y = 0; y < imgH; y++) {
				for (auto x = 0; x < imgW; x++) {
					const size_t srcIdx = (size_t)y * imgW + x;
					const size_t dstIdx = ((size_t)(fy + y) * width + (fx + x)) * m_defaultNumberOfChannels + chanNo;

					if (i == 0 && colorPtr)  colorPtr[dstIdx]  = indata[srcIdx];
					if (i == 1 && albedoPtr) albedoPtr[dstIdx] = indata[srcIdx];
					if (i == 2 && normalPtr) normalPtr[dstIdx] = indata[srcIdx];
				}
			}
		}
	}

	if (aborted() || cancelled())
		return;

	// Execute denoise filter
	try
	{
		for (unsigned int run = 0; run < (unsigned int)m_numRuns; run++)
			filter.execute();

		CsLock lock(g_cs);
		const char* errorMessage;
		if (g_device.getError(errorMessage) != oidn::Error::None) {
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

	// Copy denoised output back to the Nuke image plane
	const Box planeBounds = plane.bounds();
	const int px = planeBounds.x();
	const int py = planeBounds.y();
	const int pw = planeBounds.w();
	const int ph = planeBounds.h();

	for (auto chanNo = 0; chanNo < m_defaultNumberOfChannels; chanNo++)
	{
		float* outdata = &plane.writable()[plane.chanStride() * chanNo];

		for (auto j = 0; j < ph; j++) {
			for (auto i = 0; i < pw; i++) {
				const size_t srcIdx = ((size_t)(py + j) * width + (px + i)) * m_defaultNumberOfChannels + chanNo;
				outdata[(size_t)j * pw + i] = outputPtr[srcIdx];
			}
		}
	}
}

static Iop *build(Node *node) { return new DenoiserIop(node); }
const Iop::Description DenoiserIop::d("Denoiser", "Filter/Denoiser", build);
