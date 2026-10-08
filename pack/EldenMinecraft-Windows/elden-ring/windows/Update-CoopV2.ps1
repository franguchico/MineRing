<#
Updates the tracked native core, every known bridge profile (including SOLO), and the
original package's launch/display scripts. Checked backups and a transaction journal
remain in bridge-backups. Accounts, saves and settings contents are never read.
Explicit InstancesDirectory roots replace automatic discovery. Otherwise inspect only
APPDATA/PrismLauncher/instances and a sibling portable PrismLauncher/instances.
Release metadata must describe the current frame layout; do not use an older JSON.
#>
[CmdletBinding(SupportsShouldProcess=$true)]
param([string]$InstalledPackage, [string]$GameDir, [string[]]$InstancesDirectory, [string[]]$SteamPath)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Bridge.Common.ps1')
$sourcePackage=$script:BridgePackageRoot

function Assert-CoopGamesStopped {
    Assert-BridgeStopped
    # So bloqueia Java do Minecraft/Prism (mesmo criterio do launcher); outros Java (Gradle, IDE...) nao atrapalham.
    $mcJava = @(Get-CimInstance Win32_Process -Filter "Name='java.exe' OR Name='javaw.exe'" -ErrorAction SilentlyContinue | Where-Object {
        $c = [string]$_.CommandLine
        $c -match '(?i)net\.minecraft|prismlauncher|-Derbridge|--gameDir'
    })
    if (@(Get-Process -Name ersc_launcher -ErrorAction SilentlyContinue).Count -or $mcJava.Count) {
        throw 'Feche Elden Ring, Seamless e Minecraft antes de atualizar.'
    }
}
function Assert-UpdateInstalledFiles($Manifest) {
    foreach ($entry in $Manifest.Entries) {
        $target=Join-Path $Manifest.GameDir $entry.RelativePath
        Assert-BridgeInside $target $Manifest.GameDir
        # Inspect existence/link metadata only for settings; never hash/read their contents.
        if ($entry.RelativePath -cin @('ReShade.ini','ErmcDepthPreset.ini')) {
            Assert-BridgeNoLinks $target
            if (-not (Test-Path -LiteralPath $target -PathType Leaf)) { throw "Tracked config is missing: $target" }
        } else { Assert-BridgeFileState $target $entry.InstalledSha256 }
    }
}

Assert-CoopGamesStopped
$releasePath=Join-Path $PSScriptRoot 'coop-v2-release.json'
Assert-BridgeNoLinks $releasePath
$release=[IO.File]::ReadAllText($releasePath) | ConvertFrom-Json
foreach ($field in @('Protocol','FrameProtocol','JarName','CoreSha256','JarSha256','ProxySha256','AddonSha256')) {
    if (-not $release.PSObject.Properties[$field]) { throw "Manifesto windows.6 incompleto: $field" }
}
if ($release.Protocol -ne 4 -or $release.FrameProtocol -ne 3 -or
    $release.CoreSha256 -isnot [string] -or $release.CoreSha256 -cnotmatch '^[a-f0-9]{64}$' -or
    $release.JarSha256 -isnot [string] -or $release.JarSha256 -cnotmatch '^[a-f0-9]{64}$' -or
    $release.ProxySha256 -isnot [string] -or $release.ProxySha256 -cnotmatch '^[a-f0-9]{64}$' -or
    $release.AddonSha256 -isnot [string] -or $release.AddonSha256 -cnotmatch '^[a-f0-9]{64}$') {
    throw 'Manifesto windows.6 invalido (Protocol=4, FrameProtocol=3 e SHA256 obrigatorios).'
}
if ($release.JarName -isnot [string] -or $release.JarName.Length -gt 128 -or
    $release.JarName -cnotmatch '\Aer-bridge-[0-9][a-zA-Z0-9._-]*\.jar\z' -or
    $release.JarName.Contains('..') -or [IO.Path]::GetFileName($release.JarName) -cne $release.JarName) {
    throw 'JarName deve ser um unico nome de arquivo bridge JAR seguro.'
}
$binarySource=Join-Path $sourcePackage 'elden-ring\er-bridge\build-msvc\Release'
$nativeSource=Join-Path $binarySource 'erbridge_core.dll'
$jarSource=Join-Path (Join-Path $sourcePackage 'elden-ring\mc-bridge\build\libs') $release.JarName
Assert-BridgeInside $nativeSource $sourcePackage
Assert-BridgeInside $jarSource $sourcePackage
if ((Get-BridgePE $nativeSource).Sha256 -cne $release.CoreSha256 -or
    (Get-BridgeHash $jarSource) -cne $release.JarSha256) { throw 'Os binarios windows.6 nao passaram na verificacao.' }

