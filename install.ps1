<#
Nuke Denoiser - Build & Install
Detects installed Nuke SDKs, bootstraps CMake / VS Build Tools (2022 or newer) /
OIDN 2.1.0 if missing, builds Denoiser.dll from source, and stages the plugin into
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

# --- VS C++ build tools detection / bootstrap (VS 2022 or newer) ---
function Get-VsWhereExe {
    foreach ($base in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
        if (-not $base) { continue }
        $p = Join-Path $base 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path $p) { return $p }
    }
    return $null
}

# Returns @{ Path; Major } for the newest VS (major 17 = 2022, 18 = 2026, ...) that has
# the x64 C++ compiler, or $null. A VS install without the C++ workload is skipped.
function Get-VsToolchain {
    $vswhere = Get-VsWhereExe
    if ($vswhere) {
        # -products * is needed to see Build Tools installs, not only full VS editions.
        $json = & $vswhere -version '[17.0,)' -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json
        # Join first: Windows PowerShell 5.1 parses line-by-line input badly, and emits a
        # JSON array as one object - assigning to a variable lets the pipe enumerate it.
        $items = ($json -join "`n") | ConvertFrom-Json
        $found = $items | Sort-Object { [version]$_.installationVersion } -Descending | Select-Object -First 1
        if ($found) {
            return @{ Path = $found.installationPath; Major = ([version]$found.installationVersion).Major }
        }
    }

    # vswhere's component query depends on COM/WMI plumbing that can be unavailable
    # (e.g. minimal Windows Server Core hosts) even when the compiler installed fine.
    # Fall back to a direct filesystem check for cl.exe.
    $best = $null
    foreach ($base in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
        if (-not $base) { continue }
        foreach ($cl in Get-ChildItem -Path (Join-Path $base 'Microsoft Visual Studio') -Recurse -Filter 'cl.exe' -ErrorAction SilentlyContinue) {
            if ($cl.FullName -notmatch '\\Microsoft Visual Studio\\(\d+)\\[^\\]+\\VC\\Tools\\MSVC\\.*\\bin\\Hostx64\\x64\\cl\.exe$') { continue }
            $major = if ($Matches[1] -eq '2022') { 17 } else { [int]$Matches[1] }   # folder is "2022" or "18", "19", ...
            if ($major -lt 17 -or ($best -and $best.Major -ge $major)) { continue }
            $best = @{ Path = ($cl.FullName -replace '\\VC\\Tools\\MSVC\\.*$', ''); Major = $major }
        }
    }
    return $best
}

# CMake's Visual Studio generator name for a VS major version, if this CMake has it.
# Older CMake (e.g. 3.31) has no generator for VS 2026 - the caller falls back to NMake.
function Get-VsGenerator {
    param([string]$CMakeExe, [int]$Major)
    $names = @{ 17 = 'Visual Studio 17 2022'; 18 = 'Visual Studio 18 2026' }
    $name = $names[$Major]
    if ($name -and ((& $CMakeExe --help) -join "`n").Contains($name)) { return $name }
    return $null
}

# Loads the VS x64 compiler environment (vcvars64.bat) into this PowerShell session.
function Import-VcVars {
    param([string]$VsPath)
    $vcvars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }
    # A temp .bat avoids cmd.exe quote rules for paths like "Program Files (x86)".
    $bat = Join-Path $env:TEMP 'nuke-denoiser-vcvars.bat'
    Set-Content -Path $bat -Value "@call `"$vcvars`" >nul`r`n@set" -Encoding ASCII
    $envDump = & cmd.exe /c $bat
    Remove-Item $bat -ErrorAction SilentlyContinue
    foreach ($line in $envDump) {
        if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw "cl.exe not on PATH after running $vcvars" }
}

function Install-Vs2022BuildTools {
    param([switch]$Unattended)

    if (-not $Unattended) {
        $choice = Show-Menu -Title 'Visual Studio C++ build tools (2022 or newer) were not found.' `
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

    $vs = Get-VsToolchain
    if (-not $vs) { throw 'Visual Studio C++ build tools still not detected after install.' }
    return $vs
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

    # Drop trailing blank lines, so each re-run does not add one more.
    while ($final.Count -gt 0 -and -not $final[$final.Count - 1].Trim()) { $final.RemoveAt($final.Count - 1) }

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

$vs = Get-VsToolchain
if (-not $vs) {
    $vs = Install-Vs2022BuildTools -Unattended:$Unattended
}
Write-Host "VS Build Tools : $($vs.Path) (v$($vs.Major))"

$oidnRoot = Get-OidnRoot
Write-Host "OIDN           : $oidnRoot"

$generator = Get-VsGenerator -CMakeExe $cmakeExe -Major $vs.Major
if ($generator) {
    Write-Host "Generator      : $generator"
    Write-Host 'Configuring...'
    & $cmakeExe -B $BuildDir -G $generator -DCMAKE_GENERATOR_INSTANCE="$($vs.Path)" -DOIDN_ROOT="$oidnRoot" -DCMAKE_PREFIX_PATH="$nukeSdk" $ScriptDir
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }

    Write-Host 'Building...'
    & $cmakeExe --build $BuildDir --config Release
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    $builtDll = Join-Path $BuildDir 'lib\Release\Denoiser.dll'
} else {
    # This CMake has no generator for this VS version: build with NMake in the VS env.
    # Separate build dir, because CMake refuses to switch generators in one dir.
    $nmakeDir = Join-Path $BuildDir 'nmake'
    Write-Host 'Generator      : NMake Makefiles (no Visual Studio generator for this VS in this CMake)'
    Import-VcVars -VsPath $vs.Path
    Write-Host 'Configuring...'
    & $cmakeExe -B $nmakeDir -G 'NMake Makefiles' -DCMAKE_BUILD_TYPE=Release -DOIDN_ROOT="$oidnRoot" -DCMAKE_PREFIX_PATH="$nukeSdk" $ScriptDir
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }

    Write-Host 'Building...'
    & $cmakeExe --build $nmakeDir
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    $builtDll = Join-Path $nmakeDir 'lib\Denoiser.dll'
}

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
