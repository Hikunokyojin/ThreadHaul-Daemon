<#
.SYNOPSIS
    Builds the ThreadHaul MSI from an existing build.

.DESCRIPTION
    Uses the WiX toolset pinned in dotnet-tools.json (restored automatically).
    Run build.cmd first, then:
        powershell -ExecutionPolicy Bypass -File .\scripts\build-msi.ps1 -Version 2.2.0
    Output: out\ThreadHaul-<Version>-win-x64.msi
.PARAMETER Version
    Product version (major.minor.patch). Defaults to the version in CMakeLists.txt.
.PARAMETER BinDir
    Folder with ThreadHaulDaemon.exe, ThreadHaulController.exe and dashboard\ (default build\bin).
.PARAMETER OutDir
    Output folder (default out).
#>
[CmdletBinding()]
param(
    [string]$Version,
    [string]$BinDir,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $BinDir) { $BinDir = Join-Path $Repo 'build\bin' }
if (-not $OutDir) { $OutDir = Join-Path $Repo 'out' }
if (-not $Version) {
    $m = Select-String -Path (Join-Path $Repo 'CMakeLists.txt') -Pattern 'VERSION (\d+\.\d+\.\d+)' | Select-Object -First 1
    if (-not $m) { throw 'Could not read the version from CMakeLists.txt; pass -Version.' }
    $Version = $m.Matches[0].Groups[1].Value
}
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Version must look like 2.2.0 (got '$Version')." }

foreach ($f in 'ThreadHaulDaemon.exe', 'ThreadHaulController.exe', 'dashboard\index.html', 'dashboard\openapi.json') {
    if (-not (Test-Path (Join-Path $BinDir $f))) { throw "Missing $f in $BinDir. Run build.cmd first." }
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$msi = Join-Path $OutDir "ThreadHaul-$Version-win-x64.msi"

Push-Location $Repo
try {
    dotnet tool restore | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'dotnet tool restore failed.' }
    dotnet tool run wix -- extension add WixToolset.Util.wixext/5.0.2 | Out-Null
    dotnet tool run wix -- build (Join-Path $Repo 'installer\ThreadHaul.wxs') `
        -arch x64 -ext WixToolset.Util.wixext `
        -d "BinDir=$BinDir" -d "RepoDir=$Repo" -d "Version=$Version" `
        -o $msi
    if ($LASTEXITCODE -ne 0) { throw "wix build failed ($LASTEXITCODE)." }
} finally {
    Pop-Location
}
Write-Host "MSI: $msi"
