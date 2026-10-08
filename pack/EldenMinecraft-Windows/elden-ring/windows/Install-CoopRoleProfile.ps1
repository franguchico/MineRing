<#
.SYNOPSIS
Validates/imports a clean role ZIP into a NEW Prism instance, without launching.
.DESCRIPTION
Prefer Prism > Add Instance > Import for normal use. This optional helper only
creates a fresh folder; it never toggles an existing role, copies a save, reads
accounts, edits global Prism settings, or downloads/starts Java. Existing instance
names (including EldenMinecraft-Coop) are refused. Close Prism before installing
with this helper, then reopen it to refresh its instance list.

The host hosts BOTH Seamless and Minecraft; the guest joins BOTH. Host and guest
nicks/UUIDs come from the user's players.json (%LOCALAPPDATA%\EldenMinecraftLauncher,
written by the launcher; override with -PlayersFile).
The host whitelist is restricted to exactly these two authenticated Minecraft
profiles. Their public UUIDs are identifiers, never credentials or operator grants.
Use the separate native/Seamless installation instructions from the parent bundle.
In Prism Settings > Java enable automatic selection AND automatic download; these
are global Prism settings. Minecraft 1.21.1 requests Java 21 with no packaged path.
Two-PC multiplayer validation is still pending.
Guest default identifier: EldenMinecraft-Coop-Convidado-v2. Clean options seed
only maxFps:60, renderDistance:6, simulationDistance:5, pauseOnLostFocus:false.
These apply to the NEW instance only; no existing settings are reset.
.EXAMPLE
.\Install-CoopRoleProfile.ps1 -Role Host -ProfileZip .\EldenMinecraft-Coop-Anfitriao-<host>-Prism.zip -ValidateOnly
.EXAMPLE
.\Install-CoopRoleProfile.ps1 -Role Guest -ProfileZip .\EldenMinecraft-Coop-Convidado-<guest>-Prism.zip -WhatIf
#>
[CmdletBinding(SupportsShouldProcess=$true, ConfirmImpact='Medium')]
param(
    [Parameter(Mandatory=$true)][ValidateSet('Host','Guest')][string]$Role,
    [Parameter(Mandatory=$true)][string]$ProfileZip,
    [string]$InstancesDirectory = (Join-Path $env:APPDATA 'PrismLauncher\instances'),
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,79}$')][string]$InstanceName,
    [switch]$ValidateOnly,
    [string]$PlayersFile = (Join-Path $env:LOCALAPPDATA 'EldenMinecraftLauncher\players.json')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
$roleKey = $Role.ToLowerInvariant()

# Jogadores (nick + UUID publico) vem do players.json do usuario; nada fixo no pack.
if (-not (Test-Path -LiteralPath $PlayersFile)) { throw ('players.json ausente: ' + $PlayersFile + '. Abra o launcher e informe os nicks do anfitriao e do convidado.') }
$cfgPlayers = [IO.File]::ReadAllText($PlayersFile, [Text.Encoding]::UTF8) | ConvertFrom-Json
foreach ($k in 'host','guest') {
    $pl = $cfgPlayers.$k
    if (-not $pl -or "$($pl.nick)" -cnotmatch '^[A-Za-z0-9_]{3,16}$' -or "$($pl.uuid)" -cnotmatch '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$') { throw ('players.json invalido para ' + $k + ' (nick 3-16 letras/numeros/_ e UUID com hifens).') }
}
$HostNick = [string]$cfgPlayers.host.nick; $HostUuid = [string]$cfgPlayers.host.uuid
$GuestNick = [string]$cfgPlayers.guest.nick; $GuestUuid = [string]$cfgPlayers.guest.uuid
if (-not $InstanceName) {
    $InstanceName = if ($roleKey -eq 'host') { ('EldenMinecraft-Coop-Host-' + $HostNick) } else { 'EldenMinecraft-Coop-Convidado-v2' }
}
if ($InstanceName -match '^(?i:CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])$') { throw 'Nome de instancia reservado pelo Windows.' }

