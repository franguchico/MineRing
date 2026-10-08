<#
.SYNOPSIS
Starts the official Seamless launcher with the installed Minecraft bridge environment.
.DESCRIPTION
Validates both independent manifests, exact installed files and cooperative settings twice.
Backs up existing graphics settings and selects WINDOW while preserving supported saved
window dimensions (up to 2560x1440). Width/Height can explicitly choose dimensions.
On a first launch without GraphicsConfig.xml, lets the game generate its own configuration;
the selected window mode applies on the next launch.
Steam must already be running and signed in online for Seamless matchmaking.
Does not invoke the protected launcher, alter security settings, modify saves, or start Java.
Seamless's own supported launcher handles its game mode. Child-game environment inheritance
and compatibility with the Minecraft bridge still need live verification.
.EXAMPLE
.\Start-Coop.ps1 -GameDir 'D:\SteamLibrary\steamapps\common\ELDEN RING\Game' -WhatIf
#>
[CmdletBinding(SupportsShouldProcess=$true, ConfirmImpact='Medium')]
param([string]$GameDir, [string]$BackupDir, [string]$BridgeBackupDir,
    [string]$BridgeManifestPath, [string]$ManifestPath, [string[]]$SteamPath,
    [ValidateRange(640,2560)][int]$Width, [ValidateRange(480,1440)][int]$Height)
$displayArgs=@{}
foreach ($dimension in @('Width','Height')) { if ($PSBoundParameters.ContainsKey($dimension)) { $displayArgs[$dimension]=$PSBoundParameters[$dimension] } }
if ($displayArgs.Count -eq 1) { throw 'Specify both Width and Height, or omit both to preserve the saved window resolution.' }
. (Join-Path $PSScriptRoot 'Bridge.Common.ps1')
$validate=@{ValidateOnly=$true; GameDir=$GameDir; BackupDir=$BackupDir; BridgeBackupDir=$BridgeBackupDir;
    BridgeManifestPath=$BridgeManifestPath; ManifestPath=$ManifestPath; SteamPath=$SteamPath}
$state=& (Join-Path $PSScriptRoot 'Install-Coop.ps1') @validate
if (-not @(Get-Process -Name steam -ErrorAction SilentlyContinue).Count) { throw 'Start Steam and sign in online before Seamless co-op.' }
if (-not $env:USERPROFILE -or -not [IO.Path]::IsPathRooted($env:USERPROFILE) -or $env:USERPROFILE -match '^[a-zA-Z]:[^\\/]') { throw 'USERPROFILE must be an absolute Windows directory.' }
$ipc=Get-BridgeFullPath (Join-Path $env:USERPROFILE 'Documents\EldenMinecraft\ipc')
Assert-BridgeInside $ipc (Get-BridgeFullPath $env:USERPROFILE); Assert-BridgeNoLinks $ipc
$info=New-Object Diagnostics.ProcessStartInfo
$info.FileName=$state.Launcher; $info.WorkingDirectory=$state.GameDir; $info.UseShellExecute=$false
$info.EnvironmentVariables['ERBRIDGE']='1'; $info.EnvironmentVariables['ERMC_DIR']=$ipc
$info.EnvironmentVariables['ERBRIDGE_COOP']='1'
if (-not $env:APPDATA -or -not [IO.Path]::IsPathRooted($env:APPDATA) -or $env:APPDATA -match '^[a-zA-Z]:[^\\/]') { throw 'APPDATA must be an absolute Windows directory.' }
$graphicsRoot=Get-BridgeFullPath (Join-Path $env:APPDATA 'EldenRing')
$graphicsPath=Join-Path $graphicsRoot 'GraphicsConfig.xml'
Assert-BridgeInside $graphicsPath $graphicsRoot; Assert-BridgeNoLinks $graphicsPath
$hadGraphicsConfig=$null -ne (Get-BridgeFileState $graphicsPath)
if ($hadGraphicsConfig) {
    $displayPlan=& (Join-Path $PSScriptRoot 'Set-BridgeDisplay.ps1') @displayArgs -ValidateOnly
    $displayLabel=[string]$displayPlan.Width+'x'+$displayPlan.Height
    $launchAction="Set WINDOW $displayLabel with backup and launch official Seamless co-op with the Minecraft bridge environment"
} else {
    Write-Host 'GraphicsConfig.xml ainda nao existe. O jogo vai gerar o arquivo nesta primeira abertura; no proximo inicio sera aplicado WINDOW com a resolucao salva suportada (ou a dimensao explicita).'
    $launchAction='Launch official Seamless co-op so the game can create its first graphics configuration'
}
if (-not $PSCmdlet.ShouldProcess($state.Launcher,$launchAction)) { return }
$null=& (Join-Path $PSScriptRoot 'Install-Coop.ps1') @validate
Assert-BridgeNoLinks $ipc; $null=[IO.Directory]::CreateDirectory($ipc)
Assert-BridgeStopped
# Recheck immediately before launch: a new config is validated/backed up, but an
# existing config disappearing must never be mistaken for a first run.
Assert-BridgeNoLinks $graphicsPath
if ($null -ne (Get-BridgeFileState $graphicsPath)) {
    $null=& (Join-Path $PSScriptRoot 'Set-BridgeDisplay.ps1') @displayArgs -Confirm:$false
} elseif ($hadGraphicsConfig) {
    throw 'GraphicsConfig.xml disappeared after validation; refusing to launch with unverified display settings.'
}
$process=[Diagnostics.Process]::Start($info)
try { [pscustomobject]@{ProcessId=$process.Id; Executable=$info.FileName; IpcDir=$ipc;
    Mode='Seamless cooperative bridge'; SaveExtension='co2'; RuntimeVerified=$false; PvpLockEnforced=$false} }
finally { $process.Dispose() }
