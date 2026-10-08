<#
.SYNOPSIS
Starts owned Steam Elden Ring directly in the experimental offline bridge mode.
.DESCRIPTION
Checks the completed Steam installation, FileVersion 2.7.1.0, installed hashes and saved installation manifest.
The two tracked schema-2 ReShade INIs may change, but must exist as regular files without links.
All DLLs, add-ons, shaders and legacy entries retain strict installed-hash checks.
Validates and backs up the graphics config, then selects WINDOW while preserving supported
saved window dimensions. Width/Height can explicitly select up to 2560x1440.
Starts only eldenring.exe, with ERBRIDGE=1 and an absolute USERPROFILE/Documents/EldenMinecraft/ipc in the child environment.
Steam must already be running with the owning account. No credentials are read; ownership remains Steam's check.
Configure Steam/game for offline play before using this launcher. It does not create a firewall/network sandbox.
No start_protected_game, EAC service changes, global environment settings or Minecraft authentication bypass.
.PARAMETER ManifestPath
Optional manifest.json from installation. By default uses the newest active session in BackupDir for this game.
.EXAMPLE
.\Start-Bridge.ps1 -WhatIf
.EXAMPLE
.\Start-Bridge.ps1
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param([string]$GameDir, [string]$BackupDir, [string]$ManifestPath, [string[]]$SteamPath,
    [ValidateRange(640,2560)][int]$Width, [ValidateRange(480,1440)][int]$Height)
$displayArgs=@{}
foreach ($dimension in @('Width','Height')) { if ($PSBoundParameters.ContainsKey($dimension)) { $displayArgs[$dimension]=$PSBoundParameters[$dimension] } }
if ($displayArgs.Count -eq 1) { throw 'Specify both Width and Height, or omit both to preserve the saved window resolution.' }
. (Join-Path $PSScriptRoot 'Bridge.Common.ps1')
Assert-BridgeWindows
Assert-BridgeStopped
$game = Get-BridgeGame $GameDir $SteamPath
$backup = Get-BridgeBackupDir $BackupDir $game.GameDir
if ($ManifestPath) {
    $ManifestPath = Get-BridgeFullPath $ManifestPath
    $manifest = Read-BridgeManifest $ManifestPath $backup $game.GameDir
} else {
    $active = Find-BridgeManifest $backup $game.GameDir
    if (-not $active) { throw 'No install manifest found. Run Install-Bridge.ps1 first.' }
    $ManifestPath = $active.Path; $manifest = $active.Manifest
}
if ($manifest.Status -ne 'Installed') { throw 'Installation is not complete. Recover/remove it before launch.' }
Assert-BridgeInstalledFiles $manifest
$null = Get-BridgePE (Join-Path $game.GameDir 'dinput8.dll')
$null = Get-BridgePE (Join-Path $game.GameDir 'erbridge\erbridge_core.dll')
if ($manifest.SchemaVersion -eq 2) {
    $null = Get-BridgePE (Join-Path $game.GameDir 'dxgi.dll')
    $null = Get-BridgePE (Join-Path $game.GameDir 'ErmcDepth.addon64')
}
if ([IO.File]::ReadAllText((Join-Path $game.GameDir 'steam_appid.txt')).Trim() -ne '1245620') { throw 'steam_appid.txt must contain only 1245620.' }
if (-not @(Get-Process -Name steam -ErrorAction SilentlyContinue).Count) { throw 'Start Steam and sign in to the owning account before using the offline launcher.' }
if (-not $env:USERPROFILE -or -not [IO.Path]::IsPathRooted($env:USERPROFILE) -or $env:USERPROFILE -match '^[a-zA-Z]:[^\\/]') {
    throw 'USERPROFILE must be an absolute Windows directory.'
}
# AppData can be redirected into an MSIX launcher's LocalCache. Documents gives
# the game and Minecraft the same physical file when launched independently.
$ipc = Get-BridgeFullPath (Join-Path $env:USERPROFILE 'Documents\EldenMinecraft\ipc')
Assert-BridgeInside $ipc (Get-BridgeFullPath $env:USERPROFILE)
Assert-BridgeNoLinks $ipc
$exe = Join-Path $game.GameDir 'eldenring.exe'
$info = New-Object Diagnostics.ProcessStartInfo
$info.FileName = $exe
$info.WorkingDirectory = $game.GameDir
$info.UseShellExecute = $false
$info.EnvironmentVariables['ERBRIDGE'] = '1'
$info.EnvironmentVariables['ERBRIDGE_COOP'] = '0'
$info.EnvironmentVariables['ERMC_DIR'] = $ipc
$displayPlan = & (Join-Path $PSScriptRoot 'Set-BridgeDisplay.ps1') @displayArgs -ValidateOnly
$displayLabel=[string]$displayPlan.Width+'x'+$displayPlan.Height
if (-not $PSCmdlet.ShouldProcess($exe, "Set WINDOW $displayLabel with backup and launch for offline bridge use, ERBRIDGE=1, ERMC_DIR=$ipc")) { return }
Assert-BridgeStopped
$null = Get-BridgeGame $game.GameDir $SteamPath
Assert-BridgeInstalledFiles $manifest
$null = & (Join-Path $PSScriptRoot 'Set-BridgeDisplay.ps1') @displayArgs -Confirm:$false
Assert-BridgeNoLinks $ipc
$null = [IO.Directory]::CreateDirectory($ipc)
Assert-BridgeStopped
$process = [Diagnostics.Process]::Start($info)
try { [pscustomobject]@{ ProcessId = $process.Id; Executable = $exe; IpcDir = $ipc; Mode = 'Offline bridge'; RuntimeVerified = $false } }
finally { $process.Dispose() }
