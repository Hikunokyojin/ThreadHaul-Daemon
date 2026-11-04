<#
.SYNOPSIS
    Installs ThreadHaulDaemon as an auto-start Windows Service.

.DESCRIPTION
    - Copies ThreadHaulDaemon.exe to "%ProgramFiles%\ThreadHaulDaemon".
    - Puts config.json in "%ProgramData%\ThreadHaulDaemon" (kept if it exists,
      unless -OverwriteConfig) and locks it down: only SYSTEM and
      Administrators can modify it; the service account can only read it.
    - Registers the "ThreadHaulDaemon" Application event-log source.
    - Creates the service (auto start, restart on failure) with only the
      privileges it needs (SeDebugPrivilege, SeChangeNotifyPrivilege).
    - Starts it.

    Run from an elevated PowerShell:
        powershell -ExecutionPolicy Bypass -File scripts\install.ps1

.PARAMETER BinDir
    Folder holding ThreadHaulDaemon.exe and dashboard\ (default: <repo>\build\bin
    after build.cmd, or <package>\bin in a release zip).
.PARAMETER ConfigSource
    config.json to install (default: <repo>\config\config.example.json).
.PARAMETER Account
    LocalSystem (default), LocalService, or Virtual.
    Virtual runs the service as its own virtual account, NT SERVICE\ThreadHaulDaemon:
    no password, no rights beyond what this script grants, and nothing shared
    with other services. LocalService and Virtual are lower privilege but do
    not hold SeDebugPrivilege, so they can only manage processes whose
    security allows it (typically ones started by the same account), not
    other users' programs.
.PARAMETER OverwriteConfig
    Replace an existing installed config.json.
.PARAMETER NoStart
    Install without starting.
#>
[CmdletBinding()]
param(
    [string]$BinDir,
    [string]$ConfigSource,
    [ValidateSet('LocalSystem', 'LocalService', 'Virtual')]
    [string]$Account = 'LocalSystem',
    [switch]$OverwriteConfig,
    [switch]$NoStart
)

$ErrorActionPreference = 'Stop'
$ServiceName = 'ThreadHaulDaemon'
$Repo = Resolve-Path (Join-Path $PSScriptRoot '..')
if (-not $BinDir) {
    $BinDir = @((Join-Path $Repo 'build\bin'), (Join-Path $Repo 'bin')) |
        Where-Object { Test-Path (Join-Path $_ 'ThreadHaulDaemon.exe') } | Select-Object -First 1
    if (-not $BinDir) { throw 'ThreadHaulDaemon.exe not found. Run build.cmd first, or pass -BinDir.' }
}
if (-not $ConfigSource) { $ConfigSource = Join-Path $Repo 'config\config.example.json' }

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated (Administrator) PowerShell.'
}

$exeSource = Join-Path $BinDir 'ThreadHaulDaemon.exe'
if (-not (Test-Path $exeSource)) { throw "Not found: $exeSource. Run build.cmd first." }

$InstallDir = Join-Path $env:ProgramFiles 'ThreadHaulDaemon'
$DataDir = Join-Path $env:ProgramData 'ThreadHaulDaemon'
$LogDir = Join-Path $DataDir 'logs'
$ConfigPath = Join-Path $DataDir 'config.json'
$Exe = Join-Path $InstallDir 'ThreadHaulDaemon.exe'

# Validate the config with the binary we are about to install.
& $exeSource validate-config $ConfigSource
if ($LASTEXITCODE -ne 0) { throw "config.json failed validation: $ConfigSource" }

# Replace a previous installation cleanly (the daemon restores processes on stop).
if (Get-Service $ServiceName -ErrorAction SilentlyContinue) {
    Write-Host 'Removing existing service...'
    Stop-Service $ServiceName -Force -ErrorAction SilentlyContinue
    sc.exe delete $ServiceName | Out-Null
    Start-Sleep -Seconds 2
}

Write-Host "Installing binaries to $InstallDir"
New-Item -ItemType Directory -Force -Path $InstallDir, $DataDir, $LogDir | Out-Null
Copy-Item $exeSource $Exe -Force
$dashSource = Join-Path $BinDir 'dashboard'
if (Test-Path $dashSource) {
    Copy-Item $dashSource (Join-Path $InstallDir 'dashboard') -Recurse -Force
} else {
    Write-Warning "Dashboard files not found in $dashSource; the web dashboard will be unavailable."
}

