import os
import ctypes

# Pre-load OIDN runtime DLLs from the bundled oidn/bin/ folder.
# Using relative paths so the plugin is fully self-contained.
_plugin_dir = os.path.dirname(os.path.abspath(__file__))
_oidn_bin = os.path.join(_plugin_dir, 'oidn', 'bin')
_k32 = ctypes.windll.kernel32
_k32.SetDllDirectoryW(_oidn_bin)
_dlls = ['tbb12.dll', 'OpenImageDenoise_core.dll',
         'OpenImageDenoise_device_cpu.dll', 'OpenImageDenoise.dll']
_cuda_dll = os.path.join(_oidn_bin, 'OpenImageDenoise_device_cuda.dll')
if os.path.exists(_cuda_dll):
    _dlls.append('OpenImageDenoise_device_cuda.dll')
for _dll in _dlls:
    _k32.LoadLibraryW(os.path.join(_oidn_bin, _dll))
_k32.SetDllDirectoryW(None)
del _k32, _dll, _dlls, _cuda_dll, _oidn_bin, _plugin_dir

import nuke
nuke.load('denoiser')
