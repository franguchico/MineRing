<#
Teste ponta a ponta do RODANDO/PARAR com jogos FALSOS (sandbox). Nunca toca nos jogos reais:
 - o launcher roda com --sandbox + --simulate-games (so enxerga processos cujo exe fica dentro da sandbox);
 - UI Automation parte da janela do PROPRIO launcher de teste (por ProcessId);
 - os falsos e o launcher de teste so morrem por PID rastreado / exe dentro da sandbox.
Uso: powershell -File tools\Test-StopFlow.ps1 [-Work C:\temp\sbtest] [-Exe <exe>] [-Shots <pasta>]
Sai com codigo 1 se algum caso falhar.
#>
param([string]$Work = (Join-Path $env:TEMP 'erkmc-stoptest'), [string]$Exe, [string]$Shots)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing
if (-not $Exe) { $Exe = Join-Path $PSScriptRoot '..\MineRing-Launcher.exe' }
$Exe = [IO.Path]::GetFullPath($Exe); $Work = [IO.Path]::GetFullPath($Work)
if (-not $Shots) { $Shots = Join-Path $Work 'shots' }
$null = New-Item -ItemType Directory -Force -Path $Shots
Add-Type -Namespace W -Name N -MemberDefinition '[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f); [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r); public struct RECT { public int L, T, R, B; }'
$fail = 0
function Check([bool]$ok, [string]$msg) { if ($ok) { Write-Host ('  OK   ' + $msg) -ForegroundColor Green } else { Write-Host ('  FALHOU ' + $msg) -ForegroundColor Red; $script:fail++ } }

