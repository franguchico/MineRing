param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build-msvc'),
    [string]$Java21Home = 'C:\Program Files\Microsoft\jdk-21.0.10.7-hotspot'
)
$ErrorActionPreference = 'Stop'
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
$classes = Join-Path $BuildDir 'java-probe'
$link = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\mc-bridge\src\main\java\dev\ermc\bridge\link'))
$sources = @('BridgePaths.java','Protocol.java','BridgeShm.java','GameState.java','ControlState.java','EntityInfo.java') |
    ForEach-Object { Join-Path $link $_ }
$sources += Join-Path $PSScriptRoot 'IpcClientProbe.java'
$null = New-Item -ItemType Directory -Path $classes -Force
& (Join-Path $Java21Home 'bin\javac.exe') -encoding UTF-8 --release 21 -d $classes @sources
if ($LASTEXITCODE -ne 0) { throw 'IPC Java client compilation failed' }
$previousIpc = $env:ERMC_DIR
$previousMarker = $env:ERBRIDGE
$nativeProcess = $null
try {
    $testRoot = Join-Path $BuildDir ('test-run-' + [Guid]::NewGuid().ToString('N'))
    $env:ERMC_DIR = Join-Path $testRoot ('ipc-verifica' + [char]0xE7 + [char]0xE3 + 'o')
    $null = New-Item -ItemType Directory -Path $env:ERMC_DIR -Force
    $out = Join-Path $testRoot 'host.out'
    $err = Join-Path $testRoot 'host.err'
    $nativeProcess = Start-Process -FilePath (Join-Path $BuildDir 'Release\ermc_ipc_probe.exe') -WindowStyle Hidden `
        -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    $ready = $false
    for ($i=0; $i -lt 100; $i++) {
        if ((Test-Path -LiteralPath $out) -and ((Get-Content -LiteralPath $out -Raw) -match 'HOST_READY')) { $ready=$true; break }
        if ($nativeProcess.HasExited) { break }
        Start-Sleep -Milliseconds 50
    }
    if (-not $ready) { throw 'Synthetic native host did not become ready' }
    & (Join-Path $Java21Home 'bin\java.exe') -cp $classes IpcClientProbe
    if ($LASTEXITCODE -ne 0) { throw 'Java/C++ IPC test failed' }
    if (-not $nativeProcess.WaitForExit(15000) -or $nativeProcess.ExitCode -ne 0) { throw 'Synthetic native host failed' }
    Get-Content -LiteralPath $out
    $guard = Join-Path $BuildDir 'guard-test\Release'
    foreach ($mode in @('passive','eac')) {
        $env:ERMC_DIR = Join-Path $testRoot ('guard-' + $mode)
        & (Join-Path $guard 'eldenring.exe') (Join-Path $BuildDir 'Release\dinput8.dll') $mode (Join-Path $guard 'EasyAntiCheat_EOS.dll')
        if ($LASTEXITCODE -ne 0) { throw "Loader guard failed: $mode" }
    }
} finally {
    if ($nativeProcess) {
        if (-not $nativeProcess.HasExited) { $nativeProcess.Kill(); $nativeProcess.WaitForExit(5000) | Out-Null }
        $nativeProcess.Dispose()
    }
    $env:ERMC_DIR = $previousIpc
    $env:ERBRIDGE = $previousMarker
}
