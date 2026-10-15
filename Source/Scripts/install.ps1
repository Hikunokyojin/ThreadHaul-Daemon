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
        powershell -ExecutionPolicy Bypass -File Source\Scripts\install.ps1

.PARAMETER BinDir
    Folder holding the built ThreadHaulDaemon.exe (default: <repo>\build).
.PARAMETER ConfigSource
    config.json to install (default: <repo>\Source\Config\config.json).
.PARAMETER Account
    LocalSystem (default) or LocalService. LocalService is lower privilege but
    can only manage processes that also run as LocalService.
.PARAMETER OverwriteConfig
    Replace an existing installed config.json.
.PARAMETER NoStart
    Install without starting.
#>
[CmdletBinding()]
param(
    [string]$BinDir,
    [string]$ConfigSource,
    [ValidateSet('LocalSystem', 'LocalService')]
    [string]$Account = 'LocalSystem',
    [switch]$OverwriteConfig,
    [switch]$NoStart
)

$ErrorActionPreference = 'Stop'
$ServiceName = 'ThreadHaulDaemon'
$Repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
if (-not $BinDir) { $BinDir = Join-Path $Repo 'build' }
if (-not $ConfigSource) { $ConfigSource = Join-Path $Repo 'Source\Config\config.json' }

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

if ($OverwriteConfig -or -not (Test-Path $ConfigPath)) {
    Copy-Item $ConfigSource $ConfigPath -Force
    Write-Host "Installed config: $ConfigPath"
} else {
    Write-Host "Keeping existing config: $ConfigPath (use -OverwriteConfig to replace)"
}

# Least privilege file access. Inheritance is removed so Users get nothing.
$svcPrincipal = if ($Account -eq 'LocalService') { '*S-1-5-19' } else { '*S-1-5-18' }  # LocalService / SYSTEM
icacls $ConfigPath /inheritance:r /grant:r '*S-1-5-18:(F)' '*S-1-5-32-544:(F)' "${svcPrincipal}:(R)" | Out-Null
icacls $LogDir /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)(F)' '*S-1-5-32-544:(OI)(CI)(F)' "${svcPrincipal}:(OI)(CI)(M)" | Out-Null
Write-Host 'Config locked to SYSTEM + Administrators (write), service account (read).'

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
$obj = if ($Account -eq 'LocalService') { 'NT AUTHORITY\LocalService' } else { 'LocalSystem' }
sc.exe create $ServiceName binPath= $binPath start= auto obj= $obj DisplayName= 'ThreadHaul Resource Monitor' | Out-Null
if ($LASTEXITCODE -ne 0) { throw "sc.exe create failed ($LASTEXITCODE)" }
sc.exe description $ServiceName 'Protects critical dispatch processes from CPU starvation by throttling and suspending batch processes.' | Out-Null
sc.exe failure $ServiceName reset= 86400 actions= restart/5000/restart/5000/restart/30000 | Out-Null
# Strip every privilege the service does not need.
sc.exe privs $ServiceName 'SeDebugPrivilege/SeChangeNotifyPrivilege' | Out-Null
sc.exe sidtype $ServiceName unrestricted | Out-Null
Write-Host "Service '$ServiceName' created (auto start, account $obj)."

if (-not $NoStart) {
    Start-Service $ServiceName
    Start-Sleep -Seconds 2
    Get-Service $ServiceName | Format-Table Name, Status, StartType -AutoSize
    Write-Host "Log: $LogDir\threadhaul.jsonl"
    Write-Host "Try: & '$Exe' status"
}
