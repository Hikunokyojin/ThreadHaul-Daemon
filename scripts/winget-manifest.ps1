<#
.SYNOPSIS
    Generates and validates the winget manifests for a published release.

.DESCRIPTION
    Writes the three manifest files winget-pkgs expects (version, installer,
    defaultLocale) for package Hikunokyojin.ThreadHaul, pointing at the
    release's MSI. The SHA-256 comes from the release's .msi.sha256 file and
    the ProductCode from the MSI itself (downloaded from the release, or given
    with -Msi). Then runs `winget validate`.

        powershell -ExecutionPolicy Bypass -File .\scripts\winget-manifest.ps1 -Version 2.2.0

    Output: out\winget\manifests\h\Hikunokyojin\ThreadHaul\<Version>\  (the
    folder layout of the winget-pkgs repository).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Version,
    [string]$Msi,
    [string]$OutDir,
    # Dry run before the release exists: hash the local -Msi instead of
    # checking it against the release's .sha256 file.
    [switch]$Local
)

$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Version must look like 2.2.0 (got '$Version')." }
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $OutDir) { $OutDir = Join-Path $Repo 'out\winget' }

$Id = 'Hikunokyojin.ThreadHaul'
$Tag = "v$Version"
$Asset = "ThreadHaul-$Tag-win-x64.msi"
$Base = "https://github.com/Hikunokyojin/ThreadHaul-Daemon/releases/download/$Tag"
$ManifestVersion = '1.6.0'

$work = Join-Path $OutDir 'download'
New-Item -ItemType Directory -Force -Path $work | Out-Null
if ($Local -and -not $Msi) { throw '-Local needs -Msi <path to the built MSI>.' }
if (-not $Msi) {
    $Msi = Join-Path $work $Asset
    Invoke-WebRequest -UseBasicParsing "$Base/$Asset" -OutFile $Msi
}
$actual = (Get-FileHash $Msi -Algorithm SHA256).Hash.ToUpper()
if ($Local) {
    Write-Warning 'Dry run: the SHA-256 is from the local MSI; regenerate from the release before submitting.'
    $sha = $actual
} else {
    $shaFile = Join-Path $work "$Asset.sha256"
    Invoke-WebRequest -UseBasicParsing "$Base/$Asset.sha256" -OutFile $shaFile
    $sha = ((Get-Content $shaFile -Raw).Trim() -split '\s+')[0].ToUpper()
    if ($sha -ne $actual) { throw "The MSI's SHA-256 ($actual) does not match the release's .sha256 file ($sha)." }
}

# ProductCode from the MSI database (lets winget detect the installed version).
$installer = New-Object -ComObject WindowsInstaller.Installer
$db = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $installer, @($Msi, 0))
$view = $db.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $db, @("SELECT Value FROM Property WHERE Property='ProductCode'"))
$view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null) | Out-Null
$record = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $view, $null)
$productCode = $record.GetType().InvokeMember('StringData', 'GetProperty', $null, $record, 1)
$view.GetType().InvokeMember('Close', 'InvokeMethod', $null, $view, $null) | Out-Null
[System.Runtime.InteropServices.Marshal]::ReleaseComObject($db) | Out-Null
if ($productCode -notmatch '^\{[0-9A-F-]{36}\}$') { throw "Could not read the ProductCode from $Msi." }

$dir = Join-Path $OutDir "manifests\h\Hikunokyojin\ThreadHaul\$Version"
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$header = "# Created with scripts/winget-manifest.ps1 in https://github.com/Hikunokyojin/ThreadHaul-Daemon"
$utf8 = New-Object System.Text.UTF8Encoding($false)

[IO.File]::WriteAllText((Join-Path $dir "$Id.yaml"), @"
$header
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.version.$ManifestVersion.schema.json

PackageIdentifier: $Id
PackageVersion: $Version
DefaultLocale: en-US
ManifestType: version
ManifestVersion: $ManifestVersion
"@, $utf8)

[IO.File]::WriteAllText((Join-Path $dir "$Id.installer.yaml"), @"
$header
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.installer.$ManifestVersion.schema.json

PackageIdentifier: $Id
PackageVersion: $Version
MinimumOSVersion: 10.0.17763.0
InstallerType: msi
Scope: machine
InstallModes:
- interactive
- silent
- silentWithProgress
UpgradeBehavior: install
ElevationRequirement: elevatesSelf
ProductCode: '$productCode'
Installers:
- Architecture: x64
  InstallerUrl: $Base/$Asset
  InstallerSha256: $sha
ManifestType: installer
ManifestVersion: $ManifestVersion
"@, $utf8)

[IO.File]::WriteAllText((Join-Path $dir "$Id.locale.en-US.yaml"), @"
$header
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.defaultLocale.$ManifestVersion.schema.json

PackageIdentifier: $Id
PackageVersion: $Version
PackageLocale: en-US
Publisher: ThreadHaul contributors
PublisherUrl: https://github.com/Hikunokyojin
PublisherSupportUrl: https://github.com/Hikunokyojin/ThreadHaul-Daemon/issues
PackageName: ThreadHaul
PackageUrl: https://github.com/Hikunokyojin/ThreadHaul-Daemon
License: Apache-2.0
LicenseUrl: https://github.com/Hikunokyojin/ThreadHaul-Daemon/blob/main/LICENSE
ShortDescription: Windows service that keeps latency-critical workloads responsive under CPU pressure.
Description: |-
  ThreadHaul watches the programs you mark as critical. When they run short of CPU it lowers the
  priority of, and if needed suspends, the batch programs you name, then restores them when the
  pressure passes. It also protects the whole machine from runaway programs, and includes a local
  web dashboard, a tamper-evident log, Prometheus metrics and multi-server coordination.
Moniker: threadhaul
Tags:
- cpu
- priority
- process-management
- performance
- windows-service
- throttling
ReleaseNotesUrl: https://github.com/Hikunokyojin/ThreadHaul-Daemon/releases/tag/$Tag
ManifestType: defaultLocale
ManifestVersion: $ManifestVersion
"@, $utf8)

Write-Host "Manifests: $dir"
winget validate --manifest $dir
if ($LASTEXITCODE -ne 0) { throw "winget validate failed ($LASTEXITCODE)." }
