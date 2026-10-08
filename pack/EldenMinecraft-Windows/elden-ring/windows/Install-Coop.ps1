<#
.SYNOPSIS
Adds the locally downloaded official Seamless Co-op 2.0.1 to an existing bridge installation.
.DESCRIPTION
Pins the author's GitHub asset SHA256, checks the exact archive layout and AMD64 images,
refuses every existing destination, stages all bytes and save backups before writing,
and rolls back only unchanged newly installed files on failure. No downloads or launches.
Keeps .co2 separate from vanilla .sl2. No saves are imported unless -ImportSl2 names one.
Selects the archive's official English locale to avoid a missing-language startup dialog.
The author prohibits redistribution: never include the archive/payload/password in a public package.
.PARAMETER PasswordFile
Local single-line 12-128 character session password (letters, digits, underscore, hyphen).
The password is never printed. Without this option a cryptographically random password is generated.
Seamless requires the password in its local INI; the backup session also contains that private INI.
.PARAMETER ImportSl2
Explicit ER0000.sl2 inside APPDATA/EldenRing/<numeric-account>. Copies to a NEW ER0000.co2
only after backup; refuses an existing .co2 or .co2.bak. Never changes the source or imports backwards.
.PARAMETER ValidateOnly
Read-only validation used by Start-Coop.ps1; does not need an archive or password.
.EXAMPLE
.\Install-Coop.ps1 -GameDir 'D:\SteamLibrary\steamapps\common\ELDEN RING\Game' -ArchivePath 'C:\Downloads\Seamless.Co-op.v2.0.1.zip' -PasswordFile 'C:\Private\coop-password.txt' -WhatIf
#>
[CmdletBinding(SupportsShouldProcess=$true, ConfirmImpact='Medium')]
param([string]$GameDir, [string]$ArchivePath, [string]$PasswordFile, [Alias('ImportSave')][string]$ImportSl2,
    [string]$BackupDir, [string]$BridgeBackupDir, [string]$BridgeManifestPath,
    [string]$ManifestPath, [string[]]$SteamPath, [switch]$ValidateOnly)
. (Join-Path $PSScriptRoot 'Bridge.Common.ps1')
$releaseHash = '848ae27e1c77217590dac401c012a17d9b4f05b9b9a24aa33af54d8d7b8ba2a1'
$paths = @('ersc_launcher.exe','SeamlessCoop\ersc.dll','SeamlessCoop\ersc_settings.ini',
    'SeamlessCoop\locale\english.json','SeamlessCoop\crashpad\crashpad_handler.exe')

