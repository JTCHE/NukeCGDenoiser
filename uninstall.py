"""
Nuke Denoiser — Uninstaller
Removes the guarded block from ~/.nuke/init.py and optionally deletes the plugin folder.
"""
import os
import sys
import shutil

PLUGIN_FOLDER = 'nuke-denoiser'
BLOCK_START   = '# --- nuke-denoiser START ---'
BLOCK_END     = '# --- nuke-denoiser END ---'


def find_nuke_dir():
    nuke_path = os.environ.get('NUKE_PATH', '')
    if nuke_path:
        for p in nuke_path.replace(';', os.pathsep).split(os.pathsep):
            if os.path.isdir(p):
                return p
    return os.path.join(os.path.expanduser('~'), '.nuke')


def remove_block(init_py_path):
    if not os.path.exists(init_py_path):
        print(f"  Not found: {init_py_path} — nothing to remove.")
        return

    with open(init_py_path, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    result = []
    inside = False
    removed = False
    for line in lines:
        if line.strip() == BLOCK_START:
            inside = True
            removed = True
            continue
        if line.strip() == BLOCK_END:
            inside = False
            continue
        if not inside:
            result.append(line)

    if removed:
        with open(init_py_path, 'w', encoding='utf-8') as f:
            f.writelines(result)
        print(f"  Removed nuke-denoiser block from {init_py_path}")
    else:
        print(f"  No nuke-denoiser block found in {init_py_path}")


def main():
    nuke_dir   = find_nuke_dir()
    init_py    = os.path.join(nuke_dir, 'init.py')
    plugin_dir = os.path.join(nuke_dir, PLUGIN_FOLDER)

    print(f"Nuke directory : {nuke_dir}")
    print()

    print("Patching init.py...")
    remove_block(init_py)

    print()
    if os.path.isdir(plugin_dir):
        answer = input(f"Delete plugin folder '{plugin_dir}'? [y/N] ").strip().lower()
        if answer == 'y':
            shutil.rmtree(plugin_dir)
            print(f"  Deleted: {plugin_dir}")
        else:
            print("  Kept plugin folder.")
    else:
        print(f"  Plugin folder not found at {plugin_dir} — nothing to delete.")

    print()
    print("Uninstall complete. Restart Nuke to apply changes.")


if __name__ == '__main__':
    main()
