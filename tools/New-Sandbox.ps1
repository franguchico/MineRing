<#
Cria uma pasta de teste isolada para o launcher (--sandbox <pasta>): Steam FALSA com um Elden Ring falso (so um
eldenring.exe vazio, x64, FileVersion 2.7.1.0, compilado agora com o csc do Windows), uma pasta .minecraft falsa
e um players.json com apelidos de TESTE. Nada aqui toca Steam, Prism, saves ou players.json reais.
Uso:  powershell -File tools\New-Sandbox.ps1 -Dir C:\temp\sb [-GameVersion 2.7.1.0] [-NoPlayers] [-NoMinecraft]
Depois: MineRing-Launcher.exe --sandbox C:\temp\sb [--offline-fixtures <pasta>] [--auto-install]
#>
param([Parameter(Mandatory=$true)][string]$Dir, [string]$GameVersion = '2.7.1.0', [switch]$NoPlayers, [switch]$NoMinecraft)
$ErrorActionPreference = 'Stop'
$Dir = [IO.Path]::GetFullPath($Dir)
$game = Join-Path $Dir 'steam\steamapps\common\ELDEN RING\Game'
$null = New-Item -ItemType Directory -Force -Path $game, (Join-Path $Dir 'state'), (Join-Path $Dir 'appdata\.minecraft\versions'), (Join-Path $Dir 'userprofile\Downloads')
$acf = @('"AppState"', '{', '  "appid"  "1245620"', '  "installdir"  "ELDEN RING"', '  "StateFlags"  "4"', '}') -join "`r`n"
[IO.File]::WriteAllText((Join-Path $Dir 'steam\steamapps\appmanifest_1245620.acf'), $acf, (New-Object Text.UTF8Encoding($false)))
$exe = Join-Path $game 'eldenring.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    $csc = Join-Path $env:windir 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    $src = Join-Path $Dir 'fake-er.cs'
    [IO.File]::WriteAllText($src, ('using System.Reflection; [assembly: AssemblyFileVersion("' + $GameVersion + '")] [assembly: AssemblyVersion("' + $GameVersion + '")] class P { static void Main() { } }'))
    & $csc /nologo /platform:x64 /target:exe "/out:$exe" $src
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao compilar o eldenring.exe falso.' }
    Remove-Item -LiteralPath $src -Force
}
if ($NoMinecraft) { Remove-Item -LiteralPath (Join-Path $Dir 'appdata\.minecraft') -Recurse -Force -ErrorAction SilentlyContinue }
if (-not $NoPlayers) {
    $json = '{ "host": { "nick": "TesteAnfitriao", "uuid": "11111111-1111-4111-8111-111111111111" }, "guest": { "nick": "TesteConvidado", "uuid": "22222222-2222-4222-8222-222222222222" } }'
    [IO.File]::WriteAllText((Join-Path $Dir 'state\players.json'), $json, (New-Object Text.UTF8Encoding($false)))
}
Write-Output ('Sandbox pronta em ' + $Dir + ' (apelidos de teste, Elden Ring falso ' + $GameVersion + ').')
