"""
Nuke Denoiser — Installer
Copies the plugin into ~/.nuke/nuke-denoiser/ and patches ~/.nuke/init.py.
Idempotent: safe to run multiple times.
"""
import os
import sys
import shutil

PLUGIN_FOLDER = 'nuke-denoiser'
BLOCK_START   = '# --- nuke-denoiser START ---'
BLOCK_END     = '# --- nuke-denoiser END ---'
BLOCK_CONTENT = (
    BLOCK_START + '\n'
    "nuke.pluginAddPath('./nuke-denoiser')\n"
    + BLOCK_END + '\n'
)

# Directories to exclude when copying the plugin folder
EXCLUDE_DIRS  = {'source', '.claude', '.git', 'build'}
EXCLUDE_FILES = {'install.py', 'install.bat', 'uninstall.py'}


def find_nuke_dir():
    nuke_path = os.environ.get('NUKE_PATH', '')
    if nuke_path:
        # NUKE_PATH may be colon/semicolon separated; take the first writable dir
        for p in nuke_path.replace(';', os.pathsep).split(os.pathsep):
            if os.path.isdir(p):
                return p
    return os.path.join(os.path.expanduser('~'), '.nuke')


def copy_plugin(src, dst):
    """Copy src → dst, excluding build/dev directories."""
    os.makedirs(dst, exist_ok=True)
    for item in os.listdir(src):
        if item in EXCLUDE_DIRS or item in EXCLUDE_FILES:
            continue
        s = os.path.join(src, item)
        d = os.path.join(dst, item)
        if os.path.isdir(s):
            if item in EXCLUDE_DIRS:
                continue
            copy_plugin(s, d)
        else:
            shutil.copy2(s, d)


def remove_old_denoiser_lines(lines):
    """Remove legacy OIDN pre-loading block and old pluginAddPath/nuke.load lines."""
    result = []
    skip_oidn_block = False
    for line in lines:
        stripped = line.strip()
        # Skip legacy OIDN pre-loading lines (hardcoded path references)
        if 'oidn-2.1.0' in line or (skip_oidn_block and stripped.startswith(
                ('import ctypes', '_oidn_bin', '_k32', 'for _dll', 'del _ctypes'))):
            skip_oidn_block = True
            continue
        if skip_oidn_block and not stripped:
            skip_oidn_block = False
            continue
        # Remove old nuke-denoiser pluginAddPath and nuke.load lines
        if "pluginAddPath('./nuke-denoiser')" in line:
            continue
        if "nuke.load('denoiser')" in line and 'menu.py' not in line:
            continue
        result.append(line)
    return result


def remove_existing_block(lines):
    """Remove the guarded nuke-denoiser block if it already exists."""
    result = []
    inside = False
    for line in lines:
        if line.strip() == BLOCK_START:
            inside = True
            continue
        if line.strip() == BLOCK_END:
            inside = False
            continue
        if not inside:
            result.append(line)
    return result


def patch_init_py(init_py_path):
    if os.path.exists(init_py_path):
        with open(init_py_path, 'r', encoding='utf-8') as f:
            lines = f.readlines()
    else:
        lines = ['import nuke\n']

    lines = remove_old_denoiser_lines(lines)
    lines = remove_existing_block(lines)

    # Ensure file ends with newline before appending
    if lines and not lines[-1].endswith('\n'):
        lines[-1] += '\n'

    lines.append('\n' + BLOCK_CONTENT)

    with open(init_py_path, 'w', encoding='utf-8') as f:
        f.writelines(lines)

    print(f"  Patched: {init_py_path}")


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    nuke_dir   = find_nuke_dir()
    dst        = os.path.join(nuke_dir, PLUGIN_FOLDER)
    init_py    = os.path.join(nuke_dir, 'init.py')

    print(f"Nuke directory : {nuke_dir}")
    print(f"Plugin target  : {dst}")

    # Skip copy if already running from the target directory
    if os.path.normcase(script_dir) == os.path.normcase(dst):
        print("  Already installed in place — skipping copy.")
    else:
        print(f"  Copying plugin files...")
        copy_plugin(script_dir, dst)
        print(f"  Done.")

    print("Patching init.py...")
    patch_init_py(init_py)

    print()
    print("Installation complete. Restart Nuke to apply changes.")


if __name__ == '__main__':
    main()
