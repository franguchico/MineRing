<#
Garante que quem esta em qualquer release ja publicada consegue atualizar pelo JOGAR:
os hashes (core, jar, proxy, addon) do coop-v2-release.json de cada tag v* precisam estar
nas listas de "conhecidos" do Update-CoopV2.ps1 atual (ou ser os da release atual).
Sem isso o updater recusa com "Bridge JAR desconhecido" / "core desconhecido".
Uso: powershell -File tools\Test-UpdateKnownHashes.ps1 [-Updater <caminho do Update-CoopV2.ps1>]
#>
param([string]$Updater)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$rel = 'pack/EldenMinecraft-Windows/elden-ring/windows/coop-v2-release.json'
if (-not $Updater) { $Updater = Join-Path $repo 'pack\EldenMinecraft-Windows\elden-ring\windows\Update-CoopV2.ps1' }
$text = [IO.File]::ReadAllText($Updater)
$current = Get-Content -Raw (Join-Path $repo $rel) | ConvertFrom-Json
$fail = 0
foreach ($tag in @(git -C $repo tag --list 'v*')) {
    $json = git -C $repo show ($tag + ':' + $rel) 2>$null
    if ($LASTEXITCODE -ne 0) { continue }
    $old = ($json -join "`n") | ConvertFrom-Json
    foreach ($f in 'CoreSha256', 'JarSha256', 'ProxySha256', 'AddonSha256') {
        $h = $old.$f
        if ($h -ceq $current.$f -or $text.Contains("'" + $h + "'")) { continue }
        Write-Host ('FALHOU ' + $tag + ' ' + $f + ' ' + $h + ' nao esta nas listas do Update-CoopV2.ps1')
        $fail++
    }
}
if ($fail) { throw ('' + $fail + ' hash(es) de releases antigas seriam recusados pelo updater.') }
Write-Host 'OK: todas as releases publicadas conseguem atualizar para a atual.'
