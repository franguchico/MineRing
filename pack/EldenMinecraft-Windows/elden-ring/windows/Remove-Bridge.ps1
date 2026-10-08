<#
.SYNOPSIS
Restores the exact pre-install files from a hash-checked Windows bridge backup manifest.
.DESCRIPTION
Preflights every target and original backup before writing anything. Modified DLLs/add-ons/shaders block removal.
Schema-2 ReShade.ini and ErmcDepthPreset.ini changes are hash-verified and preserved in the install session
before those exact tracked files are removed. Other files, saves and directories are retained.
Removal also works after a Steam game update: it does not require the old executable FileVersion or a Steam manifest.
An installed/recoverable manifest for the exact GameDir is required. No recursive deletion or anti-cheat operation.
.EXAMPLE
.\Remove-Bridge.ps1 -GameDir 'D:\SteamLibrary\steamapps\common\ELDEN RING\Game' -WhatIf
.EXAMPLE
.\Remove-Bridge.ps1 -ManifestPath '..\..\bridge-backups\SESSION\manifest.json'
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param([string]$GameDir, [string]$BackupDir, [string]$ManifestPath, [string[]]$SteamPath)
. (Join-Path $PSScriptRoot 'Bridge.Common.ps1')
Assert-BridgeWindows
Assert-BridgeStopped
if (-not $GameDir -and $ManifestPath) {
    $backup = Get-BridgeBackupDir $BackupDir ''
    $ManifestPath = Get-BridgeFullPath $ManifestPath
    Assert-BridgeInside $ManifestPath $backup
    Assert-BridgeNoLinks $ManifestPath
    $header = [IO.File]::ReadAllText($ManifestPath) | ConvertFrom-Json
    $GameDir = [string]$header.GameDir
}
if (-not $GameDir) {
    $installs = @(Get-BridgeInstallations $SteamPath)
    if ($installs.Count -ne 1) { throw 'Supply -GameDir or -ManifestPath to select the installation to remove.' }
    $GameDir = $installs[0].GameDir
}
$GameDir = Get-BridgeFullPath $GameDir
Assert-BridgeNoLinks $GameDir
if (-not (Test-Path -LiteralPath $GameDir -PathType Container) -or -not (Test-Path -LiteralPath (Join-Path $GameDir 'eldenring.exe') -PathType Leaf)) {
    throw 'GameDir must be an existing Elden Ring directory containing eldenring.exe.'
}
$backup = Get-BridgeBackupDir $BackupDir $GameDir
if ($ManifestPath) {
    $ManifestPath = Get-BridgeFullPath $ManifestPath
    $manifest = Read-BridgeManifest $ManifestPath $backup $GameDir
} else {
    $active = Find-BridgeManifest $backup $GameDir
    if (-not $active) { throw 'No active install/recovery manifest found for this GameDir.' }
    $ManifestPath = $active.Path; $manifest = $active.Manifest
}
if ($manifest.Status -in @('Removed','RolledBack')) {
    [pscustomobject]@{ Status = $manifest.Status; ManifestPath = $ManifestPath }
    return
}
$plan = @(Get-BridgeRestorePlan $manifest $ManifestPath -PreserveGeneratedConfigs)
if (-not $PSCmdlet.ShouldProcess($GameDir, "Preserve changed tracked ReShade configs, then restore/delete $($plan.Count) owned files using $ManifestPath")) { return }
Assert-BridgeStopped
$plan = @(Get-BridgeRestorePlan $manifest $ManifestPath -PreserveGeneratedConfigs)
Invoke-BridgeRestore $plan $GameDir
$manifest.Status = 'Removed'
Save-BridgeManifest $manifest $ManifestPath $backup
[pscustomobject]@{ Status = 'Removed'; ManifestPath = $ManifestPath; SaveBackupsRetained = $true; PreservedConfigs = @($plan | Where-Object { $_.PreservePath } | ForEach-Object { $_.PreservePath }) }