function Get-CoopBytesHash([byte[]]$Bytes) {
    $h=[Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($h.ComputeHash($Bytes))).Replace('-','').ToLowerInvariant() }
    finally { $h.Dispose() }
}
function Assert-CoopImage([byte[]]$Bytes, [bool]$Dll) {
    if ($Bytes.Length -lt 64 -or [BitConverter]::ToUInt16($Bytes,0) -ne 0x5a4d) { throw 'Invalid Seamless MZ header.' }
    $p=[BitConverter]::ToInt32($Bytes,0x3c)
    if ($p -lt 64 -or $p -gt ($Bytes.Length-26)) { throw 'Invalid Seamless PE offset.' }
    if ([BitConverter]::ToUInt32($Bytes,$p) -ne 0x4550 -or [BitConverter]::ToUInt16($Bytes,$p+4) -ne 0x8664) { throw 'Seamless requires AMD64 PE images.' }
    $sections=[BitConverter]::ToUInt16($Bytes,$p+6); $size=[BitConverter]::ToUInt16($Bytes,$p+20)
    $flags=[BitConverter]::ToUInt16($Bytes,$p+22)
    if ($sections -lt 1 -or $size -lt 112 -or ($p+24+$size+40*$sections) -gt $Bytes.Length -or
        [BitConverter]::ToUInt16($Bytes,$p+24) -ne 0x20b -or ($flags -band 2) -eq 0 -or
        ((($flags -band 0x2000) -ne 0) -ne $Dll)) { throw 'Invalid Seamless PE32+ image type/headers.' }
}
function Assert-CoopSettings([string]$Text) {
    foreach ($pair in @(@('allow_invaders','0'), @('save_file_extension','co2'))) {
        $matches=[regex]::Matches($Text, ('(?m)^\s*'+$pair[0]+'\s*=\s*([^\r\n;]*)'))
        if ($matches.Count -ne 1 -or $matches[0].Groups[1].Value.Trim() -cne $pair[1]) { throw ('Required cooperative setting is invalid: '+$pair[0]) }
    }
    $passwords=[regex]::Matches($Text, '(?m)^\s*cooppassword\s*=\s*([^\r\n;]*)')
    if ($passwords.Count -ne 1 -or $passwords[0].Groups[1].Value.Trim() -cnotmatch '^[A-Za-z0-9_-]{12,128}$') { throw 'Co-op password must be one nonempty safe 12-128 character value (value withheld).' }
}
function Read-CoopManifest([string]$File, [string]$Root, [string]$Game) {
    Assert-BridgeInside $File $Root; Assert-BridgeNoLinks $File
    $m=[IO.File]::ReadAllText($File) | ConvertFrom-Json
    if ($m.SchemaVersion -ne 1 -or $m.Kind -cne 'ErmcSeamless' -or $m.Release -cne 'v2.0.1' -or
        $m.ArchiveSha256 -cne $releaseHash -or $m.GameDir -ne $Game -or
        $m.Status -notin @('Installing','Installed','Failed','RolledBack')) { throw 'Invalid/mismatched Seamless manifest.' }
    if (@($m.Entries).Count -ne $paths.Count) { throw 'Seamless manifest must contain exactly five owned files.' }
    for ($i=0; $i -lt $paths.Count; $i++) {
        $e=@($m.Entries | Where-Object { $_.RelativePath -ceq $paths[$i] })
        if ($e.Count -ne 1) { throw 'Unknown/missing/duplicate Seamless manifest path.' }
        if ($e[0].InstalledSha256 -cnotmatch '^[a-f0-9]{64}$' -or $e[0].BackupName -cne ('payload-'+$i+'.bin') -or
            $e[0].OriginalExists -isnot [bool] -or $e[0].OriginalExists -or $null -ne $e[0].OriginalSha256) { throw 'Invalid Seamless ownership/hash fields.' }
    }
    return $m
}
function Find-CoopManifest([string]$Root, [string]$Game) {
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) { return }
    $found=@()
    foreach ($dir in @(Get-ChildItem -LiteralPath $Root -Directory)) {
        Assert-BridgeNoLinks $dir.FullName
        $file=Join-Path $dir.FullName 'coop-manifest.json'
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { continue }
        Assert-BridgeNoLinks $file
        $header=[IO.File]::ReadAllText($file) | ConvertFrom-Json
        if ($header.GameDir -ne $Game) { continue }
        $m=Read-CoopManifest $file $Root $Game
        if ($m.Status -ne 'RolledBack') { $found += [pscustomobject]@{Path=$file; Manifest=$m} }
    }
    if ($found.Count -gt 1) { throw 'Multiple active Seamless sessions; select an explicit manifest.' }
    if ($found.Count) { return $found[0] }
}
function Get-CoopBridge([string]$Game, [string]$Root, [string]$File) {
    if ($File) { $File=Get-BridgeFullPath $File; $m=Read-BridgeManifest $File $Root $Game }
    else {
        $active=Find-BridgeManifest $Root $Game
        if (-not $active) { throw 'An installed bridge manifest is required.' }
        $File=$active.Path; $m=$active.Manifest
    }
    if ($m.Status -ne 'Installed') { throw 'Bridge installation is not complete.' }
    Assert-BridgeInstalledFiles $m
    if ([IO.File]::ReadAllText((Join-Path $Game 'steam_appid.txt')).Trim() -ne '1245620') { throw 'Unexpected bridge Steam AppID.' }
    return [pscustomobject]@{Path=$File; Manifest=$m}
}

