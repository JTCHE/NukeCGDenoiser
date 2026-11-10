#include "oidn_device.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace denoiser {
namespace device {
namespace {

// The only definitions of this state in the codebase.
#ifdef _WIN32
CRITICAL_SECTION g_cs;
#elif defined(__APPLE__)
// Recursive, to match CRITICAL_SECTION. A static initializer is constant
// initialization, so no constructor has to run first.
pthread_mutex_t g_cs = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
#else
pthread_mutex_t g_cs = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
#endif
oidn::DeviceRef g_device;
bool g_ready = false;
int g_type = -1;

// Enum index -> OIDN constant. Index 0 must stay CPU: it is the fallback target.
const OIDNDeviceType kDeviceTypes[] = {
	OIDN_DEVICE_TYPE_CPU,
	OIDN_DEVICE_TYPE_CUDA,
#ifdef __APPLE__
	OIDN_DEVICE_TYPE_METAL,
#endif
};
constexpr int kNumDeviceTypes = (int)(sizeof(kDeviceTypes) / sizeof(kDeviceTypes[0]));

// GPU device indices to try, best first, when the requested one fails. CPU is always last.
const int kGpuFallbackOrder[] = {
#ifdef __APPLE__
	2, // Metal
#endif
	1, // CUDA
};

void releaseDevice()
{
	if (g_ready)
	{
		g_device = nullptr;
		g_ready = false;
	}
}

} // namespace

#ifdef _WIN32
void initLock() { InitializeCriticalSection(&g_cs); }

void destroyLock()
{
	// Release the device before CRT teardown to avoid a use-after-free on shutdown.
	releaseDevice();
	DeleteCriticalSection(&g_cs);
}

Lock::Lock() { EnterCriticalSection(&g_cs); }
Lock::~Lock() { LeaveCriticalSection(&g_cs); }
#else
// The mutex needs no setup. Release the device at library unload, as DLL_PROCESS_DETACH does.
void initLock() {}
void destroyLock() { releaseDevice(); }
__attribute__((destructor)) static void releaseDeviceOnUnload() { releaseDevice(); }

Lock::Lock() { pthread_mutex_lock(&g_cs); }
Lock::~Lock() { pthread_mutex_unlock(&g_cs); }
#endif

const char* name(int deviceType)
{
	static const char* const names[] = {"CPU", "CUDA", "Metal"};
	return (deviceType >= 0 && deviceType < 3) ? names[deviceType] : "none";
}

int count() { return kNumDeviceTypes; }

int activeType() { return g_type; }
oidn::DeviceRef& ref() { return g_device; }

void release()
{
	g_device = nullptr;
	g_ready = false;
	g_type = -1;
}

oidn::Error drainError(const char** message)
{
	const char* scratch = nullptr;
	if (!g_ready)
		return oidn::Error::None;
	return g_device.getError(message ? *message : scratch);
}

bool ensure(int deviceType, int numThreads, bool affinity)
{
	Lock lock;

	deviceType = std::clamp(deviceType, 0, kNumDeviceTypes - 1);

	if (g_ready && g_type != deviceType)
		release();
	if (g_ready)
		return true;

	// Requested device first, then the other GPUs, then CPU as the last resort.
	std::vector<int> order = {deviceType};
	for (int gpu : kGpuFallbackOrder)
		if (gpu != deviceType)
			order.push_back(gpu);
	if (deviceType != 0)
		order.push_back(0);

	for (int tryType : order)
	{
		try
		{
			g_device = oidnNewDevice(kDeviceTypes[tryType]);

			const char* errorMessage;
			if (g_device.getError(errorMessage) != oidn::Error::None)
				throw std::runtime_error(errorMessage);

			// CPU-only settings; OIDN silently ignores these on GPU devices.
			if (tryType == 0)
			{
				g_device.set("numThreads", numThreads);
				g_device.set("setAffinity", affinity);
			}
			g_device.commit();

			if (g_device.getError(errorMessage) != oidn::Error::None)
				throw std::runtime_error(errorMessage);

			g_type = tryType;
			g_ready = true;

			if (tryType != deviceType)
				std::cerr << "[Denoiser] " << name(deviceType) << " unavailable - fell back to "
						  << name(tryType) << std::endl;
			else
				std::cerr << "[Denoiser] OIDN device ready (" << name(tryType) << ")" << std::endl;
			return true;
		}
		catch (const std::exception& e)
		{
			g_device = nullptr;
			g_ready = false;
			std::cerr << "[Denoiser] Device init failed (" << name(tryType) << "): "
					  << e.what() << std::endl;
		}
	}
	g_type = -1;
	return false;
}

} // namespace device
} // namespace denoiser
