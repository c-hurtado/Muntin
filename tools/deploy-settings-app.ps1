<#
.SYNOPSIS
    Publishes Muntin.Settings.exe (the WinUI settings window, framework-dependent: needs the .NET 9 Runtime and
    Windows App Runtime 2.x >= 2.2) for win-x64 and copies it to
    %LOCALAPPDATA%\Muntin\Settings, where the Muntin tray's "Settings..." item looks first.

.PARAMETER Destination
    Where the app goes. Default: %LOCALAPPDATA%\Muntin\Settings.

.EXAMPLE
    .\tools\deploy-settings-app.ps1
    .\tools\deploy-settings-app.ps1 -Destination $env:TEMP\MuntinSettingsTest
#>
[CmdletBinding()]
param(
    [string]$Destination = (Join-Path $env:LOCALAPPDATA 'Muntin\Settings')
)

$ErrorActionPreference = 'Stop'

# robocopy /MIR deletes whatever else is in the destination: only ever mirror into a folder that holds this app or nothing
$Destination = [IO.Path]::GetFullPath($Destination).TrimEnd('\')
$muntinFolder = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'Muntin')).TrimEnd('\')
if ($Destination -ieq $muntinFolder) {
    throw "Refusing to deploy into $muntinFolder itself: /MIR would delete Muntin.exe and settings.json. Use its Settings subfolder."
}
if ((Test-Path $Destination) -and (Get-ChildItem -LiteralPath $Destination -Force | Select-Object -First 1) -and
    -not (Test-Path (Join-Path $Destination 'Muntin.Settings.exe'))) {
    throw "Refusing to deploy into $Destination`: it is not empty and holds no Muntin.Settings.exe."
}
$project = Join-Path $PSScriptRoot '..\src\modules\fancyzones\MuntinSettings\Muntin.Settings\Muntin.Settings.csproj'
$project = (Resolve-Path $project).Path
$staging = Join-Path ([IO.Path]::GetTempPath()) ("MuntinSettingsPublish-" + [Guid]::NewGuid().ToString('N'))

try {
    Write-Host "Publishing $project"
    & dotnet publish $project -c Release -r win-x64 -o $staging --nologo
    if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed ($LASTEXITCODE)" }
    if (-not (Test-Path (Join-Path $staging 'Muntin.Settings.exe'))) { throw 'Muntin.Settings.exe is missing from the publish output' }

    # A running settings window keeps its files locked
    $running = Get-Process -Name 'Muntin.Settings' -ErrorAction SilentlyContinue
    if ($running) {
        Write-Host 'Closing the running Muntin.Settings'
        $running | ForEach-Object { $_.CloseMainWindow() | Out-Null }
        $running | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue
        Get-Process -Name 'Muntin.Settings' -ErrorAction SilentlyContinue | Stop-Process -Force
    }

    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    # /MIR: files a previous version left behind are removed
    & robocopy $staging $Destination /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
    $global:LASTEXITCODE = 0 # robocopy's 1..7 mean success

    $size = (Get-ChildItem $Destination -Recurse -File | Measure-Object Length -Sum).Sum
    Write-Host ("Muntin.Settings deployed to {0} ({1:N1} MB)" -f $Destination, ($size / 1MB))
}
finally {
    if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
}
