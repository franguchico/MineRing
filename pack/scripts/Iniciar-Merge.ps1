[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$kit=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$config=Join-Path $kit 'configuracao-local.json'
if (-not (Test-Path -LiteralPath $config -PathType Leaf)) { throw 'Execute 1-Atualizar.cmd primeiro.' }
$state=[IO.File]::ReadAllText($config)|ConvertFrom-Json
$launcher=Join-Path $state.InstalledPackage 'elden-ring\windows\Start-Coop.ps1'
if (-not (Test-Path -LiteralPath $launcher -PathType Leaf)) { throw 'Pasta da instalacao anterior nao encontrada. Execute 1-Atualizar.cmd novamente com o caminho correto.' }
& $launcher -Confirm:$false
if (-not $?) { throw 'Seamless nao iniciou.' }
Write-Host 'Agora abra no Prism seu perfil ATUAL: Anfitriao ou Convidado.'
