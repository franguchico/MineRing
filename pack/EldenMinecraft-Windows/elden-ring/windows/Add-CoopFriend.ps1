<# Add one authenticated Java profile to the separate co-op instance; no operator rights. #>
[CmdletBinding(SupportsShouldProcess=$true)]
param(
    [Parameter(Mandatory=$true)][ValidatePattern('^[A-Za-z0-9_]{3,16}$')][string]$PlayerName,
    [string]$InstanceDirectory = (Join-Path $env:APPDATA 'PrismLauncher\instances\EldenMinecraft-Coop')
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Bridge.Common.ps1')
if (@(Get-Process javaw -ErrorAction SilentlyContinue).Count) { throw 'Feche o Minecraft normalmente antes de alterar a lista de convidados.' }
$instance = Get-BridgeFullPath $InstanceDirectory
Assert-BridgeInside $instance (Get-BridgeFullPath (Join-Path $env:APPDATA 'PrismLauncher\instances'))
Assert-BridgeNoLinks $instance
$cfg = Join-Path $instance 'instance.cfg'
if (-not (Test-Path -LiteralPath $cfg -PathType Leaf) -or [IO.File]::ReadAllText($cfg) -notmatch '-Derbridge.coop=true') { throw 'Esta instancia nao e a instancia cooperativa.' }
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$profile = Invoke-RestMethod -Uri ('https://api.minecraftservices.com/minecraft/profile/lookup/name/' + $PlayerName)
if ($profile.id -notmatch '^[0-9a-fA-F]{32}$' -or $profile.name -ine $PlayerName) { throw 'A consulta oficial nao retornou o perfil esperado.' }
$uuid = [Guid]::ParseExact($profile.id, 'N').ToString()
$file = Join-Path $instance '.minecraft\whitelist.json'
Assert-BridgeNoLinks $file
$before = Get-BridgeFileState $file
$entries = @()
if ($before) { $entries = @([IO.File]::ReadAllText($file) | ConvertFrom-Json) }
if (@($entries | Where-Object { $_.uuid -eq $uuid }).Count) { Write-Output 'Este jogador ja esta autorizado.'; return }
$entries += [pscustomobject]@{ uuid=$uuid; name=$profile.name }
if (-not $PSCmdlet.ShouldProcess($file, ('Autorizar somente o jogador autenticado ' + $profile.name))) { return }
Assert-BridgeFileState $file $before
if ($before) { [IO.File]::Copy($file, ($file + '.backup-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ')), $false) }
$json = ConvertTo-Json -InputObject @($entries) -Depth 3
[IO.File]::WriteAllText($file, $json + [Environment]::NewLine, (New-Object Text.UTF8Encoding($false)))
Write-Output ('Jogador autorizado sem permissoes de operador: ' + $profile.name)
