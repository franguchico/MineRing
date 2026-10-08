<#
Compila os jogos FALSOS (tools\FakeGame.cs) dentro de <Sandbox>\fake com os nomes ersc_launcher.exe, eldenring.exe,
prismlauncher.exe e javaw.exe, e prepara a sandbox para o launcher achar tudo "instalado" (Steam falsa, Seamless falso,
perfil do Prism de anfitriao, pacote com bridge-backups). Serve para testar RODANDO/PARAR sem tocar nos jogos reais.
Uso:  powershell -File tools\New-FakeGames.ps1 -Sandbox C:\temp\sb
Depois: MineRing-Launcher.exe --sandbox C:\temp\sb --dry-run --skip-update --no-intro --no-music --simulate-games C:\temp\sb\fake
#>
param([Parameter(Mandatory=$true)][string]$Sandbox)
$ErrorActionPreference = 'Stop'
$Sandbox = [IO.Path]::GetFullPath($Sandbox)
& (Join-Path $PSScriptRoot 'New-Sandbox.ps1') -Dir $Sandbox | Out-Null
$fx = Join-Path $env:windir 'Microsoft.NET\Framework64\v4.0.30319'
$fake = Join-Path $Sandbox 'fake'
$null = New-Item -ItemType Directory -Force -Path $fake
$first = Join-Path $fake 'eldenring.exe'
& (Join-Path $fx 'csc.exe') /nologo /target:winexe /platform:x64 "/out:$first" "/r:$fx\System.Windows.Forms.dll" "/r:$fx\System.Drawing.dll" (Join-Path $PSScriptRoot 'FakeGame.cs')
if ($LASTEXITCODE -ne 0) { throw 'Falha ao compilar os jogos falsos.' }
foreach ($n in 'ersc_launcher', 'prismlauncher', 'javaw') { Copy-Item -LiteralPath $first -Destination (Join-Path $fake ($n + '.exe')) -Force }
# "instalacao" falsa para o launcher considerar tudo pronto (nada disso e real)
$game = Join-Path $Sandbox 'steam\steamapps\common\ELDEN RING\Game'
$null = New-Item -ItemType Directory -Force -Path (Join-Path $game 'SeamlessCoop')
Set-Content -LiteralPath (Join-Path $game 'ersc_launcher.exe') -Value 'fake' -Encoding ASCII
Set-Content -LiteralPath (Join-Path $game 'SeamlessCoop\ersc.dll') -Value 'fake' -Encoding ASCII
$pack = Join-Path $Sandbox 'pack-instalado'
$null = New-Item -ItemType Directory -Force -Path (Join-Path $pack 'bridge-backups'), (Join-Path $pack 'elden-ring\windows')
Set-Content -LiteralPath (Join-Path $pack 'elden-ring\windows\Start-Coop.ps1') -Value '# fake' -Encoding ASCII
$inst = Join-Path $Sandbox 'appdata\PrismLauncher\instances\ERMC-Teste-Anfitriao'
$null = New-Item -ItemType Directory -Force -Path $inst
[IO.File]::WriteAllText((Join-Path $inst 'instance.cfg'), "name=ERMC Anfitriao (falso)`r`nJvmArgs=-Derbridge.coop=true -Derbridge.coop.role=host`r`n")
$cfg = @('Role=host', 'InstanceId=ERMC-Teste-Anfitriao', ('PrismExe=' + (Join-Path $fake 'prismlauncher.exe')), ('InstallDir=' + $pack))
[IO.File]::WriteAllLines((Join-Path $Sandbox 'state\config.txt'), $cfg)
Write-Output ('Jogos falsos prontos em ' + $fake)