if ($OverwriteConfig -or -not (Test-Path $ConfigPath)) {
    Copy-Item $ConfigSource $ConfigPath -Force
    Write-Host "Installed config: $ConfigPath"
} else {
    Write-Host "Keeping existing config: $ConfigPath (use -OverwriteConfig to replace)"
}

# Event-log source. EventLogMessages.dll passes the message text through,
# so Event Viewer shows our text without a "description not found" note.
$msgDll = Join-Path $env:SystemRoot 'Microsoft.NET\Framework64\v4.0.30319\EventLogMessages.dll'
if (-not [System.Diagnostics.EventLog]::SourceExists($ServiceName)) {
    if (Test-Path $msgDll) {
        New-EventLog -LogName Application -Source $ServiceName -MessageResourceFile $msgDll
    } else {
        New-EventLog -LogName Application -Source $ServiceName
    }
    Write-Host 'Registered event-log source ThreadHaulDaemon (Application log).'
}

$binPath = "`"$Exe`" --config `"$ConfigPath`""
$obj = switch ($Account) {
    'LocalService' { 'NT AUTHORITY\LocalService' }
    'Virtual'      { "NT SERVICE\$ServiceName" }
    default        { 'LocalSystem' }
}
# New-Service, not "sc.exe create": Windows PowerShell 5.1 strips the inner
# quotes of a native-command argument, which breaks a binPath containing
# spaces ("C:\Program Files\...") and makes sc.exe fail with 1639.
$newService = @{
    Name           = $ServiceName
    BinaryPathName = $binPath
    DisplayName    = 'ThreadHaul Resource Monitor'
    Description    = 'Protects critical dispatch processes from CPU starvation by throttling and suspending batch processes.'
    StartupType    = 'Automatic'
}
if ($Account -ne 'LocalSystem') {
    # Built-in and virtual accounts have no password.
    $newService.Credential = New-Object System.Management.Automation.PSCredential(
        $obj, (New-Object System.Security.SecureString))
}
New-Service @newService | Out-Null

# These sc.exe arguments contain no quotes, so they pass through intact.
function Invoke-Sc {
    & sc.exe @args | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "sc.exe $($args -join ' ') failed ($LASTEXITCODE)" }
}
Invoke-Sc failure $ServiceName reset= 86400 actions= restart/5000/restart/5000/restart/30000
# Strip every privilege the service does not need.
Invoke-Sc privs $ServiceName SeDebugPrivilege/SeChangeNotifyPrivilege
Invoke-Sc sidtype $ServiceName unrestricted
Write-Host "Service '$ServiceName' created (auto start, account $obj)."

# Least privilege file access. Inheritance is removed so Users get nothing.
# Done after the service exists: the virtual account only resolves then.
$svcPrincipal = switch ($Account) {
    'LocalService' { '*S-1-5-19' }
    'Virtual'      { $obj }
    default        { '*S-1-5-18' }
}
icacls $ConfigPath /inheritance:r /grant:r '*S-1-5-18:(F)' '*S-1-5-32-544:(F)' "${svcPrincipal}:(R)" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Could not set permissions on $ConfigPath" }
icacls $LogDir /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)(F)' '*S-1-5-32-544:(OI)(CI)(F)' "${svcPrincipal}:(OI)(CI)(M)" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Could not set permissions on $LogDir" }
Write-Host "Config locked to SYSTEM + Administrators (write), $obj (read)."
if ($Account -ne 'LocalSystem') {
    Write-Warning "$obj has no SeDebugPrivilege: it can only throttle processes whose security allows it."
}

if (-not $NoStart) {
    Start-Service $ServiceName
    Start-Sleep -Seconds 2
    Get-Service $ServiceName | Format-Table Name, Status, StartType -AutoSize
    Write-Host "Log: $LogDir\threadhaul.jsonl"
    Write-Host "Try: & '$Exe' status"
    Write-Host "Dashboard: & '$Exe' dashboard"
}
