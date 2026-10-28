<#
.SYNOPSIS
    Configure and build NCSender (Release) with CMake.

.EXAMPLE
    .\build.ps1 -QtDir C:\Qt\6.8.3\msvc2022_64

.NOTES
    Run from a "Developer PowerShell for VS 2022" (MSVC kits), or make sure
    the matching MinGW is on PATH (MinGW kits). Output: build\bin\
#>
param(
    [string]$QtDir = $env:QTDIR,
    [string]$Config = "Release",
    [string]$Generator = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $QtDir) {
    # Pick the newest Qt 6 kit under C:\Qt if one exists.
    $kit = Get-ChildItem "C:\Qt" -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^6\.' } | Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Get-ChildItem $_.FullName -Directory | Where-Object { Test-Path (Join-Path $_.FullName "lib\cmake\Qt6") } } |
        Select-Object -First 1
    if ($kit) { $QtDir = $kit.FullName }
}
if (-not $QtDir -or -not (Test-Path (Join-Path $QtDir "lib\cmake\Qt6"))) {
    Write-Host "Qt 6 not found. Pass -QtDir, e.g. -QtDir C:\Qt\6.8.3\msvc2022_64" -ForegroundColor Red
    exit 1
}
Write-Host "Using Qt: $QtDir"

if (-not $Generator) {
    if (Get-Command ninja -ErrorAction SilentlyContinue) { $Generator = "Ninja" }
}

$cfg = @("-S", $root, "-B", (Join-Path $root "build"), "-DCMAKE_PREFIX_PATH=$QtDir")
if ($Generator) { $cfg += @("-G", $Generator) }
if ($Generator -eq "Ninja" -or $Generator -like "*Makefiles*") { $cfg += "-DCMAKE_BUILD_TYPE=$Config" }

& cmake @cfg
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --build (Join-Path $root "build") --config $Config --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$bin = Join-Path $root "build\bin"
if (Test-Path (Join-Path $bin $Config)) { $bin = Join-Path $bin $Config }
Write-Host ""
Write-Host "Built: $bin\NCSender.exe" -ForegroundColor Green
