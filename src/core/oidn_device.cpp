#include "oidn_device.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace denoiser {
namespace device {
namespace {

// The only definitions of this state in the codebase.
CRITICAL_SECTION g_cs;
oidn::DeviceRef g_device;
bool g_ready = false;
int g_type = -1;

// Enum index -> OIDN constant. Index 0 must stay CPU: it is the fallback target.
const OIDNDeviceType kDeviceTypes[] = {
	OIDN_DEVICE_TYPE_CPU,
	OIDN_DEVICE_TYPE_CUDA,
};
constexpr int kNumDeviceTypes = (int)(sizeof(kDeviceTypes) / sizeof(kDeviceTypes[0]));

} // namespace

void initLock() { InitializeCriticalSection(&g_cs); }

void destroyLock()
{
	// Release the device before CRT teardown to avoid a use-after-free on shutdown.
	if (g_ready)
	{
		g_device = nullptr;
		g_ready = false;
	}
	DeleteCriticalSection(&g_cs);
}

Lock::Lock() { EnterCriticalSection(&g_cs); }
Lock::~Lock() { LeaveCriticalSection(&g_cs); }

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

	int tryType = deviceType;
	for (int attempt = 0; attempt < 2; attempt++)
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
				std::cerr << "[Denoiser] GPU device unavailable - fell back to CPU" << std::endl;
			else
				std::cerr << "[Denoiser] OIDN device ready (type=" << tryType << ")" << std::endl;
			return true;
		}
		catch (const std::exception& e)
		{
			g_device = nullptr;
			g_ready = false;
			std::cerr << "[Denoiser] Device init failed (type=" << tryType << "): "
					  << e.what() << std::endl;
			if (tryType != 0)
			{
				tryType = 0; // retry on CPU
				continue;
			}
		}
	}
	g_type = -1;
	return false;
}

} // namespace device
} // namespace denoiser