if (-not $InstalledPackage) {
    $InstalledPackage=Read-Host 'Cole o caminho da pasta EldenMinecraft-Windows da sua instalacao ANTERIOR (a que contem bridge-backups)'
    $InstalledPackage=$InstalledPackage.Trim().Trim('"')
}
$installed=Get-BridgeFullPath $InstalledPackage
Assert-BridgeNoLinks $installed
if (-not (Test-Path -LiteralPath (Join-Path $installed 'bridge-backups') -PathType Container)) {
    throw 'Esta nao e a pasta usada na instalacao anterior: bridge-backups nao encontrado.'
}
$game=Get-BridgeGame $GameDir $SteamPath
if ($installed -eq $game.GameDir -or (Test-BridgeInside $installed $game.GameDir) -or
    (Test-BridgeInside $game.GameDir $installed)) { throw 'Pasta de instalacao deve ficar fora do jogo.' }
# Backup validation belongs to the original installation, not the new release folder.
$script:BridgePackageRoot=$installed
$backupRoot=Get-BridgeBackupDir (Join-Path $installed 'bridge-backups') $game.GameDir
$active=Find-BridgeManifest $backupRoot $game.GameDir
if (-not $active -or $active.Manifest.Status -cne 'Installed') { throw 'Instalacao anterior do bridge nao encontrada ou incompleta.' }
$manifestBeforeHash=Get-BridgeHash $active.Path
Assert-UpdateInstalledFiles $active.Manifest

$knownCores=@(
    'd3f3cb4a716cfd39ad1e57d1d18c77931886130218c388d472e3c0dd3d495ae1',
    '5076fc795e7d03ec1327667af9f355f738fa5f4cb06fa69f6e53c93bd179aa63',
    'e4c76be64b3e19961514952f27aac13001bef10894a397330e7229e3c26e8ae0',
    'e764797158427f916d4c4bac4338d8fbb8f75281a09190edfa6969ccb7ab85b9',
    '8ec5d435fb4c9d4c5137f7c7cb714c947d60596f1bd9ed59191547c3b5553f5d',
    '9dbdf32a099f6b986f15c74c385289df215c8f31686ce0bb76384f9945dd9221',
    'e4712057b4f4901361dbb3ccc3de7b96b08fce7732db458da15ec152cc674f29',
    $release.CoreSha256
)
$knownJars=@(
    'eec92d838f77818732e6d425bbf6215fa3a64fd85938775019259ed83fb36c4f',
    '23e2e854b85ad9ae8225fa5c03d790b2c3ff392a99d05c740da43e60381268fa',
    '33ecd9f03695a60287621aaba43b3c951fe7512ace06492517a854ba67ea7832',
    '83e9ea95a2783f31a3a5286520bc638cdc0d25595405bca045a77fa1dbf42de8',
    '7fb5b7eb62e188c82afe2bf17a970a86cb1a96f890d8fec1fc9aab5e34f234a8',
    '87a12d1f4cf9561fc1bd6854ba2cb1fd1700ce50b4476ec204a1d62605c2e463',
    '730e075e18078d663cc968bbb48f9c74651c1cd12a4a7234ecce6a3d9401cfc9',
    $release.JarSha256
)
$coreEntry=@($active.Manifest.Entries | Where-Object RelativePath -ceq 'erbridge\erbridge_core.dll')
if ($coreEntry.Count -ne 1 -or $coreEntry[0].InstalledSha256 -cnotin $knownCores) {
    throw 'Versao nativa instalada desconhecida; nenhuma substituicao.'
}
$originalCore=$coreEntry[0].InstalledSha256
# The two reviewed proxy builds are compatible (PE timestamp differs). Keep the
# installed build and its manifest hash; refuse every other source/installed proxy.
$knownProxies=@('5d6cd7a35b665296cc08506510df4f4a9fe53142388dca1b7c6cf8305694ff24','14e1eab0323f051c00d20aaa46477af181bde9fe6c6649cab57619bc5ff020cb')
$proxyEntry=@($active.Manifest.Entries | Where-Object RelativePath -ceq 'dinput8.dll')
$proxySource=Join-Path $binarySource 'dinput8.dll'
if ($proxyEntry.Count -ne 1 -or $proxyEntry[0].InstalledSha256 -cnotin $knownProxies -or
    $release.ProxySha256 -cnotin $knownProxies -or (Get-BridgePE $proxySource).Sha256 -cne $release.ProxySha256) {
    throw 'Proxy source/installed hash is not a reviewed compatible build.'
}
$knownAddon='91cd77482e97c867501d2a6e66b035bcc7b6266b662894c8fc6a696b73bfc4d4'
$addonEntry=@($active.Manifest.Entries | Where-Object RelativePath -ceq 'ErmcDepth.addon64')
$addonSource=Join-Path $binarySource 'ErmcDepth.addon64'
if ($release.AddonSha256 -cne $knownAddon -or (Get-BridgePE $addonSource).Sha256 -cne $knownAddon -or
    ($addonEntry.Count -and $addonEntry[0].InstalledSha256 -cne $knownAddon)) {
    throw 'Addon source/installed hash changed; core-only update refused.'
}
$specs=New-Object 'Collections.Generic.List[object]'
if ($originalCore -cne $release.CoreSha256) {
    $specs.Add([pscustomobject]@{Name='core';Root=$game.GameDir;Target=(Join-Path $game.GameDir 'erbridge\erbridge_core.dll');Source=$nativeSource;Old=$originalCore;New=$release.CoreSha256;Backup=$null})
}

