<#
Nuke Denoiser - Build & Install
Detects installed Nuke SDKs, bootstraps CMake / VS2022 Build Tools / OIDN 2.1.0 if
missing, builds Denoiser.dll from source, and stages the plugin into
~/.nuke/nuke-denoiser/ (or $NUKE_PATH). Idempotent: safe to run multiple times.
#>
param(
    [string]$NukePath,
    [switch]$Unattended
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir  = Join-Path $ScriptDir 'build'
$DepsDir   = Join-Path $BuildDir 'deps'

$OidnVersion   = '2.1.0'
$OidnExtracted = Join-Path $DepsDir "oidn-$OidnVersion.x64.windows"
$OidnRoot      = Join-Path $DepsDir "oidn-$OidnVersion"
$OidnUrl       = "https://github.com/RenderKit/oidn/releases/download/v$OidnVersion/oidn-$OidnVersion.x64.windows.zip"

$CMakeVersion = '3.29.3'
$CMakeDir     = Join-Path $DepsDir 'cmake'

$PluginFolder = 'nuke-denoiser'
$BlockStart   = '# --- nuke-denoiser START ---'
$BlockEnd     = '# --- nuke-denoiser END ---'

# --- Arrow-key menu (no TUI framework needed) ---
function Show-Menu {
    param([string]$Title, [string[]]$Options)
    $selected = 0
    while ($true) {
        Clear-Host
        Write-Host "$Title`n"
        for ($i = 0; $i -lt $Options.Count; $i++) {
            if ($i -eq $selected) { Write-Host "> $($Options[$i])" -ForegroundColor Cyan }
            else { Write-Host "  $($Options[$i])" }
        }
        $key = [Console]::ReadKey($true).Key
        switch ($key) {
            'UpArrow'   { $selected = ($selected - 1 + $Options.Count) % $Options.Count }
            'DownArrow' { $selected = ($selected + 1) % $Options.Count }
            'Enter'     { return $selected }
        }
    }
}

# --- Nuke SDK detection ---
function Find-NukeSdks {
    $found = @()
    foreach ($drive in [System.IO.DriveInfo]::GetDrives()) {
        if ($drive.DriveType -ne 'Fixed') { continue }
        $pf = Join-Path $drive.Name 'Program Files'
        if (-not (Test-Path $pf)) { continue }
        foreach ($dir in Get-ChildItem -Path $pf -Directory -Filter 'Nuke*' -ErrorAction SilentlyContinue) {
            if (Test-Path (Join-Path $dir.FullName 'cmake\NukeConfig.cmake')) {
                $found += $dir.FullName
            }
        }
    }
    return $found
}

function Select-NukeSdk {
    param([string]$NukePathParam)

    if ($NukePathParam) {
        if (-not (Test-Path (Join-Path $NukePathParam 'cmake\NukeConfig.cmake'))) {
            throw "No cmake\NukeConfig.cmake found under '$NukePathParam' - not a valid Nuke SDK root."
        }
        return $NukePathParam
    }

    $sdks = @(Find-NukeSdks)
    $options = $sdks + @('Enter path manually...')
    $choice = Show-Menu -Title 'Select a Nuke installation to build against:' -Options $options
    if ($choice -eq $options.Count - 1) {
        $manual = Read-Host 'Enter the full path to the Nuke installation'
        if (-not (Test-Path (Join-Path $manual 'cmake\NukeConfig.cmake'))) {
            throw "No cmake\NukeConfig.cmake found under '$manual' - not a valid Nuke SDK root."
        }
        return $manual
    }
    return $sdks[$choice]
}

# --- CMake detection / bootstrap (portable zip, no admin needed) ---
function Get-CMakeExe {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $existing = Get-ChildItem -Path $CMakeDir -Recurse -Filter 'cmake.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($existing) { return $existing.FullName }

    Write-Host "CMake not found on PATH - downloading portable CMake $CMakeVersion..."
    New-Item -ItemType Directory -Force -Path $CMakeDir | Out-Null
    $zipPath = Join-Path $DepsDir "cmake-$CMakeVersion-windows-x86_64.zip"
    $url = "https://github.com/Kitware/CMake/releases/download/v$CMakeVersion/cmake-$CMakeVersion-windows-x86_64.zip"
    Invoke-WebRequest -Uri $url -OutFile $zipPath
    Expand-Archive -Path $zipPath -DestinationPath $CMakeDir -Force

    $exe = Get-ChildItem -Path $CMakeDir -Recurse -Filter 'cmake.exe' | Select-Object -First 1
    if (-not $exe) { throw 'Failed to extract portable CMake.' }
    return $exe.FullName
}

# --- VS2022 C++ build tools detection / bootstrap ---
function Get-VsWhereExe {
    foreach ($base in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
        if (-not $base) { continue }
        $p = Join-Path $base 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path $p) { return $p }
    }
    return $null
}

