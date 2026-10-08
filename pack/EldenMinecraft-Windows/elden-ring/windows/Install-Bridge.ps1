<#
.SYNOPSIS
Installs the experimental native Windows bridge into a completed, owned Steam Elden Ring installation.
.DESCRIPTION
Preflights FileVersion 2.7.1.0, AMD64 PE files, SHA256, game process state and all target/backup paths.
Backs up saves and existing owned files into a timestamped package-local session, then installs tracked files.
The Steam manifest identifies an installation, not a licence: Steam performs ownership verification at launch.
Use -OwnedSteamGame only for a game you own. No build, launch or anti-cheat service operation occurs here.
.PARAMETER BinaryDir
Directory inside the package containing dinput8.dll and erbridge_core.dll. Default: er-bridge/build-msvc/Release.
.PARAMETER BackupDir
Directory strictly inside the package root, outside the game folder. Default: package-root/bridge-backups.
.PARAMETER ReShadeDir
Optional local prepared directory with dxgi.dll and ErmcDepth.addon64 (AMD64), ReShade.ini,
ErmcDepthPreset.ini and ermc-shaders/ErmcDepth.fx. Copies only these five files, refusing existing destinations even with identical bytes.
All source hashes are recorded and rechecked. Nothing is downloaded or bundled by this script.
.PARAMETER ReShadeSha256
Optional expected SHA256 for the locally supplied dxgi.dll. File hashes alone do not authenticate its publisher.
.PARAMETER SteamPath
Optional Steam roots/libraries for discovery; replaces registry/default-path discovery when supplied.
.PARAMETER OwnedSteamGame
Acknowledges that this is your owned Steam copy; required before creating steam_appid.txt (1245620).
.EXAMPLE
.\Install-Bridge.ps1 -OwnedSteamGame -WhatIf
.EXAMPLE
.\Install-Bridge.ps1 -OwnedSteamGame -BinaryDir ..\er-bridge\build-windows\Release
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
    [string]$GameDir,
    [string]$BinaryDir,
    [string]$BackupDir,
    [string]$ReShadeDir,
    [string[]]$SteamPath,
    [switch]$OwnedSteamGame,
    [ValidatePattern('^[a-fA-F0-9]{64}$')][string]$ProxySha256,
    [ValidatePattern('^[a-fA-F0-9]{64}$')][string]$CoreSha256,
    [ValidatePattern('^[a-fA-F0-9]{64}$')][string]$ReShadeSha256
)
. (Join-Path $PSScriptRoot 'Bridge.Common.ps1')
Assert-BridgeWindows
Assert-BridgeStopped
$game = Get-BridgeGame $GameDir $SteamPath
if (-not $OwnedSteamGame) { throw 'Use -OwnedSteamGame only for your owned Steam copy. A manifest alone does not prove ownership.' }
$binary = Get-BridgeBinaryDir $BinaryDir
$backup = Get-BridgeBackupDir $BackupDir $game.GameDir
if (Test-BridgeInside $binary $game.GameDir) { throw 'BinaryDir must be outside the game directory.' }
$proxy = Get-BridgePE (Join-Path $binary 'dinput8.dll')
$core = Get-BridgePE (Join-Path $binary 'erbridge_core.dll')
if ($ProxySha256 -and $proxy.Sha256 -ne $ProxySha256.ToLowerInvariant()) { throw 'Package dinput8.dll SHA256 mismatch.' }
if ($CoreSha256 -and $core.Sha256 -ne $CoreSha256.ToLowerInvariant()) { throw 'Package erbridge_core.dll SHA256 mismatch.' }
$active = Find-BridgeManifest $backup $game.GameDir
if ($active) { throw "An active/recoverable install exists. Remove it before installing again: $($active.Path)" }
$appidBytes = [Text.Encoding]::ASCII.GetBytes("1245620`r`n")
$sha = [Security.Cryptography.SHA256]::Create()
try { $appidHash = ([BitConverter]::ToString($sha.ComputeHash($appidBytes))).Replace('-', '').ToLowerInvariant() }
finally { $sha.Dispose() }
$specs = @(
    @{ Name = 'dinput8.dll'; Source = $proxy.Path; Hash = $proxy.Sha256 },
    @{ Name = 'erbridge\erbridge_core.dll'; Source = $core.Path; Hash = $core.Sha256 },
    @{ Name = 'steam_appid.txt'; Source = $null; Hash = $appidHash }
)
if ($ReShadeSha256 -and -not $ReShadeDir) { throw 'ReShadeSha256 requires -ReShadeDir.' }
if ($ReShadeDir) {
    $auxiliary = @(Get-BridgeReShadeSpecs $ReShadeDir $game.GameDir)
    if ($ReShadeSha256 -and $auxiliary[0].Hash -ne $ReShadeSha256.ToLowerInvariant()) { throw 'Local ReShade dxgi.dll SHA256 mismatch.' }
    $specs += $auxiliary
}
$entries = @()
for ($i = 0; $i -lt $specs.Count; $i++) {
    $spec = $specs[$i]
    $target = Join-Path $game.GameDir $spec.Name
    Assert-BridgeInside $target $game.GameDir
    $oldHash = Get-BridgeFileState $target
    if ($i -ge 3 -and $null -ne $oldHash) { throw "An unowned ReShade destination already exists; refusing to replace it: $target" }
    if ($i -lt 2 -and $oldHash -and $oldHash -ne $spec.Hash) {
        throw "An unrecognized/non-bridge DLL already exists; refusing to replace it: $target"
    }
    $entries += [pscustomobject]@{
        RelativePath = $spec.Name; InstalledSha256 = $spec.Hash
        OriginalExists = ($null -ne $oldHash); OriginalSha256 = $oldHash
        BackupName = ('original-' + $i + '.bin')
    }
}
$saves = @(Get-BridgeSaveFiles)
$session = Join-Path $backup ((Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssfffffffZ') + '-' + [guid]::NewGuid().ToString('N'))
Assert-BridgeInside $session $backup
Assert-BridgeNoLinks $session
if (Test-Path -LiteralPath $session) { throw 'Backup session already exists.' }
$manifestPath = Join-Path $session 'manifest.json'
$manifest = [pscustomobject]@{
    SchemaVersion = 1; AppId = '1245620'; FileVersion = '2.7.1.0'; GameDir = $game.GameDir
    SteamManifest = $game.ManifestPath; CreatedUtc = [DateTime]::UtcNow.ToString('o')
    Status = 'Installing'; Entries = $entries; SaveBackups = @()
}
if ($ReShadeDir) { $manifest.SchemaVersion = 2 }
if (-not $PSCmdlet.ShouldProcess($game.GameDir, "Back up saves/files to $session and install the offline bridge")) { return }
Assert-BridgeStopped
# Recheck the complete preflight before creating anything, including the executable and Steam state.
$null = Get-BridgeGame $game.GameDir $SteamPath
Assert-BridgeNoLinks $backup
foreach ($entry in $entries) { Assert-BridgeFileState (Join-Path $game.GameDir $entry.RelativePath) $entry.OriginalSha256 }
foreach ($spec in $specs) { if ($spec.Source -and (Get-BridgeHash $spec.Source) -ne $spec.Hash) { throw "Source changed after validation: $($spec.Source)" } }
foreach ($save in $saves) { Assert-BridgeFileState $save.Source $save.Sha256 }
$null = [IO.Directory]::CreateDirectory($session)
try {
    # Finish and verify every backup/staged file before the first game-directory write.
    foreach ($entry in $entries) {
        if ($entry.OriginalExists) {
            $source = Join-Path $game.GameDir $entry.RelativePath
            $dest = Join-Path $session $entry.BackupName
            Assert-BridgeFileState $source $entry.OriginalSha256
            [IO.File]::Copy($source, $dest, $false)
            if ((Get-BridgeHash $dest) -ne $entry.OriginalSha256) { throw "Original backup verification failed: $dest" }
        }
    }
    $saveRoot = Join-Path $session 'saves'
    foreach ($save in $saves) {
        $dest = Join-Path $saveRoot $save.RelativePath
        Assert-BridgeInside $dest $saveRoot
        Assert-BridgeNoLinks $dest
        $null = [IO.Directory]::CreateDirectory((Split-Path -Parent $dest))
        Assert-BridgeFileState $save.Source $save.Sha256
        [IO.File]::Copy($save.Source, $dest, $false)
        if ((Get-BridgeHash $dest) -ne $save.Sha256) { throw 'Save backup verification failed.' }
        $manifest.SaveBackups += [pscustomobject]@{ RelativePath = $save.RelativePath; Sha256 = $save.Sha256 }
    }
    $staged = @()
    for ($i = 0; $i -lt $specs.Count; $i++) {
        $dest = Join-Path $session ('install-' + $i + '.bin')
        if ($specs[$i].Source) { [IO.File]::Copy($specs[$i].Source, $dest, $false) }
        else { [IO.File]::WriteAllBytes($dest, $appidBytes) }
        if ((Get-BridgeHash $dest) -ne $specs[$i].Hash) { throw 'Staged package verification failed.' }
        $staged += $dest
    }
    Save-BridgeManifest $manifest $manifestPath $backup
    Assert-BridgeStopped
    $null = Get-BridgeGame $game.GameDir $SteamPath
    foreach ($entry in $entries) { Assert-BridgeFileState (Join-Path $game.GameDir $entry.RelativePath) $entry.OriginalSha256 }
    for ($i = 0; $i -lt $entries.Count; $i++) {
        $entry = $entries[$i]
        Write-BridgeCheckedFile (Join-Path $game.GameDir $entry.RelativePath) $staged[$i] $entry.OriginalSha256 $entry.InstalledSha256 $game.GameDir
    }
    $manifest.Status = 'Installed'
    Save-BridgeManifest $manifest $manifestPath $backup
} catch {
    $failure = $_
    if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
        try {
            Assert-BridgeStopped
            $rollback = @(Get-BridgeRestorePlan $manifest $manifestPath)
            Invoke-BridgeRestore $rollback $game.GameDir
            $manifest.Status = 'RolledBack'
        } catch {
            $manifest.Status = 'Failed'
            Write-Warning "Automatic rollback could not safely complete: $($_.Exception.Message). Keep $manifestPath for recovery."
        }
        Save-BridgeManifest $manifest $manifestPath $backup
    }
    throw $failure
}
[pscustomobject]@{ Status = 'Installed'; GameDir = $game.GameDir; ManifestPath = $manifestPath; SaveFilesBackedUp = $saves.Count; RuntimeVerified = $false }