$roots=New-Object 'Collections.Generic.List[string]'
if ($PSBoundParameters.ContainsKey('InstancesDirectory')) {
    if (-not $InstancesDirectory.Count) { throw 'InstancesDirectory exige pelo menos uma pasta explicita.' }
    foreach ($root in $InstancesDirectory) { $roots.Add((Get-BridgeFullPath $root)) }
} else {
    if ($env:APPDATA) { $roots.Add((Get-BridgeFullPath (Join-Path $env:APPDATA 'PrismLauncher\instances'))) }
    $roots.Add((Get-BridgeFullPath (Join-Path (Split-Path -Parent $installed) 'PrismLauncher\instances')))
}
$seenRoots=@{}
$seenJars=@{}
foreach ($instances in $roots) {
    if ($seenRoots.ContainsKey($instances)) { continue }
    $seenRoots[$instances]=$true
    Assert-BridgeNoLinks $instances
    if (-not (Test-Path -LiteralPath $instances -PathType Container)) {
        if ($PSBoundParameters.ContainsKey('InstancesDirectory') -or (Test-Path -LiteralPath $instances)) { throw "InstancesDirectory nao e uma pasta existente: $instances" }
        continue
    }
    foreach ($dir in @(Get-ChildItem -LiteralPath $instances -Directory -Force)) {
        Assert-BridgeInside $dir.FullName $instances
        Assert-BridgeNoLinks $dir.FullName
        $mods=Join-Path $dir.FullName '.minecraft\mods'
        Assert-BridgeInside $mods $dir.FullName
        Assert-BridgeNoLinks $mods
        if (-not (Test-Path -LiteralPath $mods -PathType Container)) {
            if (Test-Path -LiteralPath $mods) { throw "Mods path is not a directory: $mods" }
            continue
        }
        # No instance.cfg, account file, game settings or saves need to be opened.
        $jars=@(Get-ChildItem -LiteralPath $mods -Filter 'er-bridge*.jar' -Force)
        if ($jars.Count -eq 0) { continue } # Unmodded profile.
        if ($jars.Count -ne 1 -or $jars[0].PSIsContainer) { throw ('Esperado um unico bridge JAR no perfil '+$dir.Name) }
        $target=$jars[0].FullName
        Assert-BridgeInside $target $dir.FullName
        if ($seenJars.ContainsKey($target)) { continue }
        if ($target -eq $game.GameDir -or (Test-BridgeInside $target $game.GameDir) -or
            (Test-BridgeInside $target $backupRoot)) { throw "Bridge JAR overlaps game/backup paths: $target" }
        $old=Get-BridgeHash $target
        if ($old -cnotin $knownJars) { throw ('Bridge JAR desconhecido no perfil '+$dir.Name) }
        $seenJars[$target]=$old
        if ($old -cne $release.JarSha256) {
            # Keep the old filename so replacement is one atomic file operation.
            $specs.Add([pscustomobject]@{Name=('java-'+$specs.Count);Root=$dir.FullName;Target=$target;Source=$jarSource;Old=$old;New=$release.JarSha256;Backup=$null})
        }
    }
}

