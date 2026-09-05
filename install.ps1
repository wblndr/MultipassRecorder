<#
    MultipassRecorder installer.

    Run this from inside your game's install directory (the folder your game's
    .exe lives in, where ReShade is already installed):

        irm https://raw.githubusercontent.com/wblndr/MultipassRecorder/main/install.ps1 | iex

    Downloads the latest GitHub release's addon + shader and drops them into
    place. Requires ReShade (with full add-on support) already installed for
    the game -- this script does not install ReShade itself.
#>

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Repo      = "wblndr/MultipassRecorder"
$AddonName = "MultipassRecorderAddon.addon"
$FxName    = "MultipassRecorder.fx"
$TargetDir = (Get-Location).Path
$ShaderDir = Join-Path $TargetDir "reshade-shaders\Shaders"

Write-Host "MultipassRecorder installer" -ForegroundColor Cyan
Write-Host "Target directory: $TargetDir"

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "[!] Not running as Administrator. If this game lives under Program Files, writes may fail -- rerun from an elevated PowerShell if so." -ForegroundColor Yellow
}

try {
    $release = Invoke-RestMethod -UseBasicParsing `
        -Uri "https://api.github.com/repos/$Repo/releases/latest" `
        -Headers @{ "User-Agent" = "MultipassRecorder-Installer" }
} catch {
    Write-Host "[!] Could not reach the GitHub releases API: $_" -ForegroundColor Red
    exit 1
}

Write-Host "[*] Latest release: $($release.tag_name)"

function Get-ReleaseAsset($name) {
    $asset = $release.assets | Where-Object { $_.name -eq $name }
    if (-not $asset) {
        Write-Host "[!] Release $($release.tag_name) has no asset named '$name'." -ForegroundColor Red
        exit 1
    }
    return $asset
}

$addonAsset = Get-ReleaseAsset $AddonName
$fxAsset    = Get-ReleaseAsset $FxName

Write-Host "[*] Downloading $AddonName..."
try {
    Invoke-WebRequest -UseBasicParsing -Uri $addonAsset.browser_download_url `
        -OutFile (Join-Path $TargetDir $AddonName)
    Write-Host "[+] -> $TargetDir\$AddonName"
} catch {
    Write-Host "[!] Failed to write $AddonName to $TargetDir : $_" -ForegroundColor Red
    Write-Host "    If this folder is under Program Files, rerun from an elevated (Admin) PowerShell." -ForegroundColor Red
    exit 1
}

Write-Host "[*] Downloading $FxName..."
$fxDest = $TargetDir
try {
    New-Item -ItemType Directory -Force -Path $ShaderDir | Out-Null
    $fxDest = $ShaderDir
} catch {
    Write-Host "[!] Could not create reshade-shaders\Shaders, copying shader to game root instead." -ForegroundColor Yellow
}
try {
    Invoke-WebRequest -UseBasicParsing -Uri $fxAsset.browser_download_url `
        -OutFile (Join-Path $fxDest $FxName)
    Write-Host "[+] -> $fxDest\$FxName"
} catch {
    Write-Host "[!] Failed to write $FxName to $fxDest : $_" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Done. Before recording:" -ForegroundColor Green
Write-Host "  1. Make sure ReShade is installed for this game WITH full add-on support enabled."
Write-Host "  2. Launch the game, open the ReShade overlay, and enable the 'MultipassRecorder_Setup' technique."
Write-Host "  3. The MultipassRecorder panel lives in the ReShade overlay (Home by default)."
Write-Host "  4. F9 starts/stops a recording; F10 takes a still. Both are rebindable in the Setup tab."