function Assert-CoopRoleNoLinks([string]$Path) {
    $absolute = [IO.Path]::GetFullPath($Path)
    $cursor = $absolute
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw ('Caminho vinculado recusado: ' + $cursor) }
        }
        $parent = [IO.Directory]::GetParent($cursor)
        $cursor = if ($parent) { $parent.FullName } else { $null }
    }
    return $absolute
}

function Get-CoopRoleSha([byte[]]$Bytes) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose() }
}

function Convert-CoopRoleText([byte[]]$Bytes) {
    $encoding = New-Object Text.UTF8Encoding($false, $true)
    return $encoding.GetString($Bytes)
}

function Assert-CoopRoleProperties($Object, [string[]]$Expected) {
    $actual = @($Object.PSObject.Properties | ForEach-Object { $_.Name })
    if ($actual.Count -ne $Expected.Count) { throw 'Estrutura de metadados inesperada.' }
    foreach ($name in $Expected) { if ($actual -cnotcontains $name) { throw ('Campo de metadados ausente: ' + $name) } }
}

$zipPath = Assert-CoopRoleNoLinks $ProfileZip
$root = (Assert-CoopRoleNoLinks $InstancesDirectory).TrimEnd('\','/')
if (-not (Test-Path -LiteralPath $root -PathType Container)) { throw 'A pasta instances do Prism deve existir; informe -InstancesDirectory para um Prism portatil.' }
$destination = Assert-CoopRoleNoLinks (Join-Path $root $InstanceName)
if ([IO.Directory]::GetParent($destination).FullName -ine $root) { throw 'Destino fora da pasta de instancias.' }
if (Test-Path -LiteralPath $destination) { throw 'A instancia ja existe. Escolha um NOVO nome; nenhum perfil ou mundo sera substituido.' }
if (-not (Test-Path -LiteralPath $zipPath -PathType Leaf) -or (Get-Item -LiteralPath $zipPath).Length -gt 64MB) { throw 'ZIP ausente ou maior que 64 MiB.' }

# Read and validate the complete archive before creating any folder. SHA-256
# records validate decompressed bytes; the Python packager additionally tests CRC.
$raw = [IO.File]::ReadAllBytes($zipPath)
$zipSha = Get-CoopRoleSha $raw
$memory = [IO.MemoryStream]::new($raw, $false)
$archive = [IO.Compression.ZipArchive]::new($memory, [IO.Compression.ZipArchiveMode]::Read, $false)
$files = New-Object 'Collections.Generic.Dictionary[string,byte[]]' ([StringComparer]::Ordinal)
$seen = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
try {
    $expectedCount = if ($roleKey -eq 'host') { 8 } else { 6 }
    if ($archive.Entries.Count -ne $expectedCount) { throw 'Quantidade de arquivos inesperada: perfil incompleto ou com dados extras.' }
    [long]$total = 0
    foreach ($entry in $archive.Entries) {
        $name = $entry.FullName
        if (-not $seen.Add($name) -or $name.Contains('\') -or $name.Contains(':') -or $name.StartsWith('/') -or $name.EndsWith('/') -or ($name.Split('/') -contains '..')) { throw ('Nome ZIP inseguro/duplicado: ' + $name) }
        $unixType = ($entry.ExternalAttributes -shr 16) -band 0xF000
        if ($unixType -ne 0 -and $unixType -ne 0x8000) { throw 'O ZIP contem um link ou entrada que nao e arquivo regular.' }
        $total += $entry.Length
        if ($entry.Length -gt 16MB -or $total -gt 64MB) { throw 'O ZIP excede os limites do perfil limpo.' }
        $stream = $entry.Open()
        $buffer = [IO.MemoryStream]::new()
        try {
            $chunk = New-Object byte[] 65536
            while (($read = $stream.Read($chunk, 0, $chunk.Length)) -gt 0) {
                if ($buffer.Length + $read -gt $entry.Length -or $buffer.Length + $read -gt 16MB) { throw 'Conteudo descompactado excede o tamanho declarado.' }
                $buffer.Write($chunk, 0, $read)
            }
            if ($buffer.Length -ne $entry.Length) { throw 'Tamanho descompactado incorreto.' }
            $files.Add($name, $buffer.ToArray())
        } finally { $stream.Dispose(); $buffer.Dispose() }
    }
} finally { $archive.Dispose(); $memory.Dispose() }

if (-not $files.ContainsKey('coop-role-profile.json')) { throw 'Manifesto do perfil ausente.' }
$manifest = (Convert-CoopRoleText $files['coop-role-profile.json']) | ConvertFrom-Json
if ($manifest.format -cne 'eldenminecraft-coop-role-v1' -or $manifest.role -cne $roleKey -or $manifest.minecraft -cne '1.21.1' -or $manifest.java_major -ne 21 -or $manifest.host -cne $HostNick -or $manifest.guest -cne $GuestNick) { throw 'Manifesto de papel/versao incompativel.' }
if ($manifest.fabric_loader -cnotmatch '^[A-Za-z0-9.+_-]+$') { throw 'Versao do Fabric Loader invalida.' }
$policy = ('Restricted to ' + $HostNick + ' and ' + $GuestNick + ' only. ') + 'UUIDs are public Minecraft profile identifiers, not credentials. No operator rights are granted.'
if ($manifest.whitelist_policy -cne $policy) { throw 'A autorizacao deve ser restrita aos dois jogadores solicitados.' }

$bridgeNames = @($files.Keys | Where-Object { $_ -cmatch '^\.minecraft/mods/er-bridge-[A-Za-z0-9.+_-]+\.jar$' })
$fabricNames = @($files.Keys | Where-Object { $_ -cmatch '^\.minecraft/mods/fabric-api-[A-Za-z0-9.+_-]+\.jar$' })
if ($bridgeNames.Count -ne 1 -or $fabricNames.Count -ne 1) { throw 'Bridge/Fabric API ausente ou ambiguo.' }
$allowed = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
foreach ($name in @('instance.cfg','mmc-pack.json','coop-role-profile.json','.minecraft/options.txt',$bridgeNames[0],$fabricNames[0])) { $null = $allowed.Add($name) }
if ($roleKey -eq 'host') {
    $null = $allowed.Add('.minecraft/mods/e4mc-fabric-6.2.3.jar')
    $null = $allowed.Add('.minecraft/whitelist.json')
}
foreach ($name in $files.Keys) { if (-not $allowed.Contains($name)) { throw ('Arquivo fora da lista permitida: ' + $name) } }
foreach ($name in $allowed) { if (-not $files.ContainsKey($name)) { throw ('Arquivo permitido ausente: ' + $name) } }
$records = @($manifest.files.PSObject.Properties)
if ($records.Count -ne $files.Count - 1) { throw 'Manifesto de hashes incompleto.' }
foreach ($record in $records) {
    if ($record.Name -ceq 'coop-role-profile.json' -or -not $allowed.Contains($record.Name)) { throw 'Nome inesperado no manifesto de hashes.' }
    Assert-CoopRoleProperties $record.Value @('bytes','sha256')
    if ($record.Value.sha256 -cnotmatch '^[0-9a-f]{64}$' -or $record.Value.sha256 -cne (Get-CoopRoleSha $files[$record.Name]) -or $record.Value.bytes -ne $files[$record.Name].Length) { throw ('Hash/tamanho incorreto: ' + $record.Name) }
}

# Only the exact authored config keys/values are permitted: no launch commands,
# account bindings, Java paths, server address, or arbitrary JVM arguments.
$title = if ($roleKey -eq 'host') { ('Anfitriao - ' + $HostNick) } else { ('Convidado - ' + $GuestNick) }
$note = if ($roleKey -eq 'host') { ($HostNick + ' hospeda Seamless e Minecraft. Mundo novo; somente ' + $HostNick + ' e ' + $GuestNick + ' autorizados.') } else { ($GuestNick + ' entra no Seamless de ' + $HostNick + ' e usa Conexao direta no Multiplayer.') }
$expectedConfig = @{
    name=('EldenMinecraft Coop - ' + $title); iconKey='default'; InstanceType='OneSix'
    notes=($note + ' Experimental. Prism: ativar selecao e download automaticos de Java 21 em Configuracoes > Java.')
    OverrideMemory='true'; MinMemAlloc='2048'; MaxMemAlloc='6144'
    OverrideWindow='true'; LaunchMaximized='false'; MinecraftWinWidth='1280'; MinecraftWinHeight='720'
    OverrideJavaLocation='false'; AutomaticJava='true'; OverrideJavaArgs='true'
    JvmArgs=('-Derbridge.coop=true -Derbridge.coop.role=' + $roleKey)
    OverrideMiscellaneous='true'; ShowConsole='false'; ShowConsoleOnError='true'
    UseAccountForInstance='false'; JoinServerOnLaunch='false'
}
$parsed = New-Object 'Collections.Generic.Dictionary[string,string]' ([StringComparer]::Ordinal)
$configLines = (Convert-CoopRoleText $files['instance.cfg']).Split("`n")
if ($configLines[0].TrimEnd("`r") -cne '[General]') { throw 'Secao de configuracao invalida.' }
foreach ($line in $configLines[1..($configLines.Count - 1)]) {
    $line = $line.TrimEnd("`r")
    if ($line -ceq '') { continue }
    $separator = $line.IndexOf('=')
    if ($separator -lt 1) { throw 'Linha de configuracao inesperada.' }
    $key = $line.Substring(0, $separator)
    if ($parsed.ContainsKey($key)) { throw 'Configuracao duplicada.' }
    $parsed.Add($key, $line.Substring($separator + 1))
}
if ($parsed.Count -ne $expectedConfig.Count) { throw 'Configuracao com campos extras/ausentes.' }
foreach ($key in $expectedConfig.Keys) { if (-not $parsed.ContainsKey($key) -or $parsed[$key] -cne $expectedConfig[$key]) { throw ('Configuracao inesperada: ' + $key) } }
$expectedOptions = "maxFps:60`nrenderDistance:6`nsimulationDistance:5`npauseOnLostFocus:false`n"
if ((Convert-CoopRoleText $files['.minecraft/options.txt']) -cne $expectedOptions) { throw 'options.txt deve conter somente as quatro opcoes conservadoras do perfil novo.' }

$pack = (Convert-CoopRoleText $files['mmc-pack.json']) | ConvertFrom-Json
Assert-CoopRoleProperties $pack @('formatVersion','components')
if ($pack.formatVersion -ne 1 -or @($pack.components).Count -ne 2) { throw 'Formato do perfil Prism invalido.' }
Assert-CoopRoleProperties $pack.components[0] @('uid','version','important')
Assert-CoopRoleProperties $pack.components[1] @('uid','version')
if ($pack.components[0].uid -cne 'net.minecraft' -or $pack.components[0].version -cne '1.21.1' -or $pack.components[0].important -ne $true -or $pack.components[1].uid -cne 'net.fabricmc.fabric-loader' -or $pack.components[1].version -cne $manifest.fabric_loader) { throw 'Componentes Minecraft/Fabric inesperados.' }

if ($roleKey -eq 'host') {
    # Windows PowerShell 5.1 emits a JSON array as one pipeline object; assigning
    # first and then wrapping the value keeps the two entries flat on 5.1 and 7.
    $playersJson = (Convert-CoopRoleText $files['.minecraft/whitelist.json']) | ConvertFrom-Json
    $players = @($playersJson)
    $expectedPlayers = @(
        @{name=$HostNick; uuid=$HostUuid},
        @{name=$GuestNick; uuid=$GuestUuid}
    )
    if ($players.Count -ne 2) { throw 'Whitelist deve conter somente anfitriao e convidado.' }
    for ($i=0; $i -lt 2; $i++) {
        Assert-CoopRoleProperties $players[$i] @('name','uuid')
        if ($players[$i].name -cne $expectedPlayers[$i].name -or $players[$i].uuid -cne $expectedPlayers[$i].uuid) { throw 'Jogador/UUID inesperado na whitelist.' }
    }
}

$result = [pscustomobject]@{
    Role=$roleKey; ProfileZip=$zipPath; ZipSHA256=$zipSha; NewInstanceDirectory=$destination
    Files=$files.Count; Validated=$true; Installed=$false; RuntimeVerified=$false
    Java='Prism Settings > Java: enable automatic selection AND download (Java 21)'
    WhitelistPolicy=$policy
}
if ($ValidateOnly) { return $result }
if (-not $PSCmdlet.ShouldProcess($destination, ('Importar perfil limpo ' + $roleKey + ' em NOVA instancia Prism'))) { return $result }

# Stage only our allowlisted bytes, then atomically rename to the new instance.
# This helper never writes a pre-existing instance, even if Minecraft is running.
$stage = Join-Path $root ('.coop-role-import-' + [Guid]::NewGuid().ToString('N'))
$ownsStage = $false
try {
    $null = Assert-CoopRoleNoLinks $root
    if (Test-Path -LiteralPath $destination) { throw 'O destino foi criado durante a validacao; importacao recusada.' }
    if (Test-Path -LiteralPath $stage) { throw 'Pasta temporaria ja existe.' }
    $null = [IO.Directory]::CreateDirectory($stage)
    $ownsStage = $true
    foreach ($name in $allowed) {
        $target = Join-Path $stage ($name.Replace('/', [IO.Path]::DirectorySeparatorChar))
        if (-not [IO.Path]::GetFullPath($target).StartsWith($stage + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Extracao fora da pasta temporaria.' }
        $null = Assert-CoopRoleNoLinks $target
        $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
        $writer = [IO.File]::Open($target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try { $writer.Write($files[$name], 0, $files[$name].Length) } finally { $writer.Dispose() }
        if ((Get-CoopRoleSha ([IO.File]::ReadAllBytes($target))) -cne (Get-CoopRoleSha $files[$name])) { throw 'Falha de verificacao apos gravacao.' }
    }
    $null = Assert-CoopRoleNoLinks $stage
    $null = Assert-CoopRoleNoLinks $destination
    [IO.Directory]::Move($stage, $destination) # Fails if the target already exists.
    $result.Installed = $true
    Write-Host 'Reabra o Prism e selecione a nova instancia. Java: ative selecao e download automaticos nas Configuracoes > Java. Inicie online com sua propria conta.'
    return $result
} finally {
    if ($ownsStage -and (Test-Path -LiteralPath $stage)) {
        # Resolve/check the exact absolute target inside the requested root before
        # deleting our own temporary directory; never delete the final instance.
        $checked = Assert-CoopRoleNoLinks $stage
        if ([IO.Directory]::GetParent($checked).FullName -ine $root -or [IO.Path]::GetFileName($checked) -cnotmatch '^\.coop-role-import-[0-9a-f]{32}$') { throw 'Limpeza fora da pasta temporaria recusada.' }
        $pending = New-Object 'Collections.Generic.Queue[string]'
        $pending.Enqueue($checked)
        while ($pending.Count -gt 0) {
            foreach ($child in Get-ChildItem -LiteralPath $pending.Dequeue() -Force) {
                if (($child.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Limpeza recusada: link inesperado na pasta temporaria.' }
                if ($child.PSIsContainer) { $pending.Enqueue($child.FullName) }
            }
        }
        [IO.Directory]::Delete($checked, $true)
    }
}
