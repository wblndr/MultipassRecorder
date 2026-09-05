<#
    Compile the MultipassRecorder addon as a 32-bit (x86 / Win32) DLL, for
    DirectX 9 games.

    WHY A SEPARATE SCRIPT: a ReShade addon DLL must match the *architecture of
    the host process*. Nearly every D3D9 title is a 32-bit executable, so the
    normal 64-bit build (build64/, `cmake -A x64`) produces a .addon that a
    DX9 game silently refuses to load. This configures a Win32 build tree in
    build32/ instead. (If your DX9 target is actually a 64-bit process, use the
    normal x64 build -- the graphics API is chosen by ReShade at runtime, not
    at compile time; only the bitness has to match.)

    USAGE (from the repo root):
        ./compile_dx9.ps1                    # configure + Release build
        ./compile_dx9.ps1 -Config Debug
        ./compile_dx9.ps1 -Clean             # wipe build32/ before configuring
        ./compile_dx9.ps1 -ApiVersion 8      # legacy ReShade 5.x DX9 host

    -ApiVersion maps to the MPR_TARGET_API_VERSION CMake cache var (default 13,
    same as the x64 build). Lower it only for an old ReShade 5.x host; see the
    Build notes in CLAUDE.md for what each version unlocks.

    OUTPUT: build32/Release/MultipassRecorderAddon.addon
    Deploy it next to the game's ReShade (d3d9) install, alongside
    MultipassRecorder.fx in reshade-shaders/Shaders/.
#>
[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Release",
    [int]$ApiVersion = 0,      # 0 = use CMake default (13)
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$RepoRoot  = $PSScriptRoot
$BuildDir  = Join-Path $RepoRoot "build32"
$AddonName = "MultipassRecorderAddon.addon"

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Host "[!] cmake not found on PATH. Install CMake (or open a Developer PowerShell)." -ForegroundColor Red
    exit 1
}

if (-not (Test-Path (Join-Path $RepoRoot "deps\reshade\source"))) {
    Write-Host "[!] ReShade SDK missing (deps/reshade). Run:" -ForegroundColor Red
    Write-Host "    git submodule update --init --recursive" -ForegroundColor Red
    exit 1
}

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "[*] Cleaning $BuildDir ..." -ForegroundColor Cyan
    Remove-Item -Recurse -Force $BuildDir
}

Write-Host "MultipassRecorder -- 32-bit (Win32) build for DirectX 9" -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

# Configure: -A Win32 forces the 32-bit toolset under the default VS generator.
$configureArgs = @("-S", $RepoRoot, "-B", $BuildDir, "-A", "Win32")
if ($ApiVersion -gt 0) {
    $configureArgs += "-DMPR_TARGET_API_VERSION=$ApiVersion"
    Write-Host "[*] Targeting ReShade addon API version $ApiVersion" -ForegroundColor Cyan
}

Write-Host "[*] Configuring..." -ForegroundColor Cyan
& cmake @configureArgs
if ($LASTEXITCODE -ne 0) { Write-Host "[!] Configure failed." -ForegroundColor Red; exit 1 }

Write-Host "[*] Building ($Config)..." -ForegroundColor Cyan
& cmake --build $BuildDir --config $Config
if ($LASTEXITCODE -ne 0) { Write-Host "[!] Build failed." -ForegroundColor Red; exit 1 }

$out = Join-Path $BuildDir (Join-Path $Config $AddonName)
if (Test-Path $out) {
    Write-Host ""
    Write-Host "[+] Build successful (32-bit)." -ForegroundColor Green
    Write-Host "    $out"
    Write-Host "    Copy this into your DX9 game's folder (next to the ReShade d3d9 DLL)," -ForegroundColor Green
    Write-Host "    and MultipassRecorder.fx into reshade-shaders\Shaders\." -ForegroundColor Green
} else {
    Write-Host "[!] Build reported success but $out is missing." -ForegroundColor Red
    exit 1
}
