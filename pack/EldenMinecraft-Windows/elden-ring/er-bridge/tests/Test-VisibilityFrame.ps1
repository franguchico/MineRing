[CmdletBinding()]
param(
    [string]$MsvcRoot = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/MSVC',
    [string]$SdkRoot = 'C:/Program Files (x86)/Windows Kits/10',
    [string]$BaselineGameSource
)
$ErrorActionPreference = 'Stop'
$bridgeRoot = Split-Path $PSScriptRoot -Parent
$msvcDirectory = Get-ChildItem -LiteralPath $MsvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
$sdkDirectory = Get-ChildItem -LiteralPath (Join-Path $SdkRoot 'Include') -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (!$msvcDirectory -or !$sdkDirectory) { throw 'Installed MSVC and Windows SDK are required; this script does not install them.' }
$compilerDirectory = Join-Path $msvcDirectory.FullName 'bin/Hostx64/x64'
$compiler = Join-Path $compilerDirectory 'cl.exe'
$sdkVersion = $sdkDirectory.Name
$buildDirectory = Join-Path $PSScriptRoot 'build-visibility-frame'
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
# Abort-only unused engine stubs intentionally produce unreachable-code warnings.
$compilerArguments = @('/nologo', '/std:c++17', '/EHsc', '/MT', '/O2', '/Gy', '/W4', '/wd4702',
    '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', '/D_CRT_SECURE_NO_WARNINGS',
    '/I', (Join-Path $bridgeRoot 'src'), '/I', (Join-Path $bridgeRoot 'include'),
    '/I', (Join-Path $msvcDirectory.FullName 'include'))
foreach ($sdkPart in @('ucrt', 'shared', 'um')) {
    $compilerArguments += @('/I', (Join-Path $sdkDirectory.FullName $sdkPart))
}
$linkArguments = @('/link', '/OPT:REF', 'user32.lib',
    "/LIBPATH:$(Join-Path $msvcDirectory.FullName 'lib/x64')",
    "/LIBPATH:$(Join-Path $SdkRoot "Lib/$sdkVersion/ucrt/x64")",
    "/LIBPATH:$(Join-Path $SdkRoot "Lib/$sdkVersion/um/x64")")
$testNames = @('game_visibility_frame_contract_test', 'game_long_run_contract_test',
    'movement_safety_test', 'action_detour_contract_test')
if ($BaselineGameSource) { $testNames = @('game_visibility_frame_contract_test') }
$previousCompilerPath = $env:PATH
try {
    $env:PATH = "$compilerDirectory;$previousCompilerPath"
    foreach ($testName in $testNames) {
        $outputName = if ($BaselineGameSource) { "$testName-baseline" } else { $testName }
        $executable = Join-Path $buildDirectory "$outputName.exe"
        $arguments = $compilerArguments
        if ($BaselineGameSource) {
            $resolvedSource = (Resolve-Path -LiteralPath $BaselineGameSource).Path.Replace('\', '/')
            $arguments += @('/DERMC_BASELINE_TEST', "/DERMC_GAME_SOURCE=`"$resolvedSource`"")
        }
        $arguments += @("/Fo$(Join-Path $buildDirectory "$outputName.obj")", "/Fe$executable",
            (Join-Path $PSScriptRoot "$testName.cpp"))
        & $compiler @arguments @linkArguments
        if ($LASTEXITCODE) { throw "$testName compilation failed ($LASTEXITCODE)" }
        & $executable
        if ($LASTEXITCODE) { throw "$testName failed ($LASTEXITCODE)" }
    }
} finally {
    $env:PATH = $previousCompilerPath
}