# --- protecao: processos reais que NUNCA podem morrer ---
$realNames = 'eldenring', 'java', 'javaw', 'prismlauncher', 'ersc_launcher', 'MineRing-Launcher', 'EldenMinecraft-Launcher'
$sbNorm = $Work.TrimEnd('\') + '\'
function InSb($p) { try { return ($p.Path -and $p.Path.StartsWith($sbNorm, [StringComparison]::OrdinalIgnoreCase)) } catch { return $false } }
$guard = @(Get-Process | Where-Object { $realNames -contains $_.Name -and -not (InSb $_) } | ForEach-Object { [pscustomobject]@{ Id = $_.Id; Name = $_.Name; Start = $_.StartTime } })
Write-Host ('Processos reais protegidos: ' + (($guard | ForEach-Object { $_.Name + ':' + $_.Id }) -join ', '))
function GuardOk { $ok = $true; foreach ($g in $guard) { $p = Get-Process -Id $g.Id -ErrorAction SilentlyContinue; if (-not $p -or $p.StartTime -ne $g.Start) { Write-Host ('  !! sumiu: ' + $g.Name + ' ' + $g.Id + ' as ' + (Get-Date -Format HH:mm:ss)) -ForegroundColor Yellow; $ok = $false } }; return $ok }
function SbProcs { @(Get-Process | Where-Object { InSb $_ -and $_.Path -notlike '*\MineRing-Launcher*' }) }
function KillSb { foreach ($p in (SbProcs)) { try { Stop-Process -Id $p.Id -Force -ErrorAction Stop } catch { } } }

# --- sandbox ---
KillSb
if (Test-Path -LiteralPath $Work) { Get-ChildItem -LiteralPath $Work -Force | Where-Object { $_.Name -ne 'shots' } | Remove-Item -Recurse -Force }
& (Join-Path $PSScriptRoot 'New-FakeGames.ps1') -Sandbox $Work | Out-Null
$fake = Join-Path $Work 'fake'

$script:L = $null; $script:LP = $null
function StartLauncher([string]$stub, [switch]$NoSim) {
    $a = @('--sandbox', $Work, '--dry-run', '--skip-update', '--no-intro', '--no-music', '--low-fx')
    if (-not $NoSim) { $a += @('--simulate-games', $fake) }
    if ($stub) { $a += @('--sim-stubborn', $stub) }
    $script:LP = Start-Process -FilePath $Exe -ArgumentList $a -PassThru
    $script:L = $null
    for ($i = 0; $i -lt 60 -and -not $script:L; $i++) {
        Start-Sleep -Milliseconds 500
        $script:L = [Windows.Automation.AutomationElement]::RootElement.FindFirst('Children', (New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::ProcessIdProperty, $script:LP.Id)))
    }
    if (-not $script:L) { throw 'Janela do launcher de teste nao apareceu.' }
    Start-Sleep -Milliseconds 2500
}
function StopLauncher { if ($script:LP -and -not $script:LP.HasExited) { Stop-Process -Id $script:LP.Id -Force }; $script:L = $null }
function El([string]$id) { $script:L.FindFirst('Descendants', (New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::AutomationIdProperty, $id))) }
function Txt([string]$id) { $e = El $id; if ($e) { $e.Current.Name } else { $null } }
function Vis([string]$id) { $e = El $id; [bool]($e -and -not $e.Current.IsOffscreen) }
function Click([string]$id) { $e = El $id; if (-not $e) { throw ('Elemento ' + $id + ' nao encontrado') }; $e.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke() }
function WaitFor([scriptblock]$c, [int]$sec = 20) { for ($i = 0; $i -lt $sec * 4; $i++) { if (& $c) { return $true }; Start-Sleep -Milliseconds 250 }; return $false }
function BtnName { (El 'BtnPlay').Current.Name }
function Shot([string]$name) {
    $h = [IntPtr]$script:L.Current.NativeWindowHandle; $r = New-Object W.N+RECT; [void][W.N]::GetWindowRect($h, [ref]$r)
    $bmp = New-Object Drawing.Bitmap(($r.R - $r.L), ($r.B - $r.T)); $g = [Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [void][W.N]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc); $g.Dispose()
    $bmp.Save((Join-Path $Shots ($name + '.png')), [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
}
function FakeCount([string]$n) { @(SbProcs | Where-Object { $_.Name -eq $n }).Count }
function Ctx { Check (GuardOk) 'processos reais continuam vivos' }

try {
    Write-Host "`n== Caso 1: JOGAR -> RODANDO -> PARAR -> confirma -> so os falsos morrem -> volta a JOGAR =="
    StartLauncher
    Check ((BtnName) -eq 'Jogar') 'botao comeca como Jogar'
    Check (-not (Vis 'RunText')) 'selo RODANDO escondido antes'
    Shot '1-antes-jogar'
    Click 'BtnPlay'
    Check (WaitFor { (BtnName) -like 'Parar*' } 30) 'depois do JOGAR o botao vira PARAR'
    Start-Sleep -Milliseconds 1200
    Check (Vis 'RunText') 'selo RODANDO visivel'
    Check ((FakeCount 'eldenring') -eq 1 -and (FakeCount 'javaw') -eq 1) 'fake eldenring e fake javaw estao rodando'
    Shot '2-rodando'
    Click 'BtnPlay'
    Check (WaitFor { Vis 'ConfTitle' } 10) 'confirmacao apareceu'
    Shot '3-confirmacao'
    Click 'BtnConfYes'
    $null = WaitFor { (El 'PlayText') -and (El 'PlayText').Current.Name -like 'FECHANDO*' } 5
    Start-Sleep -Milliseconds 600
    Shot '4-fechando'
    Check (WaitFor { (BtnName) -eq 'Jogar' } 60) 'voltou a JOGAR sozinho'
    Check ((FakeCount 'eldenring') -eq 0 -and (FakeCount 'javaw') -eq 0 -and (FakeCount 'prismlauncher') -eq 0 -and (FakeCount 'ersc_launcher') -eq 0) 'nenhum falso restou'
    Check ($script:LP.HasExited -eq $false) 'launcher de teste continua vivo'
    Shot '5-depois'
    Ctx; StopLauncher; KillSb

    Write-Host "`n== Caso 2: cancelar a confirmacao =="
    StartLauncher
    Click 'BtnPlay'; $null = WaitFor { (BtnName) -like 'Parar*' } 30; Start-Sleep -Milliseconds 800
    Click 'BtnPlay'; $null = WaitFor { Vis 'ConfTitle' } 10
    Click 'BtnConfNo'; Start-Sleep -Milliseconds 1500
    Check (-not (Vis 'ConfTitle')) 'confirmacao fechou'
    Check ((FakeCount 'eldenring') -eq 1 -and (FakeCount 'javaw') -eq 1) 'cancelar nao fechou nada'
    Check ((BtnName) -like 'Parar*') 'continua PARAR'
    Ctx; StopLauncher; KillSb

    Write-Host "`n== Caso 3: Elden Ring que ignora CloseMainWindow (precisa do Kill) =="
    StartLauncher 'er'
    Click 'BtnPlay'; $null = WaitFor { (BtnName) -like 'Parar*' } 30; Start-Sleep -Milliseconds 800
    Click 'BtnPlay'; $null = WaitFor { Vis 'ConfTitle' } 10; $t0 = Get-Date; Click 'BtnConfYes'
    Check (WaitFor { (BtnName) -eq 'Jogar' } 90) 'voltou a JOGAR depois do kill'
    $dt = ((Get-Date) - $t0).TotalSeconds
    Check ($dt -ge 9) ('esperou ~10 s antes de forcar (levou ' + [int]$dt + ' s)')
    Check ((FakeCount 'eldenring') -eq 0 -and (FakeCount 'javaw') -eq 0) 'ambos fechados'
    Ctx; StopLauncher; KillSb

    Write-Host "`n== Caso 4: Minecraft que ignora o quit (CloseMainWindow e depois Kill) =="
    StartLauncher 'mc'
    Click 'BtnPlay'; $null = WaitFor { (BtnName) -like 'Parar*' } 30; Start-Sleep -Milliseconds 800
    Click 'BtnPlay'; $null = WaitFor { Vis 'ConfTitle' } 10; Click 'BtnConfYes'
    Check (WaitFor { (BtnName) -eq 'Jogar' } 120) 'voltou a JOGAR depois de forcar o Minecraft'
    Check ((FakeCount 'eldenring') -eq 0 -and (FakeCount 'javaw') -eq 0) 'ambos fechados'
    Ctx; StopLauncher; KillSb

    Write-Host "`n== Caso 5: reabrir o launcher com os falsos ja rodando (adocao) + 'Fechar tudo' do rodape =="
    StartLauncher
    Click 'BtnPlay'; $null = WaitFor { (BtnName) -like 'Parar*' } 30; Start-Sleep -Milliseconds 800
    StopLauncher     # fecha so o launcher de teste; os falsos continuam
    Start-Sleep -Milliseconds 500
    Check ((FakeCount 'eldenring') -eq 1 -and (FakeCount 'javaw') -eq 1) 'falsos seguem vivos sem o launcher'
    StartLauncher -NoSim
    Check (WaitFor { (BtnName) -like 'Parar*' } 15) 'launcher reaberto reconhece os jogos: PARAR'
    Shot '6-adotado'
    Click 'BtnStop'
    Check (WaitFor { Vis 'ConfTitle' } 10) 'rodape Fechar tudo tambem pede a mesma confirmacao'
    Click 'BtnConfYes'
    Check (WaitFor { (BtnName) -eq 'Jogar' } 60) 'voltou a JOGAR'
    Check ((FakeCount 'eldenring') -eq 0 -and (FakeCount 'javaw') -eq 0) 'ambos fechados'
    Ctx; StopLauncher; KillSb

    Write-Host "`n== Caso 6: um jogo fecha por fora (mostra so o outro) e depois o outro (volta a JOGAR sozinho) =="
    StartLauncher
    Click 'BtnPlay'; $null = WaitFor { (BtnName) -like 'Parar*' } 30; Start-Sleep -Milliseconds 800
    $mc = SbProcs | Where-Object { $_.Name -eq 'javaw' }; Stop-Process -Id $mc.Id -Force
    Check (WaitFor { (Txt 'RunText') -ne $null } 5) 'selo existe'
    $null = WaitFor { (El 'RunText') -and ((El 'RunText').Current.Name -eq 'RODANDO: Elden Ring') } 10
    Check ((El 'RunText').Current.Name -eq 'RODANDO: Elden Ring') 'so o Elden Ring: selo "RODANDO: Elden Ring"'
    Shot '7-so-elden'
    $er = SbProcs | Where-Object { $_.Name -eq 'eldenring' }; Stop-Process -Id $er.Id -Force
    Check (WaitFor { (BtnName) -eq 'Jogar' } 15) 'fechou tudo por fora: voltou a JOGAR sozinho'
    Ctx; StopLauncher; KillSb
}
finally { StopLauncher; KillSb }
Check (GuardOk) 'FINAL: todos os processos reais protegidos seguem vivos'
if ($fail -gt 0) { Write-Host ("`n" + $fail + ' verificacao(oes) falharam.') -ForegroundColor Red; exit 1 } else { Write-Host "`nTodas as verificacoes passaram." -ForegroundColor Green }
