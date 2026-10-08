<#
.SYNOPSIS
Abre o Prism existente ou um Prism portatil opcional e prepara apenas o Java do perfil escolhido.
.DESCRIPTION
Nao baixa runtimes, nao importa/copias perfis e nao inicia Minecraft. Sem JavaHome
nem Java21 local, preserva as configuracoes Java do Prism. PrismLauncher e Java21
sao pastas OPCIONAIS ao lado de scripts; nao precisam constar no pacote distribuido.
Um Java21 local e conferido pelo arquivo release e pelo PE x64, sem executar Java.
Sem InstanceName explicito, abre apenas o Prism e nunca altera um perfil antigo.
Pode localizar o mesmo Prism portatil do kit antigo por configuracao-local.json,
campo InstalledPackage apontando para a antiga pasta EldenMinecraft-Windows.
Antes de alterar JavaPath, exige Prism alvo e Minecraft fechados, preserva encoding,
BOM, demais campos e uma copia original via substituicao atomica.
.PARAMETER PrismPath
Executavel prismlauncher.exe existente. Quando explicito, exige PrismDataDirectory
para evitar confundir a pasta de dados de outra instalacao portatil.
.PARAMETER PrismDataDirectory
Pasta de dados. Padrao: PrismLauncher local para o portatil do kit; caso contrario
APPDATA/PrismLauncher para uma instalacao detectada em Programs, Program Files ou PATH.
.PARAMETER JavaHome
JDK/JRE 21 x64 opcional, contendo release e bin/javaw.exe. Padrao opcional: Java21 local.
.PARAMETER InstanceName
Identificador EXPLICITO da instancia nova ja importada, necessario para ajustar
JavaPath. Sem este parametro, nenhum instance.cfg e lido ou modificado. Use o ID
real escolhido na importacao; o nome de um antigo perfil convidado nao e presumido.
.PARAMETER ValidateOnly
Confere somente arquivos/caminhos conhecidos. Nao escreve nem abre programas.
#>
[CmdletBinding(SupportsShouldProcess=$true, ConfirmImpact='Medium')]
param(
    [string]$PrismPath, [string]$PrismDataDirectory, [string]$JavaHome,
    [ValidateLength(1,120)][ValidatePattern('^(?![. ])[^\\/:*?"<>|\x00-\x1f]+(?<![. ])$')][string]$InstanceName,
    [switch]$ValidateOnly
)
$ErrorActionPreference='Stop'
$kitRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $kitRoot 'EldenMinecraft-Windows\elden-ring\windows\Bridge.Common.ps1')
Assert-BridgeWindows
if ($InstanceName -match '^(?i:CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\.|$)') { throw 'Identificador de instancia reservado pelo Windows.' }

function Get-PrismGeneralSection([string]$Text) {
    $sections=[regex]::Matches($Text,'(?m)^\[General\][ \t]*\r?$')
    if ($sections.Count -ne 1) { throw 'instance.cfg precisa conter uma unica secao [General].' }
    $start=$sections[0].Index+$sections[0].Length
    $remaining=$Text.Substring($start)
    $next=[regex]::Match($remaining,'(?m)^\[[^\r\n]+\][ \t]*\r?$')
    $length=$remaining.Length
    if ($next.Success) { $length=$next.Index }
    return [pscustomobject]@{Start=$start; Length=$length; Body=$remaining.Substring(0,$length); Remaining=$remaining}
}
function Set-GeneralValue([string]$Text,[string]$Key,[string]$Value) {
    $section=Get-PrismGeneralSection $Text
    $body=$section.Body
    $matches=[regex]::Matches($body,('(?m)^[ \t]*'+[regex]::Escape($Key)+'[ \t]*=[ \t]*(?<value>[^\r\n]*)'))
    if ($matches.Count -gt 1) { throw "Chave duplicada em instance.cfg: $Key" }
    if ($matches.Count -eq 1) {
        $span=$matches[0].Groups['value']
        $body=$body.Substring(0,$span.Index)+$Value+$body.Substring($span.Index+$span.Length)
    } else {
        $newline="`n"
        if ($Text.Contains("`r`n")) { $newline="`r`n" }
        if (-not $body.EndsWith("`n")) { $body+=$newline }
        $body+=$Key+'='+$Value+$newline
    }
    return $Text.Substring(0,$section.Start)+$body+$section.Remaining.Substring($section.Length)
}
function Assert-ThisPrismClosed([string]$Executable) {
    foreach ($taskProcess in @(Get-Process -Name prismlauncher -ErrorAction SilentlyContinue)) {
        $taskPath=$null
        try { $taskPath=$taskProcess.Path } catch {}
        if (-not $taskPath) { throw 'Nao foi possivel identificar um Prism aberto. Feche o Prism antes de ajustar JavaPath.' }
        if ((Get-BridgeFullPath $taskPath) -ieq $Executable) { throw 'Feche o Prism desta instalacao antes de ajustar JavaPath.' }
    }
    if (@(Get-Process -Name java,javaw -ErrorAction SilentlyContinue).Count) { throw 'Feche o Minecraft/Java antes de ajustar JavaPath.' }
}

