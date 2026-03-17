@echo off
echo Installing Nuke Denoiser...
python "%~dp0install.py" 2>nul
if not errorlevel 1 goto :done
echo System Python not found, trying Nuke's Python...
for /d %%G in ("C:\Program Files\Nuke*") do (
    if exist "%%G\python.exe" (
        "%%G\python.exe" "%~dp0install.py"
        goto :done
    )
)
echo ERROR: Python not found. Install Python or ensure Nuke is installed.
:done
pause
