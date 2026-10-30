<#
.SYNOPSIS
    Stops and removes the ThreadHaulDaemon service.

.DESCRIPTION
    Stopping the service resumes every suspended thread and restores normal
    priorities before it exits. Then the service, its event-log source and
    the program files are removed. Config and logs in
    %ProgramData%\ThreadHaulDaemon are kept unless -RemoveData is given.

        powershell -ExecutionPolicy Bypass -File scripts\uninstall.ps1 [-RemoveData]
#>
[CmdletBinding()]
param([switch]$RemoveData)

$ErrorActionPreference = 'Stop'
$ServiceName = 'ThreadHaulDaemon'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated (Administrator) PowerShell.'
}

if (Get-Service $ServiceName -ErrorAction SilentlyContinue) {
    Write-Host 'Stopping service (managed processes are restored on stop)...'
    Stop-Service $ServiceName -Force -ErrorAction SilentlyContinue
    (Get-Service $ServiceName).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(20))
    sc.exe delete $ServiceName | Out-Null
    Write-Host "Service '$ServiceName' removed."
} else {
    Write-Host "Service '$ServiceName' is not installed."
}

if ([System.Diagnostics.EventLog]::SourceExists($ServiceName)) {
    Remove-EventLog -Source $ServiceName
    Write-Host 'Event-log source removed.'
}

$InstallDir = Join-Path $env:ProgramFiles 'ThreadHaulDaemon'
if (Test-Path $InstallDir) {
    Remove-Item $InstallDir -Recurse -Force
    Write-Host "Removed $InstallDir"
}

$DataDir = Join-Path $env:ProgramData 'ThreadHaulDaemon'
if ($RemoveData -and (Test-Path $DataDir)) {
    Remove-Item $DataDir -Recurse -Force
    Write-Host "Removed $DataDir"
} elseif (Test-Path $DataDir) {
    Write-Host "Kept config and logs in $DataDir (use -RemoveData to delete)."
}
