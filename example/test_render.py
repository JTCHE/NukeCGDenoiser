"""
Terminal-mode test: loads example.nk, renders Denoiser output to test_out.jpg.
Run: "C:/Program Files/Nuke16.0v8/Nuke16.0.exe" -i -t test_render.py
"""
import os, sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SCRIPT     = os.path.join(SCRIPT_DIR, 'example.nk')
OUT_FILE   = os.path.join(SCRIPT_DIR, 'test_out.jpg')

import nuke
nuke.scriptOpen(SCRIPT)

denoiser = nuke.toNode('Denoiser1')
if denoiser is None:
    print('[TEST FAIL] Denoiser1 node not found — plugin did not register')
    sys.exit(1)

print('[TEST] Device type:', denoiser['device'].value())
print('[TEST] Quality:',     denoiser['quality'].value())

write = nuke.nodes.Write(
    inputs=[denoiser],
    file=OUT_FILE.replace('\\', '/'),
    file_type='jpeg',
)

try:
    nuke.execute(write, 1, 1)
    if os.path.exists(OUT_FILE):
        size = os.path.getsize(OUT_FILE)
        print(f'[TEST PASS] Output: {OUT_FILE} ({size} bytes)')
    else:
        print('[TEST FAIL] Execute ran but no output file found')
        sys.exit(1)
except Exception as e:
    print(f'[TEST FAIL] Render exception: {e}')
    sys.exit(1)
finally:
    nuke.delete(write)
