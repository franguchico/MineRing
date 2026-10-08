<#
Passos da instalacao guiada do launcher (aba INSTALAR). Cada passo e idempotente e devolve linhas parseaveis:
  PROGRESS|<0-100>|<texto>   RESULT|<status>|<info>   ERROR|<codigo>|<detalhe>
Reaproveita os scripts do pack (Install-Bridge, Install-Coop, Install-CoopRoleProfile): nada de logica nova de
instalacao no jogo. Nunca toca em saves/mundos. Backup antes de substituir. Nao baixa nada (downloads sao do launcher).
Passos: pack (copia o pack para a pasta do launcher), bridge, seamless, password (troca a senha da sessao), profile, validate.
#>
param(
    [Parameter(Mandatory=$true)][ValidateSet('pack','bridge','seamless','password','profile','validate')][string]$Step,
    [Parameter(Mandatory=$true)][string]$PackageRoot,
    [string]$GameDir, [string]$SteamPath, [string]$InstancesDirectory,
    [ValidateSet('host','guest')][string]$Role, [string]$PlayersFile,
    [string]$FabricApiJar, [string]$E4mcJar, [string]$ArchivePath, [string]$PasswordFile
)
$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false) } catch { }
$source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\EldenMinecraft-Windows'))
$PackageRoot = [IO.Path]::GetFullPath($PackageRoot).TrimEnd('\')
$CommonRel = 'elden-ring\windows\Bridge.Common.ps1'

function Out-Result([string]$Status, [string]$Info) { Write-Output ('RESULT|' + $Status + '|' + $Info) }
function Out-Prog([int]$Pct, [string]$Text) { Write-Output ('PROGRESS|' + $Pct + '|' + $Text) }
function Fail([string]$Code, [string]$Msg) { $e = New-Object System.InvalidOperationException($Msg); $e.Data['ErmcCode'] = $Code; throw $e }
function Get-Sha([string]$Path) {
    $h = [Security.Cryptography.SHA256]::Create(); $s = [IO.File]::OpenRead($Path)
    try { return ([BitConverter]::ToString($h.ComputeHash($s))).Replace('-', '').ToLowerInvariant() } finally { $s.Dispose(); $h.Dispose() }
}
function Get-ShaBytes([byte[]]$Bytes) {
    $h = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($h.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() } finally { $h.Dispose() }
}
function Get-Release([string]$Root) {
    $p = Join-Path $Root 'elden-ring\windows\coop-v2-release.json'
    if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { Fail 'BinariosAusentes' ('coop-v2-release.json ausente em ' + $Root) }
    return ([IO.File]::ReadAllText($p) | ConvertFrom-Json)
}
function Get-SteamArg { if ($SteamPath) { return @{ SteamPath = @($SteamPath) } } else { return @{} } }

# ---- pack: copia o minimo para a pasta do launcher (hash conferido; o que existir e diferente vai para launcher-backups) ----
function Step-Pack {
    $rel = Get-Release $source
    $bin = 'elden-ring\er-bridge\build-msvc\Release'
    $list = New-Object 'Collections.Generic.List[string]'
    $list.Add('LICENSE')
    foreach ($f in @(Get-ChildItem -LiteralPath (Join-Path $source 'elden-ring\windows') -Recurse -File)) { $list.Add($f.FullName.Substring($source.Length + 1)) }
    foreach ($b in @('dinput8.dll', 'erbridge_core.dll', 'ErmcDepth.addon64')) { $list.Add($bin + '\' + $b) }
    $list.Add('elden-ring\mc-bridge\build\libs\' + $rel.JarName)
    foreach ($r in $list) {
        if (-not (Test-Path -LiteralPath (Join-Path $source $r) -PathType Leaf)) { Fail 'BinariosAusentes' ('Arquivo ausente no pack: ' + $r) }
    }
    $checks = @(@{ P = ($bin + '\erbridge_core.dll'); H = $rel.CoreSha256 }, @{ P = ($bin + '\dinput8.dll'); H = $rel.ProxySha256 },
        @{ P = ('elden-ring\mc-bridge\build\libs\' + $rel.JarName); H = $rel.JarSha256 }, @{ P = ($bin + '\ErmcDepth.addon64'); H = $rel.AddonSha256 })
    foreach ($c in $checks) { if ((Get-Sha (Join-Path $source $c.P)) -cne $c.H) { Fail 'BinarioHash' ('Hash diferente do esperado: ' + $c.P) } }
    if ($PackageRoot -ieq $source) { Out-Result 'AlreadyInstalled' '0'; return }
    $stamp = (Get-Date).ToString('yyyyMMdd-HHmmss'); $backup = Join-Path $PackageRoot ('launcher-backups\' + $stamp)
    $copied = 0; $i = 0
    foreach ($r in $list) {
        $i++; Out-Prog ([int](100 * $i / $list.Count)) ('Copiando arquivos da ponte (' + $i + '/' + $list.Count + ')')
        $from = Join-Path $source $r; $to = Join-Path $PackageRoot $r; $new = Get-Sha $from
        if (Test-Path -LiteralPath $to -PathType Leaf) {
            if ((Get-Sha $to) -ceq $new) { continue }
            $bk = Join-Path $backup $r
            $null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $bk)
            [IO.File]::Copy($to, $bk, $true)
        }
        $null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to)
        $tmp = $to + '.tmp-' + [guid]::NewGuid().ToString('N')
        [IO.File]::Copy($from, $tmp, $false)
        if ((Get-Sha $tmp) -cne $new) { [IO.File]::Delete($tmp); Fail 'BinarioHash' ('Copia nao confere: ' + $r) }
        if (Test-Path -LiteralPath $to -PathType Leaf) { [IO.File]::Replace($tmp, $to, [NullString]::Value) } else { [IO.File]::Move($tmp, $to) }
        $copied++
    }
    if ($copied -eq 0) { Out-Result 'AlreadyInstalled' '0' } else { Out-Result 'Installed' ([string]$copied) }
}

# ---- bridge: Install-Bridge (backup de saves, manifesto). Pula se ja ha instalacao ativa conferida ----
function Step-Bridge {
    . (Join-Path $PackageRoot $CommonRel)
    Assert-BridgeWindows
    Out-Prog 10 'Conferindo o Elden Ring (versao 2.7.1.0)'
    $game = Get-BridgeGame $GameDir (@($SteamPath) | Where-Object { $_ })
    $backup = Get-BridgeBackupDir $null $game.GameDir
    $active = Find-BridgeManifest $backup $game.GameDir
    if ($active) {
        if ($active.Manifest.Status -ceq 'Installed') { Assert-BridgeInstalledFiles $active.Manifest; Out-Result 'AlreadyInstalled' ''; return }
        Fail 'BridgeIncompleto' ('Instalacao anterior com status ' + $active.Manifest.Status + ' em ' + $active.Path)
    }
    # o release.json so e preciso para instalar; pacotes antigos (instalados antes do windows.6) nao o tem
    $rel = Get-Release $PackageRoot
    Assert-BridgeStopped
    Out-Prog 40 'Instalando a ponte com backup dos saves'
    $a = @{ GameDir = $GameDir; OwnedSteamGame = $true; ProxySha256 = $rel.ProxySha256; CoreSha256 = $rel.CoreSha256; Confirm = $false }
    foreach ($k in (Get-SteamArg).Keys) { $a[$k] = (Get-SteamArg)[$k] }
    $null = & (Join-Path $PackageRoot 'elden-ring\windows\Install-Bridge.ps1') @a
    Out-Result 'Installed' ''
}

# ---- seamless: Install-Coop com o ZIP oficial v2.0.1 (hash pinado no proprio Install-Coop) ----
function Step-Seamless {
    . (Join-Path $PackageRoot $CommonRel)
    Assert-BridgeWindows
    Out-Prog 10 'Conferindo o jogo e a ponte'
    $game = Get-BridgeGame $GameDir (@($SteamPath) | Where-Object { $_ })
    $coopRoot = Join-Path $script:BridgePackageRoot 'coop-backups'
    if (Test-Path -LiteralPath $coopRoot -PathType Container) {
        foreach ($d in @(Get-ChildItem -LiteralPath $coopRoot -Directory)) {
            $f = Join-Path $d.FullName 'coop-manifest.json'
            if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { continue }
            $m = [IO.File]::ReadAllText($f) | ConvertFrom-Json
            if ($m.GameDir -eq $game.GameDir -and $m.Status -ceq 'Installed') { Out-Result 'AlreadyInstalled' ''; return }
        }
    }
    if (-not $ArchivePath -or -not (Test-Path -LiteralPath $ArchivePath -PathType Leaf)) { Fail 'SemZip' 'ZIP do Seamless nao informado.' }
    Out-Prog 40 'Instalando o Seamless Co-op (backup dos saves antes)'
    $a = @{ GameDir = $GameDir; ArchivePath = $ArchivePath; Confirm = $false }
    if ($PasswordFile) { $a.PasswordFile = $PasswordFile }
    foreach ($k in (Get-SteamArg).Keys) { $a[$k] = (Get-SteamArg)[$k] }
    $null = & (Join-Path $PackageRoot 'elden-ring\windows\Install-Coop.ps1') @a
    Out-Result 'Installed' ''
}

# ---- password: grava a senha da sessao no ersc_settings.ini E acerta o hash desse arquivo no coop-manifest.json.
# O Start-Coop (Install-Coop -ValidateOnly) confere o hash do ini; editar so o ini quebraria o JOGAR.
# Ordem a prova de queda: payload-2.bin (copia do manifesto) -> ini -> manifesto. Se cair no meio, a proxima chamada continua.
# Regra: o valor da senha NUNCA vai para stdout, stderr nem mensagem de erro (tudo isso acaba no launcher.log).
function Step-Password {
    . (Join-Path $PackageRoot $CommonRel)
    Assert-BridgeWindows
    if (-not $PasswordFile -or -not (Test-Path -LiteralPath $PasswordFile -PathType Leaf)) { Fail 'SenhaInvalida' 'Senha nao informada.' }
    $pw = [IO.File]::ReadAllText($PasswordFile).Trim()
    if ($pw -cnotmatch '^[A-Za-z0-9_-]{12,128}$') { Fail 'SenhaInvalida' 'A senha precisa ter de 12 a 128 caracteres (letras, numeros, _ ou -). Valor omitido.' }
    Out-Prog 10 'Conferindo o jogo e o registro do Seamless'
    $game = Get-BridgeGame $GameDir (@($SteamPath) | Where-Object { $_ })
    $coopRoot = Get-BridgeBackupDir (Join-Path $script:BridgePackageRoot 'coop-backups') $game.GameDir
    $found = @()
    if (Test-Path -LiteralPath $coopRoot -PathType Container) {
        foreach ($d in @(Get-ChildItem -LiteralPath $coopRoot -Directory)) {
            Assert-BridgeNoLinks $d.FullName
            $f = Join-Path $d.FullName 'coop-manifest.json'
            if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { continue }
            Assert-BridgeNoLinks $f
            $hdr = [IO.File]::ReadAllText($f) | ConvertFrom-Json
            if ($hdr.GameDir -ne $game.GameDir -or $hdr.Status -ceq 'RolledBack') { continue }
            $found += [pscustomobject]@{ Path = $f; Manifest = $hdr }
        }
    }
    if ($found.Count -eq 0) { Out-Result 'NoSeamless' ''; return }
    if ($found.Count -gt 1) { Fail 'SeamlessMultiplo' 'Ha mais de uma instalacao do Seamless registrada. Nao mexi em nada.' }
    $mf = $found[0].Path; $m = $found[0].Manifest
    if ($m.Kind -cne 'ErmcSeamless' -or $m.Status -cne 'Installed') { Fail 'SeamlessIncompleto' ('Instalacao do Seamless com status ' + $m.Status + '. Nao mexi em nada.') }
    $e = @($m.Entries | Where-Object { $_.RelativePath -ceq 'SeamlessCoop\ersc_settings.ini' })
    if ($e.Count -ne 1 -or $e[0].BackupName -cne 'payload-2.bin' -or $e[0].InstalledSha256 -cnotmatch '^[a-f0-9]{64}$') { Fail 'ManifestoInvalido' 'O registro do Seamless nao lista o ersc_settings.ini como esperado.' }
    $ini = Join-Path $game.GameDir 'SeamlessCoop\ersc_settings.ini'
    Assert-BridgeInside $ini $game.GameDir
    $cur = Get-BridgeFileState $ini
    if ($null -eq $cur) { Fail 'IniAusente' 'SeamlessCoop\ersc_settings.ini nao existe na pasta do jogo.' }
    $session = Split-Path -Parent $mf
    $payload = Join-Path $session 'payload-2.bin'
    Assert-BridgeInside $payload $session
    $payHash = Get-BridgeFileState $payload
    if ($null -eq $payHash) { Fail 'BackupAusente' 'Falta o payload-2.bin do registro do Seamless.' }
    if ($cur -cne $e[0].InstalledSha256 -and $cur -cne $payHash) {
        Fail 'IniAlterado' 'O ersc_settings.ini foi alterado fora do launcher (o hash nao bate com o registro). Por seguranca nao sobrescrevi.'
    }
    Out-Prog 40 'Gravando a senha'
    $text = [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($ini))
    $rx = '(?m)^([ \t]*)cooppassword[ \t]*=[^\r\n]*'
    if ([regex]::Matches($text, $rx).Count -ne 1) { Fail 'IniInvalido' 'O ersc_settings.ini nao tem exatamente uma linha cooppassword.' }
    $new = [regex]::Replace($text, $rx, ('${1}cooppassword = ' + $pw))
    $bytes = (New-Object Text.UTF8Encoding($false)).GetBytes($new)
    $newHash = Get-ShaBytes $bytes
    $pw = $null; $text = $null; $new = $null
    if ($cur -ceq $newHash -and $e[0].InstalledSha256 -ceq $newHash -and $payHash -ceq $newHash) { Out-Result 'Unchanged' ''; return }
    $tmp = Join-Path $session ('.pw-' + [guid]::NewGuid().ToString('N') + '.tmp')
    Assert-BridgeInside $tmp $session
    try {
        [IO.File]::WriteAllBytes($tmp, $bytes)
        if ($payHash -cne $newHash) { Write-BridgeCheckedFile $payload $tmp $payHash $newHash $session }
        if ($cur -cne $newHash) { Write-BridgeCheckedFile $ini $tmp $cur $newHash $game.GameDir }
    } finally {
        if (Test-Path -LiteralPath $tmp -PathType Leaf) { [IO.File]::WriteAllBytes($tmp, (New-Object byte[] $bytes.Length)); [IO.File]::Delete($tmp) }
    }
    Out-Prog 80 'Atualizando o registro do Seamless'
    if ($e[0].InstalledSha256 -cne $newHash) {
        $e[0].InstalledSha256 = $newHash
        Save-BridgeManifest $m $mf $coopRoot
    }
    Out-Result 'Updated' ''
}

# ---- profile: monta o ZIP de papel no formato exato e deixa Install-CoopRoleProfile.ps1 validar e criar a instancia ----
function Step-Profile {
    if (-not $Role) { Fail 'Parametro' 'Role ausente.' }
    $rel = Get-Release $source
    $loader = if ($rel.PSObject.Properties['FabricLoader']) { [string]$rel.FabricLoader } else { '0.19.5' }
    if ($loader -cnotmatch '^[A-Za-z0-9.+_-]+$') { Fail 'Parametro' 'FabricLoader invalido.' }
    if (-not $PlayersFile -or -not (Test-Path -LiteralPath $PlayersFile -PathType Leaf)) { Fail 'PlayersInvalido' 'players.json ausente.' }
    $pl = [IO.File]::ReadAllText($PlayersFile, [Text.Encoding]::UTF8) | ConvertFrom-Json
    foreach ($k in 'host', 'guest') {
        $p = $pl.$k
        if (-not $p -or "$($p.nick)" -cnotmatch '^[A-Za-z0-9_]{3,16}$' -or "$($p.uuid)" -cnotmatch '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$') { Fail 'PlayersInvalido' ('players.json invalido para ' + $k) }
    }
    $hostNick = [string]$pl.host.nick; $hostUuid = [string]$pl.host.uuid; $guestNick = [string]$pl.guest.nick; $guestUuid = [string]$pl.guest.uuid
    $jarSrc = Join-Path $source ('elden-ring\mc-bridge\build\libs\' + $rel.JarName)
    if (-not (Test-Path -LiteralPath $jarSrc -PathType Leaf)) { Fail 'BinariosAusentes' ('Jar da ponte ausente: ' + $rel.JarName) }
    if ((Get-Sha $jarSrc) -cne $rel.JarSha256) { Fail 'BinarioHash' 'Jar da ponte nao confere com o release.' }
    if (-not $FabricApiJar -or (Split-Path -Leaf $FabricApiJar) -cnotmatch '^fabric-api-[A-Za-z0-9.+_-]+\.jar$' -or -not (Test-Path -LiteralPath $FabricApiJar -PathType Leaf)) { Fail 'SemMods' 'Fabric API ausente.' }
    if ($Role -eq 'host' -and (-not $E4mcJar -or (Split-Path -Leaf $E4mcJar) -cne 'e4mc-fabric-6.2.3.jar' -or -not (Test-Path -LiteralPath $E4mcJar -PathType Leaf))) { Fail 'SemMods' 'e4mc ausente.' }
    Out-Prog 20 'Montando o perfil do Minecraft'
    $utf8 = New-Object Text.UTF8Encoding($false)
    $title = if ($Role -eq 'host') { 'Anfitriao - ' + $hostNick } else { 'Convidado - ' + $guestNick }
    $note = if ($Role -eq 'host') { $hostNick + ' hospeda Seamless e Minecraft. Mundo novo; somente ' + $hostNick + ' e ' + $guestNick + ' autorizados.' } else { $guestNick + ' entra no Seamless de ' + $hostNick + ' e usa Conexao direta no Multiplayer.' }
    $cfg = [ordered]@{
        name = ('EldenMinecraft Coop - ' + $title); iconKey = 'default'; InstanceType = 'OneSix'
        notes = ($note + ' Experimental. Prism: ativar selecao e download automaticos de Java 21 em Configuracoes > Java.')
        OverrideMemory = 'true'; MinMemAlloc = '2048'; MaxMemAlloc = '6144'
        OverrideWindow = 'true'; LaunchMaximized = 'false'; MinecraftWinWidth = '1280'; MinecraftWinHeight = '720'
        OverrideJavaLocation = 'false'; AutomaticJava = 'true'; OverrideJavaArgs = 'true'
        JvmArgs = ('-Derbridge.coop=true -Derbridge.coop.role=' + $Role)
        OverrideMiscellaneous = 'true'; ShowConsole = 'false'; ShowConsoleOnError = 'true'
        UseAccountForInstance = 'false'; JoinServerOnLaunch = 'false'
    }
    $cfgText = '[General]' + "`n" + (($cfg.Keys | ForEach-Object { $_ + '=' + $cfg[$_] }) -join "`n") + "`n"
    $files = [ordered]@{}
    $files['instance.cfg'] = $utf8.GetBytes($cfgText)
    $files['mmc-pack.json'] = $utf8.GetBytes('{"formatVersion":1,"components":[{"uid":"net.minecraft","version":"1.21.1","important":true},{"uid":"net.fabricmc.fabric-loader","version":"' + $loader + '"}]}')
    $files['.minecraft/options.txt'] = $utf8.GetBytes("maxFps:60`nrenderDistance:6`nsimulationDistance:5`npauseOnLostFocus:false`n")
    $files['.minecraft/mods/' + $rel.JarName] = [IO.File]::ReadAllBytes($jarSrc)
    $files['.minecraft/mods/' + (Split-Path -Leaf $FabricApiJar)] = [IO.File]::ReadAllBytes($FabricApiJar)
    if ($Role -eq 'host') {
        $files['.minecraft/mods/e4mc-fabric-6.2.3.jar'] = [IO.File]::ReadAllBytes($E4mcJar)
        $files['.minecraft/whitelist.json'] = $utf8.GetBytes('[{"name":"' + $hostNick + '","uuid":"' + $hostUuid + '"},{"name":"' + $guestNick + '","uuid":"' + $guestUuid + '"}]')
    }
    $policy = ('Restricted to ' + $hostNick + ' and ' + $guestNick + ' only. ') + 'UUIDs are public Minecraft profile identifiers, not credentials. No operator rights are granted.'
    $recs = [ordered]@{}
    foreach ($n in @($files.Keys)) { $recs[$n] = [ordered]@{ bytes = $files[$n].Length; sha256 = (Get-ShaBytes $files[$n]) } }
    $manifest = [ordered]@{ format = 'eldenminecraft-coop-role-v1'; role = $Role; minecraft = '1.21.1'; java_major = 21; fabric_loader = $loader; host = $hostNick; guest = $guestNick; whitelist_policy = $policy; files = $recs }
    $files['coop-role-profile.json'] = $utf8.GetBytes(($manifest | ConvertTo-Json -Depth 6))
    $tmpDir = Join-Path $PackageRoot 'launcher-tmp'
    $null = New-Item -ItemType Directory -Force -Path $tmpDir
    $zipPath = Join-Path $tmpDir ('role-' + [guid]::NewGuid().ToString('N') + '.zip')
    try {
        Add-Type -AssemblyName System.IO.Compression
        $fs = [IO.File]::Open($zipPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
        try {
            $za = New-Object IO.Compression.ZipArchive($fs, [IO.Compression.ZipArchiveMode]::Create)
            try {
                foreach ($n in @($files.Keys)) {
                    $en = $za.CreateEntry($n, [IO.Compression.CompressionLevel]::Optimal); $st = $en.Open()
                    try { $st.Write($files[$n], 0, $files[$n].Length) } finally { $st.Dispose() }
                }
            } finally { $za.Dispose() }
        } finally { $fs.Dispose() }
        Out-Prog 60 'Validando e criando o perfil no Prism'
        $installer = Join-Path $PackageRoot 'elden-ring\windows\Install-CoopRoleProfile.ps1'
        if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) { $installer = Join-Path $source 'elden-ring\windows\Install-CoopRoleProfile.ps1' }
        $r = & $installer -Role $Role -ProfileZip $zipPath -InstancesDirectory $InstancesDirectory -PlayersFile $PlayersFile -Confirm:$false
        $null = $r
        Out-Result 'Installed' ''
    } finally {
        if (Test-Path -LiteralPath $zipPath -PathType Leaf) { [IO.File]::Delete($zipPath) }
        if ((Test-Path -LiteralPath $tmpDir -PathType Container) -and -not @(Get-ChildItem -LiteralPath $tmpDir -Force).Count) { [IO.Directory]::Delete($tmpDir) }
    }
}

# ---- validate: exatamente a checagem que o Start-Coop faz antes de abrir o jogo ----
function Step-Validate {
    . (Join-Path $PackageRoot $CommonRel)
    Out-Prog 30 'Conferindo ponte e Seamless (somente leitura)'
    $a = @{ ValidateOnly = $true; GameDir = $GameDir }
    foreach ($k in (Get-SteamArg).Keys) { $a[$k] = (Get-SteamArg)[$k] }
    $null = & (Join-Path $PackageRoot 'elden-ring\windows\Install-Coop.ps1') @a
    Out-Result 'Valid' ''
}

try {
    switch ($Step) {
        'pack'     { Step-Pack }
        'bridge'   { Step-Bridge }
        'seamless' { Step-Seamless }
        'password' { Step-Password }
        'profile' { Step-Profile }
        'validate' { Step-Validate }
    }
} catch {
    $code = ''
    try { $code = [string]$_.Exception.Data['ErmcCode'] } catch { }
    Write-Output ('ERROR|' + $code + '|' + (($_.Exception.Message) -replace '[\r\n]+', ' '))
    exit 2
}