Assert-BridgeWindows; Assert-BridgeStopped
if (@(Get-Process -Name ersc_launcher -ErrorAction SilentlyContinue).Count) { throw 'Seamless launcher is already running.' }
$game=Get-BridgeGame $GameDir $SteamPath
$bridgeRoot=Get-BridgeBackupDir $BridgeBackupDir $game.GameDir
$bridge=Get-CoopBridge $game.GameDir $bridgeRoot $BridgeManifestPath
if (-not $BackupDir) { $BackupDir=Join-Path $script:BridgePackageRoot 'coop-backups' }
$backup=Get-BridgeBackupDir $BackupDir $game.GameDir
if ($ValidateOnly) {
    if ($ManifestPath) { $ManifestPath=Get-BridgeFullPath $ManifestPath; $manifest=Read-CoopManifest $ManifestPath $backup $game.GameDir }
    else {
        $active=Find-CoopManifest $backup $game.GameDir
        if (-not $active) { throw 'No installed Seamless manifest found.' }
        $ManifestPath=$active.Path; $manifest=$active.Manifest
    }
    if ($manifest.Status -ne 'Installed') { throw 'Seamless installation is incomplete; recover it before launch.' }
    foreach ($e in $manifest.Entries) { Assert-BridgeFileState (Join-Path $game.GameDir $e.RelativePath) $e.InstalledSha256 }
    foreach ($index in @(0,1,4)) { $null=Get-BridgePE (Join-Path $game.GameDir $paths[$index]) ($index -eq 1) }
    Assert-CoopSettings ([IO.File]::ReadAllText((Join-Path $game.GameDir $paths[2])))
    [pscustomobject]@{GameDir=$game.GameDir; Launcher=(Join-Path $game.GameDir $paths[0]); ManifestPath=$ManifestPath;
        BridgeManifestPath=$bridge.Path; Release='v2.0.1'; SaveExtension='co2'; InvasionsAllowed=$false; PvpLockEnforced=$false}
    return
}
if ($ManifestPath) { throw '-ManifestPath is only used with -ValidateOnly.' }
if (Find-CoopManifest $backup $game.GameDir) { throw 'An active/recoverable Seamless install already exists; no files were replaced.' }
$ArchivePath=Get-BridgeFullPath $ArchivePath
if ((Get-BridgeHash $ArchivePath) -cne $releaseHash) { throw 'Official Seamless v2.0.1 archive SHA256 mismatch.' }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$payload=@{}; $zip=[IO.Compression.ZipFile]::OpenRead($ArchivePath)
try {
    foreach ($entry in $zip.Entries) {
        $name=$entry.FullName.Replace('/','\')
        if ($name -cin @('SeamlessCoop\','SeamlessCoop\locale\','SeamlessCoop\crashpad\')) { continue }
        if ($name -cnotin $paths -or $payload.ContainsKey($name) -or $entry.Length -gt 32MB) { throw 'Unexpected/duplicate/oversized path in official archive.' }
        $stream=$entry.Open(); $memory=New-Object IO.MemoryStream
        try { $stream.CopyTo($memory); $payload[$name]=$memory.ToArray() }
        finally { $memory.Dispose(); $stream.Dispose() }
    }
} finally { $zip.Dispose() }
if ($payload.Count -ne $paths.Count) { throw 'Official archive is missing required files.' }
foreach ($index in @(0,1,4)) { Assert-CoopImage $payload[$paths[$index]] ($index -eq 1) }
$password=$null
if ($PasswordFile) { $PasswordFile=Get-BridgeFullPath $PasswordFile; $passwordHash=Get-BridgeHash $PasswordFile; $password=[IO.File]::ReadAllText($PasswordFile).Trim() }
else {
    $random=New-Object byte[] 24; $rng=[Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($random); $password=([BitConverter]::ToString($random)).Replace('-','').ToLowerInvariant() }
    finally { $rng.Dispose() }
}
if ($password -cnotmatch '^[A-Za-z0-9_-]{12,128}$') { throw 'Invalid local password file (value withheld).' }
$settings=[Text.Encoding]::UTF8.GetString($payload[$paths[2]])
foreach ($key in @('allow_invaders','cooppassword','save_file_extension','mod_language_override')) {
    if ([regex]::Matches($settings, ('(?m)^\s*'+$key+'\s*=')).Count -ne 1) { throw 'Unexpected official settings template.' }
}
$settings=[regex]::Replace($settings,'(?m)^allow_invaders\s*=[^\r\n]*','allow_invaders = 0')
$settings=[regex]::Replace($settings,'(?m)^cooppassword\s*=[^\r\n]*',('cooppassword = '+$password))
$settings=[regex]::Replace($settings,'(?m)^save_file_extension\s*=[^\r\n]*','save_file_extension = co2')
$settings=[regex]::Replace($settings,'(?m)^mod_language_override\s*=[^\r\n]*','mod_language_override = english')
Assert-CoopSettings $settings
$payload[$paths[2]]=[Text.Encoding]::UTF8.GetBytes($settings)
$password=$null; $settings=$null
$entries=@()
for ($i=0; $i -lt $paths.Count; $i++) {
    $target=Join-Path $game.GameDir $paths[$i]; Assert-BridgeInside $target $game.GameDir
    if ($null -ne (Get-BridgeFileState $target)) { throw "Unowned Seamless destination exists; refusing to overwrite: $target" }
    $entries += [pscustomobject]@{RelativePath=$paths[$i]; InstalledSha256=(Get-CoopBytesHash $payload[$paths[$i]]);
        OriginalExists=$false; OriginalSha256=$null; BackupName=('payload-'+$i+'.bin')}
}
# Read only known save extensions; never account/login configuration.
if (-not $env:APPDATA) { throw 'APPDATA is missing.' }
$saveRoot=Get-BridgeFullPath (Join-Path $env:APPDATA 'EldenRing'); Assert-BridgeNoLinks $saveRoot
$saves=@(); $import=$null
if (Test-Path -LiteralPath $saveRoot -PathType Container) {
    foreach ($dir in @($saveRoot)+@(Get-ChildItem -LiteralPath $saveRoot -Directory | ForEach-Object { $_.FullName })) {
        Assert-BridgeNoLinks $dir
        foreach ($f in @(Get-ChildItem -LiteralPath $dir -File | Where-Object { $_.Name -match '\.(sl2|co2)(\.bak)?$' })) {
            $saves += [pscustomobject]@{Source=$f.FullName; RelativePath=$f.FullName.Substring($saveRoot.Length+1); Sha256=(Get-BridgeHash $f.FullName)}
        }
    }
}
if ($ImportSl2) {
    $ImportSl2=Get-BridgeFullPath $ImportSl2; Assert-BridgeInside $ImportSl2 $saveRoot
    $rel=$ImportSl2.Substring($saveRoot.Length+1)
    if ($rel -cnotmatch '^\d+\\ER0000\.sl2$') { throw 'ImportSl2 must name ER0000.sl2 in one numeric account folder.' }
    $sourceHash=Get-BridgeHash $ImportSl2; $co2=[IO.Path]::ChangeExtension($ImportSl2,'.co2')
    if ($null -ne (Get-BridgeFileState $co2) -or $null -ne (Get-BridgeFileState ($co2+'.bak'))) { throw 'Existing co2/co2.bak blocks save import; nothing will be overwritten.' }
    $import=[pscustomobject]@{Source=$ImportSl2; Target=$co2; Sha256=$sourceHash}
}
$session=Join-Path $backup ((Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssfffffffZ')+'-'+[guid]::NewGuid().ToString('N'))
Assert-BridgeInside $session $backup; Assert-BridgeNoLinks $session
$ManifestPath=Join-Path $session 'coop-manifest.json'
$manifest=[pscustomobject]@{SchemaVersion=1; Kind='ErmcSeamless'; Release='v2.0.1'; ArchiveSha256=$releaseHash;
    GameDir=$game.GameDir; BridgeManifestPath=$bridge.Path; CreatedUtc=[DateTime]::UtcNow.ToString('o');
    Status='Installing'; Entries=$entries; SaveBackups=@(); SaveImport=$import}
if (-not $PSCmdlet.ShouldProcess($game.GameDir,"Back up saves to $session and add five official Seamless files; separate .co2 saves")) { return }
Assert-BridgeStopped
$null=Get-BridgeGame $game.GameDir $SteamPath; $null=Get-CoopBridge $game.GameDir $bridgeRoot $bridge.Path
if ((Get-BridgeHash $ArchivePath) -cne $releaseHash) { throw 'Archive changed after preflight.' }
if ($PasswordFile) { Assert-BridgeFileState $PasswordFile $passwordHash }
foreach ($e in $entries) { Assert-BridgeFileState (Join-Path $game.GameDir $e.RelativePath) $null }
if ($import) { Assert-BridgeFileState $import.Source $import.Sha256; Assert-BridgeFileState $import.Target $null; Assert-BridgeFileState ($import.Target+'.bak') $null }
$null=[IO.Directory]::CreateDirectory($session)
$importCommitted=$false
try {
    foreach ($save in $saves) {
        $dest=Join-Path (Join-Path $session 'saves') $save.RelativePath
        Assert-BridgeInside $dest $session; Assert-BridgeNoLinks $dest
        $null=[IO.Directory]::CreateDirectory((Split-Path -Parent $dest))
        Assert-BridgeFileState $save.Source $save.Sha256
        [IO.File]::Copy($save.Source,$dest,$false); Assert-BridgeFileState $dest $save.Sha256
        $manifest.SaveBackups += [pscustomobject]@{RelativePath=$save.RelativePath; Sha256=$save.Sha256}
    }
    foreach ($e in $entries) {
        $dest=Join-Path $session $e.BackupName
        [IO.File]::WriteAllBytes($dest,$payload[$e.RelativePath]); Assert-BridgeFileState $dest $e.InstalledSha256
    }
    Save-BridgeManifest $manifest $ManifestPath $backup
    foreach ($e in $entries) { Assert-BridgeFileState (Join-Path $game.GameDir $e.RelativePath) $null }
    foreach ($e in $entries) { Write-BridgeCheckedFile (Join-Path $game.GameDir $e.RelativePath) (Join-Path $session $e.BackupName) $null $e.InstalledSha256 $game.GameDir }
    if ($import) {
        $source=Join-Path (Join-Path $session 'saves') $import.Source.Substring($saveRoot.Length+1)
        Assert-BridgeFileState $import.Source $import.Sha256; Assert-BridgeFileState ($import.Target+'.bak') $null
        Write-BridgeCheckedFile $import.Target $source $null $import.Sha256 $saveRoot
        $importCommitted=$true
    }
    $manifest.Status='Installed'; Save-BridgeManifest $manifest $ManifestPath $backup
} catch {
    $failure=$_
    if (Test-Path -LiteralPath $ManifestPath -PathType Leaf) {
        try {
            Assert-BridgeStopped
            $plan=@(Get-BridgeRestorePlan $manifest $ManifestPath)
            if ($import -and (Test-Path -LiteralPath $import.Target)) {
                if (-not $importCommitted) { throw 'An unconfirmed import target exists; preserve it for recovery.' }
                Assert-BridgeInside $import.Target $saveRoot; Assert-BridgeFileState $import.Target $import.Sha256
            }
            Invoke-BridgeRestore $plan $game.GameDir
            if ($import -and (Test-Path -LiteralPath $import.Target)) { Assert-BridgeStopped; Assert-BridgeFileState $import.Target $import.Sha256; [IO.File]::Delete($import.Target) }
            $manifest.Status='RolledBack'
        } catch { $manifest.Status='Failed'; Write-Warning "Rollback stopped safely; retain session $session for recovery." }
        Save-BridgeManifest $manifest $ManifestPath $backup
    }
    throw $failure
}
[pscustomobject]@{Status='Installed'; ManifestPath=$ManifestPath; Release='v2.0.1'; FilesInstalled=5;
    SettingsPath=(Join-Path $game.GameDir $paths[2]); SaveFilesBackedUp=$saves.Count; SaveImported=($null -ne $import);
    InvasionsAllowed=$false; PvpLockEnforced=$false; RuntimeVerified=$false}