# A native-only update leaves a custom Prism data directory on the old protocol.
# Refuse before creating backups or changing any installation file; the wrapper
# can ask for the exact instances directory and retry this same preflight.
if ($seenJars.Count -eq 0) {
    $missingProfiles=New-Object System.InvalidOperationException('Nenhum perfil Minecraft com ER Bridge encontrado. Informe -InstancesDirectory com a pasta instances do Prism usado para jogar; nada foi alterado.')
    $missingProfiles.Data['ErmcCode']='BridgeProfilesNotFound'
    throw $missingProfiles
}

# Baseline and ours-before are identical; Matt's QHD scripts are separately known.
$knownScripts=@{
    'Start-Coop.ps1'=@('58e7d806d4ff501c642a593373f5e8050020d8f1989c68351cf398f951992294','488998d8b342a6d8e197a81da73db5ac87ef694092b10976808fd5518ab97f6b')
    'Start-Bridge.ps1'=@('b37d6f7d595bdac7bdc9373e186e1dd47860c33be408c2fef5ad47c1692a5761','46ae2c0cf52163411f3a8718b96d1c5afa6cb9489254d27bf1bc04d9341f7a08')
    'Set-BridgeDisplay.ps1'=@('73e6b724100d1f8d06c41971d95a5d4a3e4285f7a90a5f7edff787984ecf607c','587536a6339ed40c1d183d88358f14c4fda9e6698ae4a1147e864662a99a07b3')
}
foreach ($name in @('Start-Coop.ps1','Start-Bridge.ps1','Set-BridgeDisplay.ps1')) {
    $source=Join-Path $PSScriptRoot $name
    $target=Join-Path (Join-Path $installed 'elden-ring\windows') $name
    Assert-BridgeInside $target $installed
    $new=Get-BridgeHash $source
    $old=Get-BridgeHash $target
    if ($old -cnotin @($knownScripts[$name]+@($new))) { throw "Launcher script desconhecido; nenhuma substituicao: $name" }
    if ($old -cne $new) {
        $specs.Add([pscustomobject]@{Name=('script-'+$name);Root=$installed;Target=$target;Source=$source;Old=$old;New=$new;Backup=$null})
    }
}
if ($specs.Count -eq 0) {
    [pscustomobject]@{Status='AlreadyInstalled';Protocol=4;FrameProtocol=3;JarName=$release.JarName;CoreSha256=$release.CoreSha256;JarSha256=$release.JarSha256;ProfilesVerified=$seenJars.Count;ProfileJars=@($seenJars.Keys | Sort-Object)}
    return
}
if (-not $PSCmdlet.ShouldProcess($game.GameDir, 'Fazer backups verificados e atualizar core, todos os bridge JARs e scripts para windows.6')) { return }
Assert-CoopGamesStopped
Assert-UpdateInstalledFiles $active.Manifest
Assert-BridgeFileState $active.Path $manifestBeforeHash
foreach ($target in $seenJars.Keys) { Assert-BridgeFileState $target $seenJars[$target] }
foreach ($spec in $specs) {
    Assert-BridgeFileState $spec.Target $spec.Old
    Assert-BridgeFileState $spec.Source $spec.New
}
$session=Join-Path (Split-Path -Parent $active.Path) ('coop-v2-'+[Guid]::NewGuid().ToString('N'))
Assert-BridgeInside $session $backupRoot
Assert-BridgeNoLinks $session
if (Test-Path -LiteralPath $session) { throw 'Backup session already exists.' }
$null=[IO.Directory]::CreateDirectory($session)
$manifestBackup=Join-Path $session 'manifest.before.json'
Copy-Item -LiteralPath $active.Path -Destination $manifestBackup
Assert-BridgeFileState $manifestBackup $manifestBeforeHash
foreach ($spec in $specs) {
    Assert-BridgeFileState $spec.Target $spec.Old
    $spec.Backup=Join-Path $session ($spec.Name+'.before.bin')
    Copy-Item -LiteralPath $spec.Target -Destination $spec.Backup
    Assert-BridgeFileState $spec.Backup $spec.Old
}
if ($originalCore -cne $release.CoreSha256) {
    $nextManifest=[IO.File]::ReadAllText($manifestBackup) | ConvertFrom-Json
    ($nextManifest.Entries | Where-Object RelativePath -ceq 'erbridge\erbridge_core.dll').InstalledSha256=$release.CoreSha256
    $manifestSource=Join-Path $session 'manifest.after.json'
    Save-BridgeManifest $nextManifest $manifestSource $backupRoot
    $specs.Add([pscustomobject]@{Name='manifest';Root=$backupRoot;Target=$active.Path;Source=$manifestSource;Old=$manifestBeforeHash;New=(Get-BridgeHash $manifestSource);Backup=$manifestBackup})
}
$result=[pscustomobject]@{
    Status='Updating';Protocol=4;FrameProtocol=3;JarName=$release.JarName;Backup=$session
    CoreSha256=$release.CoreSha256;JarSha256=$release.JarSha256
    ProfilesVerified=$seenJars.Count;ProfileJars=@($seenJars.Keys | Sort-Object)
    Files=@($specs | Select-Object Target,Old,New,Backup);RollbackErrors=@()
}
$resultPath=Join-Path $session 'update-result.json'
Save-BridgeManifest $result $resultPath $backupRoot
$attempted=New-Object 'Collections.Generic.List[object]'
try {
    foreach ($spec in $specs) {
        Assert-CoopGamesStopped
        Assert-BridgeFileState $spec.Backup $spec.Old
        # Track BEFORE writing: Replace may succeed and a later verification may throw.
        $attempted.Add($spec)
        Write-BridgeCheckedFile $spec.Target $spec.Source $spec.Old $spec.New $spec.Root
    }
    $verified=Read-BridgeManifest $active.Path $backupRoot $game.GameDir
    Assert-UpdateInstalledFiles $verified
    foreach ($spec in $specs) { Assert-BridgeFileState $spec.Target $spec.New }
    foreach ($target in $seenJars.Keys) { Assert-BridgeFileState $target $release.JarSha256 }
    $result.Status='Installed'
    Save-BridgeManifest $result $resultPath $backupRoot
} catch {
    $failure=$_
    $rollbackErrors=New-Object 'Collections.Generic.List[string]'
    # Restore data/scripts first and the manifest last, so metadata describes restored files.
    $rollback=@($attempted | Where-Object Name -ne 'manifest')
    [array]::Reverse($rollback)
    $rollback+=@($attempted | Where-Object Name -eq 'manifest')
    foreach ($spec in $rollback) {
        try {
            $actual=Get-BridgeHash $spec.Target
            if ($actual -ceq $spec.Old) { continue }
            if ($actual -cne $spec.New) { throw "Rollback refuses concurrently changed target: $($spec.Target)" }
            if ($spec.Name -eq 'manifest') { Assert-BridgeFileState (Join-Path $game.GameDir 'erbridge\erbridge_core.dll') $originalCore }
            Assert-BridgeFileState $spec.Backup $spec.Old
            Write-BridgeCheckedFile $spec.Target $spec.Backup $spec.New $spec.Old $spec.Root
        } catch { $rollbackErrors.Add($_.Exception.Message) }
    }
    $result.Status='RolledBack'
    if ($rollbackErrors.Count) { $result.Status='RollbackFailed' }
    $result.RollbackErrors=@($rollbackErrors.ToArray())
    try { Save-BridgeManifest $result $resultPath $backupRoot }
    catch { $rollbackErrors.Add('Could not record rollback: '+$_.Exception.Message) }
    if ($rollbackErrors.Count) { throw ("Update failed: $($failure.Exception.Message). Backups: $session. Rollback issues: "+($rollbackErrors -join '; ')) }
    throw $failure
}
$result
