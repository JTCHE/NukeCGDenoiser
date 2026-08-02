// Single source of truth for the OIDN device.
//
// The device is process-global and shared by every Denoiser node: constructing
// and destroying one per instance races during multi-threaded Nuke renders. All
// of that state lives in oidn_device.cpp and nothing else may declare it.
//
// Every function that touches the device requires the caller to hold a Lock.
#pragma once

#include <OpenImageDenoise/oidn.hpp>

namespace denoiser {
namespace device {

// Created and destroyed from DllMain. A Windows CRITICAL_SECTION rather than a
// std::mutex because std::mutex global constructors do not reliably run when
// Nuke loads the DLL via LoadLibrary — locking an uninitialised one segfaults.
// DllMain is guaranteed to run before any plugin callback.
void initLock();
void destroyLock();

// RAII guard over that lock.
struct Lock
{
	Lock();
	~Lock();
	Lock(const Lock&) = delete;
	Lock& operator=(const Lock&) = delete;
};

// Creates the device if needed, tearing down and recreating it when the
// requested type differs from the live one. Falls back to CPU if a GPU device
// fails to initialise. Returns true when a device is ready to use.
// Takes the lock internally — do not call while holding one.
bool ensure(int deviceType, int numThreads, bool affinity);

// The device type actually in use, which may differ from the one requested if a
// fallback happened. -1 when no device is live.
int activeType();

// Caller must hold a Lock.
oidn::DeviceRef& ref();

// Clears any error left on the device by a previous run and returns it, so a
// cancelled render cannot leak its state into the next evaluation.
// Caller must hold a Lock.
oidn::Error drainError(const char** message);

// Caller must hold a Lock.
void release();

} // namespace device
} // namespace denoiser
