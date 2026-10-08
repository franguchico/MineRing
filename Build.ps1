# Compila MineRing-Launcher.exe usando o csc.exe do .NET Framework (ja vem no Windows). Nada para instalar.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$fx = Join-Path $env:windir 'Microsoft.NET\Framework64\v4.0.30319'
$csc = Join-Path $fx 'csc.exe'
if (-not (Test-Path $csc)) { throw 'csc.exe nao encontrado. Precisa do .NET Framework 4.x (ja vem no Windows 10/11).' }
$wpf = Join-Path $fx 'WPF'
$refs = @('WindowsBase','PresentationCore','PresentationFramework') | ForEach-Object { "/r:`"$wpf\$_.dll`"" }
$refs += @('System.Xaml','System.Core','System.Management','System.Windows.Forms','System.Drawing','System.IO.Compression','System.IO.Compression.FileSystem','System.Security') | ForEach-Object { "/r:`"$fx\$_.dll`"" }
$mp3 = Join-Path $root 'assets\musica-8bit.mp3'
$out = Join-Path $root 'MineRing-Launcher.exe'
$extra = @()
$avatar = Join-Path $root 'assets\avatar.jpg'
if (Test-Path $avatar) { $extra += "/resource:$avatar,avatar.jpg" }
$ico = Join-Path $root 'assets\icon.ico'
if (Test-Path $ico) { $extra += "/win32icon:$ico" }
if (Test-Path $mp3) { $extra += "/resource:$mp3,musica-8bit.mp3"; Write-Host 'Musica encontrada: embutindo.' } else { Write-Host 'Sem assets\musica-8bit.mp3: compilando sem musica (opcional).' }
& $csc /nologo /target:winexe /optimize+ /codepage:65001 /platform:anycpu "/out:$out" "/resource:$root\src\Ui.xaml.txt,Ui.xaml" "/resource:$root\assets\fonts\PressStart2P-Regular.ttf,PressStart2P-Regular.ttf" @extra @refs "$root\src\Launcher.cs" "$root\src\Fx.cs" "$root\src\Players.cs" "$root\src\Installer.cs" "$root\src\Senha.cs"
if ($LASTEXITCODE -ne 0) { throw "Falha na compilacao (codigo $LASTEXITCODE)." }
Write-Host "OK: $out"