function Get-Vs2022Path {
    $vswhere = Get-VsWhereExe
    if ($vswhere) {
        $installPath = & $vswhere -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($installPath) { return ($installPath | Select-Object -First 1) }
    }

    # vswhere's component query depends on COM/WMI plumbing that can be unavailable
    # (e.g. minimal Windows Server Core hosts) even when the compiler installed fine.
    # Fall back to a direct filesystem check for cl.exe.
    foreach ($base in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
        if (-not $base) { continue }
        $cl = Get-ChildItem -Path (Join-Path $base 'Microsoft Visual Studio\2022') -Recurse -Filter 'cl.exe' -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\VC\\Tools\\MSVC\\.*\\bin\\Hostx64\\x64\\cl\.exe$' } |
            Select-Object -First 1
        if ($cl) {
            return ($cl.FullName -replace '\\VC\\Tools\\MSVC\\.*$', '')
        }
    }
    return $null
}

function Install-Vs2022BuildTools {
    param([switch]$Unattended)

    if (-not $Unattended) {
        $choice = Show-Menu -Title 'Visual Studio 2022 C++ build tools were not found.' `
            -Options @('Download and install Visual Studio Build Tools 2022 (required to build the plugin)', 'Cancel')
        if ($choice -eq 1) { throw 'Build cancelled - VS2022 C++ build tools are required.' }
    }

    $installerPath = Join-Path $env:TEMP 'vs_buildtools.exe'
    Write-Host 'Downloading Visual Studio Build Tools 2022 installer...'
    Invoke-WebRequest -Uri 'https://aka.ms/vs/17/release/vs_buildtools.exe' -OutFile $installerPath

    $installArgs = @('--add', 'Microsoft.VisualStudio.Workload.VCTools', '--includeRecommended')
    if ($Unattended) { $installArgs += @('--quiet', '--wait', '--norestart') }

    Write-Host 'Launching Visual Studio Build Tools installer...'
    Start-Process -FilePath $installerPath -ArgumentList $installArgs -Wait

    $vsPath = Get-Vs2022Path
    if (-not $vsPath) { throw 'Visual Studio 2022 C++ build tools still not detected after install.' }
    return $vsPath
}

# --- OIDN fetch (pinned to 2.1.0 per CLAUDE.md - do not bump without checking TBB ABI) ---
function Get-OidnRoot {
    if (Test-Path (Join-Path $OidnRoot 'include\OpenImageDenoise\oidn.hpp')) {
        return $OidnRoot
    }

    Write-Host "Downloading OIDN $OidnVersion..."
    New-Item -ItemType Directory -Force -Path $DepsDir | Out-Null
    $zipPath = Join-Path $DepsDir "oidn-$OidnVersion.x64.windows.zip"
    Invoke-WebRequest -Uri $OidnUrl -OutFile $zipPath
    Expand-Archive -Path $zipPath -DestinationPath $DepsDir -Force

    if (Test-Path $OidnRoot) { Remove-Item -Recurse -Force $OidnRoot }
    Rename-Item -Path $OidnExtracted -NewName (Split-Path -Leaf $OidnRoot)

    if (-not (Test-Path (Join-Path $OidnRoot 'include\OpenImageDenoise\oidn.hpp'))) {
        throw "OIDN extraction failed - expected headers not found in $OidnRoot"
    }
    return $OidnRoot
}

# --- Nuke user dir + init.py patching (ported from the old install.py) ---
function Find-NukeUserDir {
    $nukePathEnv = $env:NUKE_PATH
    if ($nukePathEnv) {
        foreach ($p in $nukePathEnv -split '[;:]') {
            if ($p -and (Test-Path $p -PathType Container)) { return $p }
        }
    }
    return (Join-Path $env:USERPROFILE '.nuke')
}

function Update-InitPy {
    param([string]$InitPyPath)

    if (Test-Path $InitPyPath) {
        $lines = @(Get-Content -Path $InitPyPath -Encoding UTF8)
    } else {
        $lines = @('import nuke')
    }

    # Remove legacy OIDN pre-loading block and old pluginAddPath/nuke.load lines
    $afterLegacy = New-Object System.Collections.Generic.List[string]
    $skipOidnBlock = $false
    foreach ($line in $lines) {
        $stripped = $line.Trim()
        if ($line -match 'oidn-2\.1\.0' -or ($skipOidnBlock -and (
                $stripped.StartsWith('import ctypes') -or $stripped.StartsWith('_oidn_bin') -or
                $stripped.StartsWith('_k32') -or $stripped.StartsWith('for _dll') -or
                $stripped.StartsWith('del _ctypes')))) {
            $skipOidnBlock = $true
            continue
        }
        if ($skipOidnBlock -and -not $stripped) { $skipOidnBlock = $false; continue }
        if ($line -match "pluginAddPath\('\./nuke-denoiser'\)") { continue }
        if ($line -match "nuke\.load\('denoiser'\)" -and $line -notmatch 'menu\.py') { continue }
        $afterLegacy.Add($line)
    }

    # Remove the existing guarded block, if present, so re-running is idempotent
    $final = New-Object System.Collections.Generic.List[string]
    $inside = $false
    foreach ($line in $afterLegacy) {
        if ($line.Trim() -eq $BlockStart) { $inside = $true; continue }
        if ($line.Trim() -eq $BlockEnd) { $inside = $false; continue }
        if (-not $inside) { $final.Add($line) }
    }

    $final.Add('')
    $final.Add($BlockStart)
    $final.Add("nuke.pluginAddPath('./nuke-denoiser')")
    $final.Add($BlockEnd)

    Set-Content -Path $InitPyPath -Value $final -Encoding UTF8
    Write-Host "  Patched: $InitPyPath"
}

# --- Main ---
Write-Host '=== Nuke Denoiser: Build & Install ==='

$nukeSdk = Select-NukeSdk -NukePathParam $NukePath
Write-Host "Nuke SDK       : $nukeSdk"

$cmakeExe = Get-CMakeExe
Write-Host "CMake          : $cmakeExe"

$vsPath = Get-Vs2022Path
if (-not $vsPath) {
    $vsPath = Install-Vs2022BuildTools -Unattended:$Unattended
}
Write-Host "VS2022 Tools   : $vsPath"

$oidnRoot = Get-OidnRoot
Write-Host "OIDN           : $oidnRoot"

Write-Host 'Configuring...'
& $cmakeExe -B $BuildDir -G 'Visual Studio 17 2022' -DOIDN_ROOT="$oidnRoot" -DCMAKE_PREFIX_PATH="$nukeSdk" $ScriptDir
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }

Write-Host 'Building...'
& $cmakeExe --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }

$builtDll = Join-Path $BuildDir 'lib\Release\Denoiser.dll'
if (-not (Test-Path $builtDll)) { throw "Build succeeded but Denoiser.dll not found at $builtDll" }

$nukeUserDir = Find-NukeUserDir
$dst = Join-Path $nukeUserDir $PluginFolder
$dstOidnBin = Join-Path $dst 'oidn\bin'
New-Item -ItemType Directory -Force -Path $dstOidnBin | Out-Null

Write-Host "Staging plugin into $dst..."
Copy-Item -Path $builtDll -Destination (Join-Path $dst 'denoiser.dll') -Force
Copy-Item -Path (Join-Path $ScriptDir 'init.py') -Destination $dst -Force

$nukeSdkMajor = if ((Split-Path -Leaf $nukeSdk) -match 'Nuke(\d+)\.') { $Matches[1] } else { '' }
Set-Content -Path (Join-Path $dst 'built_for_nuke_version.txt') -Value $nukeSdkMajor -NoNewline
Copy-Item -Path (Join-Path $ScriptDir 'menu.py') -Destination $dst -Force

$oidnRuntimeDlls = @(
    'OpenImageDenoise.dll', 'OpenImageDenoise_core.dll', 'OpenImageDenoise_device_cpu.dll',
    'OpenImageDenoise_device_cuda.dll', 'tbb12.dll', 'tbbbind.dll', 'tbbbind_2_0.dll', 'tbbbind_2_5.dll'
)
foreach ($dll in $oidnRuntimeDlls) {
    $src = Join-Path $oidnRoot "bin\$dll"
    if (Test-Path $src) { Copy-Item -Path $src -Destination $dstOidnBin -Force }
}

Write-Host 'Patching init.py...'
Update-InitPy -InitPyPath (Join-Path $nukeUserDir 'init.py')

Write-Host ''
Write-Host 'Installation complete. Restart Nuke to apply changes.'
