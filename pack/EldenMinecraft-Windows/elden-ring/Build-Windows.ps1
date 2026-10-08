param(
    [Parameter(Mandatory=$true)][string]$Java25Home,
    [string]$Java21Home = 'C:\Program Files\Microsoft\jdk-21.0.10.7-hotspot',
    [string]$CMake = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
    [string]$Generator = 'Visual Studio 18 2026',
    [switch]$NativeOnly
)
$ErrorActionPreference = 'Stop'
function Invoke-BridgeCMake([string[]]$Arguments) {
    $taskInfo = New-Object Diagnostics.ProcessStartInfo
    $taskInfo.FileName = $CMake
    $taskInfo.UseShellExecute = $false
    $taskInfo.RedirectStandardOutput = $true
    $taskInfo.RedirectStandardError = $true
    # Windows environment names are case-insensitive, but MSBuild's inherited
    # environment parser can reject PATH/path duplicates from a packaged host.
    $taskInfo.EnvironmentVariables.Clear()
    foreach ($taskEntry in [Environment]::GetEnvironmentVariables().GetEnumerator()) {
        $taskInfo.EnvironmentVariables[$taskEntry.Key.ToString().ToUpperInvariant()] = $taskEntry.Value.ToString()
    }
    # Quote for the Windows native argument parser, not for a shell.
    $taskInfo.Arguments = (@($Arguments | ForEach-Object {
        '"' + ($_ -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
    }) -join ' ')
    $taskProcess = New-Object Diagnostics.Process
    $taskProcess.StartInfo = $taskInfo
    try {
        $null = $taskProcess.Start()
        $taskStdout = $taskProcess.StandardOutput.ReadToEndAsync()
        $taskStderr = $taskProcess.StandardError.ReadToEndAsync()
        $taskProcess.WaitForExit()
        Write-Host ($taskStdout.GetAwaiter().GetResult())
        $taskErrorOutput = $taskStderr.GetAwaiter().GetResult()
        if ($taskErrorOutput) { Write-Host $taskErrorOutput }
        if ($taskProcess.ExitCode -ne 0) { throw "CMake failed with exit code $($taskProcess.ExitCode)." }
    } finally { $taskProcess.Dispose() }
}
foreach ($path in @($CMake,(Join-Path $Java25Home 'bin\java.exe'),(Join-Path $Java21Home 'bin\javac.exe'))) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Build dependency missing: $path" }
}
$oldJavaHome = $env:JAVA_HOME
$oldJava21Home = $env:JAVA21_HOME
$oldGradleHome = $env:GRADLE_USER_HOME
try {
    $native = Join-Path $PSScriptRoot 'er-bridge'
    $build = Join-Path $native 'build-msvc'
    Invoke-BridgeCMake -Arguments @('-S', $native, '-B', $build, '-G', $Generator, '-A', 'x64', '-T', 'v145,host=x64')
    Invoke-BridgeCMake -Arguments @('--build', $build, '--config', 'Release', '--parallel', '1', '--', '/nr:false')
    if ($NativeOnly) { return }
    $env:JAVA_HOME = [IO.Path]::GetFullPath($Java25Home)
    $env:JAVA21_HOME = [IO.Path]::GetFullPath($Java21Home)
    $env:GRADLE_USER_HOME = Join-Path $PSScriptRoot '.gradle-cache'
    Push-Location (Join-Path $PSScriptRoot 'mc-bridge')
    try {
        & '.\gradlew.bat' --no-daemon --no-configuration-cache build
        if ($LASTEXITCODE -ne 0) { throw 'Fabric mod compilation failed' }
    } finally { Pop-Location }
} finally {
    $env:JAVA_HOME = $oldJavaHome
    $env:JAVA21_HOME = $oldJava21Home
    $env:GRADLE_USER_HOME = $oldGradleHome
}
