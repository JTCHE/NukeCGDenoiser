import os
import ctypes

# Pre-load OIDN runtime DLLs from the bundled oidn-cpu-only/bin/ folder.
# Using relative paths so the plugin is fully self-contained.
_plugin_dir = os.path.dirname(os.path.abspath(__file__))
_oidn_bin = os.path.join(_plugin_dir, 'oidn-cpu-only', 'bin')
_k32 = ctypes.windll.kernel32
_k32.SetDllDirectoryW(_oidn_bin)
for _dll in ['tbb12.dll', 'OpenImageDenoise_core.dll',
             'OpenImageDenoise_device_cpu.dll', 'OpenImageDenoise.dll']:
    _k32.LoadLibraryW(os.path.join(_oidn_bin, _dll))
_k32.SetDllDirectoryW(None)
del _k32, _dll, _oidn_bin, _plugin_dir
