param(
    [string]$Bin = (Join-Path $PSScriptRoot "..\bin\Release")
)
$ErrorActionPreference = 'Stop'
Write-Host "MCST 1.108 Release startup diagnostic"
Write-Host "Bin: $Bin"
$probe = Join-Path $Bin 'MCST-StartupProbe.exe'
$watchdog = Join-Path $Bin 'MCST-Watchdog.exe'
if (!(Test-Path $probe)) { throw "Missing $probe. Rebuild Solution / Release x64 first." }
if (!(Test-Path $watchdog)) { throw "Missing $watchdog. Rebuild Solution / Release x64 first." }
Write-Host "Starting minimal probe..."
$p = Start-Process -FilePath $probe -PassThru
$p.WaitForExit()
Write-Host "Probe exit code: $($p.ExitCode)"
Write-Host "Probe marker: C:\Temp\MCST-StartupProbe.log"
Write-Host "Now start MCST-Watchdog.exe manually. If it exits, inspect:"
Write-Host "  $Bin\MCST-Watchdog-Startup.log"
Write-Host "  C:\Temp\MCST-Watchdog-Startup.log"
