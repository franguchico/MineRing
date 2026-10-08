<#
Atualiza o dist\stage (que ja tem os binarios da ponte, ignorados pelo git) com o exe e os scripts atuais do repo
e gera o ZIP da Release. Nunca inclui o ZIP do Seamless (nao redistribuivel) nem mods de terceiros.
Uso: powershell -File tools\Make-Dist.ps1 [-Stage <pasta>] [-NoZip] [-Zip <arquivo.zip>]
#>
param([string]$Stage, [switch]$NoZip, [string]$Zip)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $Stage) { $Stage = Join-Path $repo 'dist\stage' }
$Stage = [IO.Path]::GetFullPath($Stage)
if (-not $Zip) { $Zip = Join-Path $repo 'dist\MineRing-Launcher.zip' }
$bin = 'pack\EldenMinecraft-Windows\elden-ring\er-bridge\build-msvc\Release\erbridge_core.dll'
if (-not (Test-Path -LiteralPath (Join-Path $Stage $bin))) { throw ('O stage nao tem os binarios da ponte (' + $bin + '). Preciso de um dist\stage anterior ou do build MSVC.') }
function Sync([string]$Rel) {
    $from = Join-Path $repo $Rel; $to = Join-Path $Stage $Rel
    $null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to)
    Copy-Item -LiteralPath $from -Destination $to -Force
}
foreach ($f in 'MineRing-Launcher.exe', 'README.md', 'THIRD-PARTY-NOTICES.txt', 'LICENSE') { Sync $f }
foreach ($d in 'pack\launcher', 'pack\scripts', 'pack\EldenMinecraft-Windows\elden-ring\windows') {
    foreach ($f in Get-ChildItem -LiteralPath (Join-Path $repo $d) -Recurse -File) { Sync ($f.FullName.Substring($repo.Length + 1)) }
}
Write-Output ('Stage atualizado: ' + $Stage)
if (-not $NoZip) {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (Test-Path -LiteralPath $Zip) { Remove-Item -LiteralPath $Zip -Force }
    # entradas com '/' (o CreateFromDirectory do Windows PowerShell 5.1 grava barra invertida, que o unzip e outros extratores estranham)
    $fs = [IO.File]::Create($Zip)
    $za = New-Object IO.Compression.ZipArchive($fs, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in Get-ChildItem -LiteralPath $Stage -Recurse -File) {
            $name = $f.FullName.Substring($Stage.Length).TrimStart([char]92, [char]47).Replace([string][char]92, '/')
            $null = [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f.FullName, $name, [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $za.Dispose(); $fs.Dispose() }
    Write-Output ('ZIP: ' + $Zip + ' (' + (Get-Item -LiteralPath $Zip).Length + ' bytes)')
}
