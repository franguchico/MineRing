<#
.SYNOPSIS
Chooses a clean host/guest Prism profile; defaults to validation and import guidance.
.DESCRIPTION
Keep the two ZIPs produced by package-coop-roles.py beside this script, or pass
-ProfilesDirectory. Default use makes no changes and does not open a program.
Import the displayed ZIP through Prism > Add Instance > Import with a NEW name.
Optional -Install uses Install-CoopRoleProfile.ps1 to create a NEW instance only.
It never switches the role of an installed instance or changes an existing world.
The guest default identifier is EldenMinecraft-Coop-Convidado-v2. Conservative
options apply only to the clean instance; no existing settings are reset.

Host hosts BOTH Seamless and Minecraft. Guest joins BOTH. Nicks come from players.json.
Only these two public Minecraft profiles are authorized by the host whitelist;
UUIDs are identifiers, not passwords. Native/Seamless setup belongs to the parent
bundle. Two-PC multiplayer validation is pending.
.EXAMPLE
.\Select-CoopRoleProfile.ps1 -Role Host -ProfilesDirectory .\profiles
.EXAMPLE
.\Select-CoopRoleProfile.ps1 -Role Guest -ProfilesDirectory .\profiles -Install -WhatIf
#>
[CmdletBinding(SupportsShouldProcess=$true, ConfirmImpact='Medium')]
param(
    [ValidateSet('Host','Guest')][string]$Role,
    [string]$ProfilesDirectory = $PSScriptRoot,
    [string]$InstancesDirectory = (Join-Path $env:APPDATA 'PrismLauncher\instances'),
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,79}$')][string]$InstanceName,
    [switch]$Install,
    [string]$PlayersFile = (Join-Path $env:LOCALAPPDATA 'EldenMinecraftLauncher\players.json')
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $PlayersFile)) { throw ('players.json ausente: ' + $PlayersFile + '. Abra o launcher e informe os nicks.') }
$cfgPlayers = [IO.File]::ReadAllText($PlayersFile, [Text.Encoding]::UTF8) | ConvertFrom-Json
$HostNick = [string]$cfgPlayers.host.nick; $GuestNick = [string]$cfgPlayers.guest.nick
if (-not $HostNick -or -not $GuestNick) { throw 'players.json invalido (host/guest).' }
if (-not $Role) {
    Write-Host ('1 - ' + $HostNick + ': anfitriao do Seamless E do Minecraft')
    Write-Host ('2 - ' + $GuestNick + ': convidado nos dois jogos')
    $choice = Read-Host 'Escolha 1 ou 2'
    switch ($choice) {
        '1' { $Role = 'Host' }
        '2' { $Role = 'Guest' }
        default { throw 'Escolha invalida. Use -Role Host ou -Role Guest.' }
    }
}
$filename = if ($Role -ieq 'Host') { ('EldenMinecraft-Coop-Anfitriao-' + $HostNick + '-Prism.zip') } else { ('EldenMinecraft-Coop-Convidado-' + $GuestNick + '-Prism.zip') }
$arguments = @{
    Role=$Role; ProfileZip=(Join-Path $ProfilesDirectory $filename)
    InstancesDirectory=$InstancesDirectory; ValidateOnly=(-not $Install)
    WhatIf=[bool]$WhatIfPreference
}
if ($InstanceName) { $arguments.InstanceName = $InstanceName }
if ($PSBoundParameters.ContainsKey('Confirm')) { $arguments.Confirm = $PSBoundParameters['Confirm'] }
$result = & (Join-Path $PSScriptRoot 'Install-CoopRoleProfile.ps1') @arguments
Write-Host ('Papel selecionado: ' + $Role + '. ZIP: ' + $result.ProfileZip)
if (-not $result.Installed) { Write-Host 'Prism > Adicionar instancia > Importar: selecione esse ZIP e use um nome NOVO.' }
Write-Host 'Prism > Configuracoes > Java: ative selecao automatica e download automatico (Java 21). Entre online com sua propria conta.'
if ($Role -ieq 'Host') {
    Write-Host ($HostNick + ' hospeda Seamless e Minecraft. Este perfil novo inclui e4mc e autoriza apenas ' + $HostNick + ' e ' + $GuestNick + '.')
} else {
    Write-Host ($GuestNick + ' entra no Seamless de ' + $HostNick + '; no Minecraft use Multiplayer > Conexao direta com o endereco atual enviado pelo anfitriao.')
}
Write-Host 'Cooperativo experimental; funcionamento entre dois PCs ainda nao validado.'
$result
