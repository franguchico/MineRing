<#
Publica uma nova versao do MineRing no GitHub: gera dist\MineRing-Launcher.zip (Make-Dist) e cria a release.
O nome do arquivo e SEMPRE o mesmo, entao o link
  https://github.com/franguchico/MineRing/releases/latest/download/MineRing-Launcher.zip
baixa sempre a ultima versao (o site usa esse link).
Uso:  powershell -File tools\Publish-Release.ps1 -Version v0.2.0 [-Notes notas.md] [-Title "MineRing Launcher v0.2.0"]
Requer o gh CLI logado na conta dona do repo e o launcher ja compilado (Build.ps1).
#>
param(
    [Parameter(Mandatory=$true)][ValidatePattern('^v\d+\.\d+\.\d+$')][string]$Version,
    [string]$Notes,
    [string]$Title
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
# quem esta numa release antiga precisa conseguir atualizar pelo JOGAR (hashes antigos nas listas do updater)
& (Join-Path $PSScriptRoot 'Test-UpdateKnownHashes.ps1')
& (Join-Path $PSScriptRoot 'Make-Dist.ps1')
$zip = Join-Path $repo 'dist\MineRing-Launcher.zip'
if (-not (Test-Path $zip)) { throw "Nao achei $zip" }
if (-not $Title) { $Title = "MineRing Launcher $Version" }
$args2 = @('release','create',$Version,$zip,'--title',$Title,'--target','main')
if ($Notes) { $args2 += @('--notes-file',$Notes) } else { $args2 += @('--generate-notes') }
& gh @args2
if ($LASTEXITCODE -ne 0) { throw 'gh release create falhou.' }
Write-Host "Publicado: https://github.com/franguchico/MineRing/releases/tag/$Version"
Write-Host 'Link fixo da ultima versao: https://github.com/franguchico/MineRing/releases/latest/download/MineRing-Launcher.zip'
