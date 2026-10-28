<#
.SYNOPSIS
    Build NCSender as ONE standalone NCSender.exe (no DLLs, no installer).

.DESCRIPTION
    Needs a static build of Qt (README: "Standalone single-exe build").
    Runs the Visual Studio compiler setup itself, so a normal PowerShell
    window is fine. The result is dist\NCSender.exe.

.EXAMPLE
    .\build-standalone.ps1
    .\build-standalone.ps1 -QtDir D:\Qt\6.8.3-static
#>
param(
    [string]$QtDir = "C:\Qt\6.8.3-static"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $root "build-standalone"
$distDir = Join-Path $root "dist"

if (-not (Test-Path (Join-Path $QtDir "lib\cmake\Qt6"))) {
    Write-Host "Static Qt not found at $QtDir. See README, 'Standalone single-exe build'." -ForegroundColor Red
    exit 1
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } else { $null }
if (-not $vsPath) {
    Write-Host "Visual Studio C++ build tools not found." -ForegroundColor Red
    exit 1
}
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"

$configure = "cmake -S `"$root`" -B `"$buildDir`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=`"$QtDir`" " +
             "-DNCSENDER_BUILD_TOOLS=OFF -DNCSENDER_BUILD_TESTS=OFF"
$build = "cmake --build `"$buildDir`" --target NCSender"
cmd /c "call `"$vcvars`" >nul 2>&1 && $configure && $build"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

New-Item -ItemType Directory -Force $distDir | Out-Null
Copy-Item (Join-Path $buildDir "bin\NCSender.exe") (Join-Path $distDir "NCSender.exe") -Force
$size = [math]::Round((Get-Item (Join-Path $distDir "NCSender.exe")).Length / 1MB, 1)
Write-Host ""
Write-Host "Built: $distDir\NCSender.exe ($size MB) - copy this one file anywhere and run it." -ForegroundColor Green
