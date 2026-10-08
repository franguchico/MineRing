<#
.SYNOPSIS
Returns one read-only snapshot of Windows/Steam install, progress, version and package readiness.
.DESCRIPTION
Works before the game exists or while Steam is downloading it. Reads only filesystem Steam library/manifest data
and basic process/session information. Never polls, starts a process, accesses account credentials or writes files.
.EXAMPLE
.\Diagnose-Bridge.ps1 -AsJson
.EXAMPLE
.\Diagnose-Bridge.ps1 -SteamPath 'D:\SteamLibrary' -GameDir 'D:\SteamLibrary\steamapps\common\ELDEN RING\Game'
#>
[CmdletBinding()]
param([string]$GameDir, [string]$BinaryDir, [string[]]$SteamPath, [switch]$AsJson)
. (Join-Path $PSScriptRoot 'Bridge.Common.ps1')
$issues = @()
$libraries = @(); $installs = @(); $binaries = @()
try { $libraries = @(Get-BridgeSteamLibraries $SteamPath); $installs = @(Get-BridgeInstallations $SteamPath) }
catch { $issues += $_.Exception.Message }
$explicit = $null
if ($GameDir) {
    try {
        $path = Get-BridgeFullPath $GameDir
        Assert-BridgeNoLinks $path
        $exe = Join-Path $path 'eldenring.exe'
        $present = Test-Path -LiteralPath $exe -PathType Leaf
        $version = $null; if ($present) { $version = Get-BridgeVersion $exe }
        $explicit = [pscustomobject]@{ GameDir = $path; ExePresent = $present; FileVersion = $version; VersionSupported = ($version -eq '2.7.1.0') }
    } catch { $issues += $_.Exception.Message }
}
try {
    $binary = Get-BridgeBinaryDir $BinaryDir
    foreach ($name in @('dinput8.dll','erbridge_core.dll')) {
        $path = Join-Path $binary $name
        try { $pe = Get-BridgePE $path; $binaries += [pscustomobject]@{ Name = $name; Present = $true; X64PE = $true; Sha256 = $pe.Sha256 } }
        catch { $binaries += [pscustomobject]@{ Name = $name; Present = (Test-Path -LiteralPath $path -PathType Leaf); X64PE = $false; Issue = $_.Exception.Message } }
    }
} catch { $issues += $_.Exception.Message }
$sessionId = $null
try { $sessionId = [Diagnostics.Process]::GetCurrentProcess().SessionId } catch { $issues += 'Session ID unavailable.' }
$result = [pscustomobject]@{
    SnapshotUtc = [DateTime]::UtcNow.ToString('o'); PowerShell = $PSVersionTable.PSVersion.ToString()
    NativeWindows = ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT)
    OS64Bit = [Environment]::Is64BitOperatingSystem; Process64Bit = [Environment]::Is64BitProcess; SessionId = $sessionId
    SteamRunning = (@(Get-Process -Name steam -ErrorAction SilentlyContinue).Count -gt 0)
    GameRunning = (@(Get-Process -Name eldenring,start_protected_game -ErrorAction SilentlyContinue).Count -gt 0)
    SteamLibraries = $libraries; Installations = $installs; ExplicitGame = $explicit
    ExpectedFileVersion = '2.7.1.0'; Binaries = $binaries; Issues = $issues
    RuntimeStatus = 'Experimental; Windows D3D12/game runtime verification pending'
}
if ($AsJson) { $result | ConvertTo-Json -Depth 8 } else { $result }