$portableRoot=Join-Path $kitRoot 'PrismLauncher'
$portableExe=Join-Path $portableRoot 'prismlauncher.exe'
$oldKitRoot=$null
$localConfig=Join-Path $kitRoot 'configuracao-local.json'
if (-not $PrismPath -and (Test-Path -LiteralPath $localConfig)) {
    Assert-BridgeNoLinks $localConfig
    if (-not (Test-Path -LiteralPath $localConfig -PathType Leaf) -or (Get-Item -LiteralPath $localConfig).Length -gt 64KB) { throw 'configuracao-local.json invalido ou maior que 64 KiB.' }
    $local=[IO.File]::ReadAllText($localConfig) | ConvertFrom-Json
    # An instances root may live outside the Prism data root (InstDir setting).
    # Never guess a new data root and open a different set of accounts/worlds.
    $customRoots=$local.PSObject.Properties['InstancesDirectories']
    if ($customRoots -and @($customRoots.Value).Count -gt 0 -and -not $PrismDataDirectory) {
        throw 'A atualizacao usou uma pasta de instancias personalizada. Abra o Prism pelo seu atalho habitual. Para usar este script, informe explicitamente PrismDataDirectory da instalacao que voce usa.'
    }
    $installedField=$local.PSObject.Properties['InstalledPackage']
    if ($installedField) {
        if ($installedField.Value -isnot [string] -or -not [IO.Path]::IsPathRooted($installedField.Value) -or $installedField.Value -match '^[a-zA-Z]:[^\\/]') { throw 'InstalledPackage precisa ser o caminho absoluto do pacote antigo.' }
        $installedPackage=Get-BridgeFullPath $installedField.Value
        Assert-BridgeNoLinks $installedPackage
        if ([IO.Path]::GetFileName($installedPackage) -ine 'EldenMinecraft-Windows' -or -not (Test-Path -LiteralPath $installedPackage -PathType Container)) { throw 'InstalledPackage deve identificar a pasta EldenMinecraft-Windows existente do kit antigo.' }
        $oldKitRoot=[IO.Directory]::GetParent($installedPackage).FullName
    }
}
$portableData=@{}
$portableData[$portableExe]=$portableRoot
if ($PrismPath -and -not $PrismDataDirectory) { throw 'Com PrismPath explicito, informe tambem PrismDataDirectory da mesma instalacao.' }
if (-not $PrismPath) {
    $candidates=New-Object 'Collections.Generic.List[string]'
    if ($oldKitRoot) {
        $oldPrismRoot=Join-Path $oldKitRoot 'PrismLauncher'
        $oldPrismExe=Join-Path $oldPrismRoot 'prismlauncher.exe'
        $portableData[$oldPrismExe]=$oldPrismRoot
        $candidates.Add($oldPrismExe)
    }
    $candidates.Add($portableExe)
    if ($env:LOCALAPPDATA) { $candidates.Add((Join-Path $env:LOCALAPPDATA 'Programs\PrismLauncher\prismlauncher.exe')) }
    if ($env:ProgramFiles) { $candidates.Add((Join-Path $env:ProgramFiles 'PrismLauncher\prismlauncher.exe')) }
    foreach ($command in @(Get-Command prismlauncher.exe -CommandType Application -ErrorAction SilentlyContinue)) { $candidates.Add($command.Source) }
    foreach ($candidate in $candidates) {
        Assert-BridgeNoLinks $candidate
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $PrismPath=$candidate; break }
    }
    if (-not $PrismPath) { throw 'Prism nao encontrado. Use seu Prism instalado ou informe PrismPath e PrismDataDirectory; a pasta PrismLauncher no kit e opcional.' }
}
$PrismPath=Get-BridgeFullPath $PrismPath
if ([IO.Path]::GetFileName($PrismPath) -ine 'prismlauncher.exe') { throw 'PrismPath deve apontar para prismlauncher.exe.' }
$prismState=Get-BridgePE $PrismPath $false
if (-not $PrismDataDirectory) {
    if ($portableData.ContainsKey($PrismPath)) { $PrismDataDirectory=$portableData[$PrismPath] }
    else {
        if (-not $env:APPDATA -or -not [IO.Path]::IsPathRooted($env:APPDATA) -or $env:APPDATA -match '^[a-zA-Z]:[^\\/]') { throw 'APPDATA precisa ser absoluto; informe PrismDataDirectory.' }
        $PrismDataDirectory=Join-Path $env:APPDATA 'PrismLauncher'
    }
}
$PrismDataDirectory=Get-BridgeFullPath $PrismDataDirectory
Assert-BridgeNoLinks $PrismDataDirectory
if (Test-Path -LiteralPath $PrismDataDirectory) {
    if (-not (Test-Path -LiteralPath $PrismDataDirectory -PathType Container)) { throw 'PrismDataDirectory precisa ser uma pasta.' }
}
if ($PrismDataDirectory -match '["\r\n]') { throw 'Caminho de dados Prism nao pode conter aspas ou quebras de linha.' }
$instanceRoot=$null; $instanceCfg=$null; $originalHash=$null
if ($InstanceName) {
    $instanceRoot=Join-Path $PrismDataDirectory ('instances\'+$InstanceName)
    $instanceCfg=Join-Path $instanceRoot 'instance.cfg'
    Assert-BridgeInside $instanceRoot $PrismDataDirectory
    Assert-BridgeNoLinks $instanceCfg
    $originalHash=Get-BridgeFileState $instanceCfg
}
$javaExe=$null; $javaState=$null; $releasePath=$null; $releaseHash=$null
if (-not $JavaHome) {
    $javaRoots=@()
    if ($oldKitRoot) { $javaRoots+=Join-Path $oldKitRoot 'Java21' }
    $javaRoots+=Join-Path $kitRoot 'Java21'
    foreach ($candidate in $javaRoots) { if (Test-Path -LiteralPath $candidate) { $JavaHome=$candidate; break } }
}
if ($JavaHome) {
    $JavaHome=Get-BridgeFullPath $JavaHome
    $javaExe=Join-Path $JavaHome 'bin\javaw.exe'
    if ($javaExe -match '[",;\r\n]') { throw 'JavaHome contem caracteres nao suportados em JavaPath do Prism.' }
    Assert-BridgeNoLinks $javaExe
    # Opening the Prism UI with an unknown new profile must not require or alter
    # any Java configuration. Verify an optional Java only for explicit preparation.
    if ($InstanceName -or $PSBoundParameters.ContainsKey('JavaHome')) {
    $releasePath=Join-Path $JavaHome 'release'
    $releaseHash=Get-BridgeHash $releasePath
    if ((Get-Item -LiteralPath $releasePath).Length -gt 64KB) { throw 'Metadados Java release excedem 64 KiB.' }
    $release=[IO.File]::ReadAllText($releasePath)
    $versions=[regex]::Matches($release,'(?m)^JAVA_VERSION="(?<v>[^"\r\n]+)"\r?$')
    $architectures=[regex]::Matches($release,'(?m)^OS_ARCH="(?<v>[^"\r\n]+)"\r?$')
    if ($versions.Count -ne 1 -or $versions[0].Groups['v'].Value -notmatch '^21(?:[.+_-]|$)' -or
        $architectures.Count -ne 1 -or $architectures[0].Groups['v'].Value -notmatch '^(amd64|x86_64)$') { throw 'JavaHome deve conter Java 21 x64, conferido pelo arquivo release.' }
    $javaState=Get-BridgePE $javaExe $false
    } elseif (-not (Test-Path -LiteralPath $javaExe -PathType Leaf)) { $javaExe=$null }
}
$changeRequired=$false; $updatedBytes=$null
if ($originalHash -and $javaExe) {
    if ((Get-Item -LiteralPath $instanceCfg).Length -gt 1MB) { throw 'instance.cfg excede 1 MiB.' }
    $original=[IO.File]::ReadAllBytes($instanceCfg)
    $bom=0; $encoding=New-Object Text.UTF8Encoding($false,$true)
    if ($original.Length -ge 3 -and $original[0] -eq 0xef -and $original[1] -eq 0xbb -and $original[2] -eq 0xbf) { $bom=3 }
    elseif ($original.Length -ge 2 -and $original[0] -eq 0xff -and $original[1] -eq 0xfe) { $encoding=New-Object Text.UnicodeEncoding($false,$false,$true); $bom=2 }
    elseif ($original.Length -ge 2 -and $original[0] -eq 0xfe -and $original[1] -eq 0xff) { $encoding=New-Object Text.UnicodeEncoding($true,$false,$true); $bom=2 }
    $text=$encoding.GetString($original,$bom,$original.Length-$bom)
    $general=Get-PrismGeneralSection $text
    $jvm=[regex]::Matches($general.Body,'(?m)^[ \t]*JvmArgs[ \t]*=[ \t]*(?<value>[^\r\n]*)')
    if ($jvm.Count -ne 1 -or $jvm[0].Groups['value'].Value -notmatch '(?:^|\s)-Derbridge.coop=true(?:\s|$)' -or
        $jvm[0].Groups['value'].Value -notmatch '(?:^|\s)-Derbridge.coop.role=(?:host|guest)(?:\s|$)') { throw 'A instancia selecionada nao e um perfil co-op do MineRing.' }
    $updated=Set-GeneralValue $text 'OverrideJavaLocation' 'true'
    # QSettings treats backslashes as escapes. Patch only the two Java keys.
    $updated=Set-GeneralValue $updated 'JavaPath' ($javaExe.Replace('\','/'))
    $changeRequired=$updated -cne $text
    $body=$encoding.GetBytes($updated)
    $updatedBytes=New-Object byte[] ($bom+$body.Length)
    if ($bom) { [Array]::Copy($original,0,$updatedBytes,0,$bom) }
    [Array]::Copy($body,0,$updatedBytes,$bom,$body.Length)
    if ($changeRequired -and ((Get-Item -LiteralPath $instanceCfg).Attributes -band [IO.FileAttributes]::ReadOnly) -ne 0) { throw 'instance.cfg esta somente leitura; atributos preservados.' }
}
$result=[pscustomobject]@{KitRoot=$kitRoot; PrismExecutable=$PrismPath; PrismData=$PrismDataDirectory;
    JavaExecutable=$javaExe; InstanceConfig=$instanceCfg; InstanceConfigPresent=($null -ne $originalHash);
    JavaPathNeedsUpdate=$changeRequired; BackupPath=$null; Launched=$false}
if ($ValidateOnly) { return $result }
if (-not $PSCmdlet.ShouldProcess($PrismPath,'Preparar Java opcional do perfil escolhido e abrir somente a interface do Prism')) { return $result }
Assert-BridgeFileState $PrismPath $prismState.Sha256
Assert-BridgeNoLinks $PrismDataDirectory
if ($InstanceName) { Assert-BridgeFileState $instanceCfg $originalHash }
if ($javaState) { Assert-BridgeFileState $javaExe $javaState.Sha256; Assert-BridgeFileState $releasePath $releaseHash }
if ($changeRequired) {
    Assert-ThisPrismClosed $PrismPath
    $temporary=$instanceCfg+'.java-'+[Guid]::NewGuid().ToString('N')+'.tmp'
    $backup=$instanceCfg+'.java-backup-'+[Guid]::NewGuid().ToString('N')
    Assert-BridgeInside $temporary $instanceRoot; Assert-BridgeInside $backup $instanceRoot
    Assert-BridgeNoLinks $temporary; Assert-BridgeNoLinks $backup
    try {
        $stream=[IO.File]::Open($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
        try { $stream.Write($updatedBytes,0,$updatedBytes.Length) } finally { $stream.Dispose() }
        Assert-ThisPrismClosed $PrismPath
        Assert-BridgeFileState $instanceCfg $originalHash
        Assert-BridgeFileState $backup $null
        [IO.File]::Replace($temporary,$instanceCfg,$backup)
        Assert-BridgeFileState $backup $originalHash
        if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($instanceCfg)) -cne [Convert]::ToBase64String($updatedBytes)) { throw ('instance.cfg mudou apos a gravacao. Original preservado em '+$backup) }
        $result.BackupPath=$backup
    } finally { if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) } }
}
if (-not $InstanceName) { Write-Host 'Abra seu perfil ATUAL (Anfitriao ou Convidado). Este comando apenas abre o Prism.' }
elseif (-not $originalHash) { Write-Host 'Importe o ZIP limpo do seu papel no Prism. Depois informe o ID real da instancia para usar o Java21 local opcional.' }
elseif (-not $javaExe) { Write-Host 'Usando as configuracoes Java existentes do Prism. O perfil Minecraft 1.21.1 requer Java 21.' }
$startInfo=New-Object Diagnostics.ProcessStartInfo
$startInfo.FileName=$PrismPath; $startInfo.Arguments='--dir "'+$PrismDataDirectory+'"'
$startInfo.WorkingDirectory=[IO.Path]::GetDirectoryName($PrismPath)
$startInfo.UseShellExecute=$false; $startInfo.WindowStyle=[Diagnostics.ProcessWindowStyle]::Normal
Assert-BridgeFileState $PrismPath $prismState.Sha256
$process=[Diagnostics.Process]::Start($startInfo)
if ($process) { $process.Dispose() }
$result.Launched=$true
return $result
