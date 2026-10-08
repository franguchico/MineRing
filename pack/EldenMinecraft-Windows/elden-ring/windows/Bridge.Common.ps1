# Shared, read-only on import. Compatible with Windows PowerShell 5.1 and PowerShell 7.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:BridgeAppId = '1245620'
$script:BridgeExpectedVersion = '2.7.1.0'
$script:BridgePackageRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))

function Get-BridgeFullPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw 'An explicit filesystem path is required.' }
    $resolved = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    $full = [IO.Path]::GetFullPath($resolved)
    if ($full -eq [IO.Path]::GetPathRoot($full)) { return $full }
    return $full.TrimEnd('\')
}

function Test-BridgeInside([string]$Path, [string]$Root) {
    $p = Get-BridgeFullPath $Path
    $r = Get-BridgeFullPath $Root
    return $p -ne $r -and $p.StartsWith($r.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)
}

function Assert-BridgeInside([string]$Path, [string]$Root) {
    if (-not (Test-BridgeInside $Path $Root)) { throw "Path must remain strictly inside its root: $Path (root: $Root)" }
}

function Assert-BridgeNoLinks([string]$Path) {
    $p = Get-BridgeFullPath $Path
    while ($p) {
        if (Test-Path -LiteralPath $p) {
            $item = Get-Item -LiteralPath $p -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Reparse points/symlinks/junctions are not supported: $p"
            }
            if ($item.PSObject.Properties['LinkType'] -and $item.LinkType -eq 'HardLink') {
                throw "Hard-linked files are not supported: $p"
            }
        }
        $parent = [IO.Directory]::GetParent($p)
        if ($null -eq $parent) { break }
        $p = $parent.FullName
    }
}

function Assert-BridgeWindows {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT -or -not [Environment]::Is64BitOperatingSystem) {
        throw 'These scripts require native 64-bit Windows.'
    }
}

function Assert-BridgeStopped {
    $running = @(Get-Process -Name eldenring,start_protected_game -ErrorAction SilentlyContinue)
    if ($running.Count) { throw 'Elden Ring or start_protected_game is running. Close the game before continuing.' }
}

function Get-BridgeHash([string]$Path) {
    Assert-BridgeNoLinks $Path
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "File not found: $Path" }
    # Windows PowerShell 5.1's Get-FileHash helper inherits WhatIf and may skip
    # its internal ProviderPath enumeration. Hashing is read-only even in preflight.
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [IO.File]::OpenRead($Path)
        return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    } finally {
        if ($stream) { $stream.Dispose() }
        $algorithm.Dispose()
    }
}

function Get-BridgeFileState([string]$Path) {
    Assert-BridgeNoLinks $Path
    $parent = Split-Path -Parent $Path
    while ($parent) {
        if (Test-Path -LiteralPath $parent) {
            if (-not (Test-Path -LiteralPath $parent -PathType Container)) { throw "Parent is not a directory: $parent" }
            break
        }
        $parent = Split-Path -Parent $parent
    }
    if (Test-Path -LiteralPath $Path) {
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Expected a regular file: $Path" }
        return Get-BridgeHash $Path
    }
    return $null
}

function Assert-BridgeFileState([string]$Path, $ExpectedHash) {
    $actual = Get-BridgeFileState $Path
    if ($actual -ne $ExpectedHash) { throw "File changed; refusing to overwrite/remove: $Path" }
}

function Get-BridgePE([string]$Path, [bool]$IsDll = $true) {
    Assert-BridgeNoLinks $Path
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $reader = New-Object IO.BinaryReader($stream)
    try {
        if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5a4d) { throw 'Missing DOS MZ header.' }
        $stream.Position = 0x3c
        $offset = $reader.ReadInt32()
        if ($offset -lt 64 -or $offset -gt ($stream.Length - 24)) { throw 'Invalid PE offset.' }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x00004550) { throw 'Missing PE signature.' }
        if ($reader.ReadUInt16() -ne 0x8664) { throw 'Expected AMD64/x64 machine (0x8664).' }
        $sections = $reader.ReadUInt16()
        $stream.Position = $offset + 20
        $optionalSize = $reader.ReadUInt16()
        $flags = $reader.ReadUInt16()
        if ($optionalSize -lt 112 -or $sections -lt 1 -or ($offset + 24 + $optionalSize + 40 * $sections) -gt $stream.Length) {
            throw 'Truncated PE headers/section table.'
        }
        if ($reader.ReadUInt16() -ne 0x20b) { throw 'Expected PE32+ optional header.' }
        if ((($flags -band 0x2000) -ne 0) -ne $IsDll) { throw 'PE DLL/executable type mismatch.' }
        if (($flags -band 0x0002) -eq 0) { throw 'PE is not an executable image.' }
    } catch { throw "Invalid x64 PE '$Path': $($_.Exception.Message)" }
    finally { $reader.Dispose(); $stream.Dispose() }
    return [pscustomobject]@{ Path = $Path; Machine = 'AMD64'; Format = 'PE32+'; Sha256 = (Get-BridgeHash $Path) }
}

function Get-BridgeVersion([string]$Exe) {
    Assert-BridgeNoLinks $Exe
    return [Diagnostics.FileVersionInfo]::GetVersionInfo($Exe).FileVersion
}

# Parse only filesystem configuration, never loginusers.vdf, config accounts or credentials.
function Read-BridgeVdf([string]$Path) {
    Assert-BridgeNoLinks $Path
    $tokens = @([regex]::Matches([IO.File]::ReadAllText($Path), '"(?:\\.|[^"\\])*"|[{}]|//[^\r\n]*') |
        Where-Object { -not $_.Value.StartsWith('//') } | ForEach-Object { $_.Value })
    $cursor = @{ Index = 0 }
    function Read-BridgeVdfObject($Tokens, $Cursor, [bool]$Nested) {
        $result = @{}
        while ($Cursor.Index -lt $Tokens.Count) {
            $key = $Tokens[$Cursor.Index]; $Cursor.Index++
            if ($key -eq '}') {
                if (-not $Nested) { throw 'Unexpected closing brace in VDF.' }
                return $result
            }
            if (-not $key.StartsWith('"') -or $Cursor.Index -ge $Tokens.Count) { throw 'Invalid VDF key/value.' }
            $key = $key.Substring(1, $key.Length - 2).Replace('\\', '\').Replace('\"', '"')
            $value = $Tokens[$Cursor.Index]; $Cursor.Index++
            if ($value -eq '{') { $value = Read-BridgeVdfObject $Tokens $Cursor $true }
            elseif ($value.StartsWith('"')) { $value = $value.Substring(1, $value.Length - 2).Replace('\\', '\').Replace('\"', '"') }
            else { throw 'Invalid VDF value.' }
            if ($result.ContainsKey($key)) { throw "Duplicate VDF key: $key" }
            $result[$key] = $value
        }
        if ($Nested) { throw 'Unclosed VDF object.' }
        return $result
    }
    return Read-BridgeVdfObject $tokens $cursor $false
}

function Get-BridgeSteamLibraries([string[]]$SteamPath) {
    $roots = @($SteamPath | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if (-not $roots.Count) {
        $roots = @()
        foreach ($spec in @(
            @{ Hive = [Microsoft.Win32.Registry]::CurrentUser; Key = 'Software\Valve\Steam'; Value = 'SteamPath' },
            @{ Hive = [Microsoft.Win32.Registry]::LocalMachine; Key = 'SOFTWARE\WOW6432Node\Valve\Steam'; Value = 'InstallPath' },
            @{ Hive = [Microsoft.Win32.Registry]::LocalMachine; Key = 'SOFTWARE\Valve\Steam'; Value = 'InstallPath' }
        )) {
            $key = $spec.Hive.OpenSubKey($spec.Key)
            if ($null -ne $key) {
                try { $value = $key.GetValue($spec.Value); if ($value) { $roots += [string]$value } }
                finally { $key.Dispose() }
            }
        }
        if (${env:ProgramFiles(x86)}) { $roots += Join-Path ${env:ProgramFiles(x86)} 'Steam' }
    }
    $libraries = @()
    foreach ($root in ($roots | Select-Object -Unique)) {
        if (-not $root) { continue }
        $root = Get-BridgeFullPath $root
        Assert-BridgeNoLinks $root
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        $libraries += $root
        foreach ($relative in @('steamapps\libraryfolders.vdf', 'config\libraryfolders.vdf')) {
            $vdf = Join-Path $root $relative
            if (-not (Test-Path -LiteralPath $vdf -PathType Leaf)) { continue }
            $parsed = Read-BridgeVdf $vdf
            if (-not $parsed.ContainsKey('libraryfolders')) { continue }
            foreach ($entry in $parsed['libraryfolders'].GetEnumerator()) {
                if ($entry.Key -notmatch '^\d+$') { continue }
                $value = $entry.Value
                if ($value -is [System.Collections.IDictionary]) { $value = $value['path'] }
                if ($value) {
                    $path = Get-BridgeFullPath ([string]$value)
                    Assert-BridgeNoLinks $path
                    if (Test-Path -LiteralPath $path -PathType Container) { $libraries += $path }
                }
            }
        }
    }
    return $libraries | Sort-Object -Unique
}

function Get-BridgeInstallations([string[]]$SteamPath) {
    foreach ($library in @(Get-BridgeSteamLibraries $SteamPath)) {
        $manifest = Join-Path $library 'steamapps\appmanifest_1245620.acf'
        if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) { continue }
        $vdf = Read-BridgeVdf $manifest
        if (-not $vdf.ContainsKey('AppState')) { throw "Missing AppState in $manifest" }
        $state = $vdf['AppState']
        if ($state['appid'] -ne $script:BridgeAppId) { throw "Unexpected Steam AppID in $manifest" }
        $common = Join-Path $library 'steamapps\common'
        $installDir = [string]$state['installdir']
        if (-not $installDir -or [IO.Path]::IsPathRooted($installDir)) { throw "Invalid installdir in $manifest" }
        $game = Get-BridgeFullPath (Join-Path (Join-Path $common $installDir) 'Game')
        Assert-BridgeInside $game $common
        Assert-BridgeNoLinks $game
        $exe = Join-Path $game 'eldenring.exe'
        $progress = [ordered]@{}
        foreach ($field in @('BytesDownloaded','BytesToDownload','BytesStaged','BytesToStage','SizeOnDisk')) {
            if ($state.ContainsKey($field) -and [string]$state[$field] -match '^\d+$') { $progress[$field] = [string]$state[$field] }
        }
        $version = $null
        if (Test-Path -LiteralPath $exe -PathType Leaf) { $version = Get-BridgeVersion $exe }
        [pscustomobject]@{
            Library = $library; ManifestPath = $manifest; GameDir = $game
            StateFlags = [string]$state['StateFlags']; Progress = $progress
            ExePresent = (Test-Path -LiteralPath $exe -PathType Leaf); FileVersion = $version
            Ready = ([string]$state['StateFlags'] -eq '4' -and (Test-Path -LiteralPath $exe -PathType Leaf))
        }
    }
}

function Get-BridgeGame([string]$GameDir, [string[]]$SteamPath) {
    # Validate an explicit folder first, so unsupported versions fail before any other preflight.
    if ($GameDir) {
        $game = Get-BridgeFullPath $GameDir
        $exe = Join-Path $game 'eldenring.exe'
        if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "eldenring.exe not found: $game" }
        $version = Get-BridgeVersion $exe
        if ($version -ne $script:BridgeExpectedVersion) { throw "Unsupported eldenring.exe FileVersion '$version'; expected 2.7.1.0." }
    }
    $installs = @(Get-BridgeInstallations $SteamPath)
    if ($GameDir) { $installs = @($installs | Where-Object { $_.GameDir -eq $game }) }
    if ($installs.Count -ne 1) { throw 'Select exactly one Steam appmanifest_1245620.acf installation; use -GameDir and/or -SteamPath.' }
    $install = $installs[0]
    if (-not $install.Ready) { throw "Steam installation is incomplete/updating (StateFlags=$($install.StateFlags)). Run Diagnose-Bridge.ps1 for a snapshot." }
    $exe = Join-Path $install.GameDir 'eldenring.exe'
    if ($install.FileVersion -ne $script:BridgeExpectedVersion) { throw "Unsupported eldenring.exe FileVersion '$($install.FileVersion)'; expected 2.7.1.0." }
    $null = Get-BridgePE $exe $false
    return $install
}

function Get-BridgeBinaryDir([string]$BinaryDir) {
    if (-not $BinaryDir) { $BinaryDir = Join-Path $script:BridgePackageRoot 'elden-ring\er-bridge\build-msvc\Release' }
    $path = Get-BridgeFullPath $BinaryDir
    Assert-BridgeInside $path $script:BridgePackageRoot
    Assert-BridgeNoLinks $path
    if ((Test-Path -LiteralPath $path) -and -not (Test-Path -LiteralPath $path -PathType Container)) { throw 'BinaryDir must be a directory.' }
    return $path
}

function Get-BridgeBackupDir([string]$BackupDir, [string]$GameDir) {
    if (-not $BackupDir) { $BackupDir = Join-Path $script:BridgePackageRoot 'bridge-backups' }
    $path = Get-BridgeFullPath $BackupDir
    Assert-BridgeInside $path $script:BridgePackageRoot
    if ($GameDir -and ($path -eq $GameDir -or (Test-BridgeInside $path $GameDir) -or (Test-BridgeInside $GameDir $path))) {
        throw 'BackupDir and GameDir must not contain one another.'
    }
    Assert-BridgeNoLinks $path
    if ((Test-Path -LiteralPath $path) -and -not (Test-Path -LiteralPath $path -PathType Container)) { throw 'BackupDir must be a directory.' }
    return $path
}

function Get-BridgeReShadePaths {
    return @('dxgi.dll', 'ReShade.ini', 'ErmcDepthPreset.ini', 'ermc-shaders\ErmcDepth.fx', 'ErmcDepth.addon64')
}

function Get-BridgeReShadeSpecs([string]$ReShadeDir, [string]$GameDir) {
    $root = Get-BridgeFullPath $ReShadeDir
    Assert-BridgeNoLinks $root
    if (-not (Test-Path -LiteralPath $root -PathType Container)) { throw 'ReShadeDir must be an existing prepared directory.' }
    if ($root -eq $GameDir -or (Test-BridgeInside $root $GameDir)) { throw 'ReShadeDir must be outside the game directory.' }
    # Deliberately do not enumerate/copy other files or fetch third-party binaries.
    foreach ($name in @(Get-BridgeReShadePaths)) {
        $source = Join-Path $root $name
        Assert-BridgeInside $source $root
        $hash = Get-BridgeHash $source
        if ($name -cin @('dxgi.dll', 'ErmcDepth.addon64')) { $hash = (Get-BridgePE $source).Sha256 }
        @{ Name = $name; Source = $source; Hash = $hash }
    }
}

function Write-BridgeCheckedFile([string]$Target, [string]$Source, $ExpectedHash, [string]$NewHash, [string]$Root) {
    Assert-BridgeInside $Target $Root
    Assert-BridgeNoLinks $Target
    Assert-BridgeFileState $Target $ExpectedHash
    $parent = Split-Path -Parent $Target
    $null = [IO.Directory]::CreateDirectory($parent)
    $stage = Join-Path $parent ('.erbridge-' + [guid]::NewGuid().ToString('N') + '.tmp')
    Assert-BridgeInside $stage $Root
    try {
        [IO.File]::Copy($Source, $stage, $false)
        if ((Get-BridgeHash $stage) -ne $NewHash) { throw "Source changed or copy failed: $Source" }
        Assert-BridgeStopped
        Assert-BridgeNoLinks $Target
        Assert-BridgeFileState $Target $ExpectedHash
        if ($null -eq $ExpectedHash) { [IO.File]::Move($stage, $Target) }
        else { [IO.File]::Replace($stage, $Target, [NullString]::Value) }
        if ((Get-BridgeHash $Target) -ne $NewHash) { throw "Destination verification failed: $Target" }
    } finally {
        Assert-BridgeInside $stage $Root
        Assert-BridgeNoLinks $stage
        if (Test-Path -LiteralPath $stage -PathType Leaf) { [IO.File]::Delete($stage) }
    }
}

function Save-BridgeManifest($Manifest, [string]$Path, [string]$BackupRoot) {
    Assert-BridgeInside $Path $BackupRoot
    Assert-BridgeNoLinks $Path
    $stage = Join-Path (Split-Path -Parent $Path) ('.manifest-' + [guid]::NewGuid().ToString('N') + '.tmp')
    Assert-BridgeInside $stage $BackupRoot
    try {
        [IO.File]::WriteAllText($stage, ($Manifest | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding($false)))
        if (Test-Path -LiteralPath $Path -PathType Leaf) { [IO.File]::Replace($stage, $Path, [NullString]::Value) }
        else { [IO.File]::Move($stage, $Path) }
    } finally {
        Assert-BridgeInside $stage $BackupRoot
        Assert-BridgeNoLinks $stage
        if (Test-Path -LiteralPath $stage -PathType Leaf) { [IO.File]::Delete($stage) }
    }
}

function Read-BridgeManifest([string]$Path, [string]$BackupRoot, [string]$GameDir) {
    $path = Get-BridgeFullPath $Path
    Assert-BridgeInside $path $BackupRoot
    Assert-BridgeNoLinks $path
    $m = [IO.File]::ReadAllText($path) | ConvertFrom-Json
    if ($m.SchemaVersion -notin @(1, 2) -or $m.AppId -ne $script:BridgeAppId -or $m.GameDir -ne $GameDir) { throw "Manifest does not match this Steam game: $path" }
    if ($m.Status -notin @('Installing','Installed','Failed','Removed','RolledBack')) { throw 'Invalid manifest status.' }
    $expected = @('dinput8.dll','erbridge\erbridge_core.dll','steam_appid.txt')
    if ($m.SchemaVersion -eq 2) { $expected += @(Get-BridgeReShadePaths) }
    if (@($m.Entries).Count -ne $expected.Count) { throw "Expected exactly $($expected.Count) owned files in manifest." }
    foreach ($name in $expected) {
        $entries = @($m.Entries | Where-Object { $_.RelativePath -ceq $name })
        if ($entries.Count -ne 1) { throw "Missing/duplicate manifest entry: $name" }
        $entry = $entries[0]
        if ($entry.InstalledSha256 -notmatch '^[a-fA-F0-9]{64}$' -or $entry.OriginalExists -isnot [bool]) { throw 'Invalid manifest hash/exists field.' }
        if ($entry.BackupName -cne ('original-' + [array]::IndexOf($expected, $name) + '.bin')) { throw 'Unexpected backup filename.' }
        if ($entry.OriginalExists -and $entry.OriginalSha256 -notmatch '^[a-fA-F0-9]{64}$') { throw 'Invalid original hash.' }
        if (-not $entry.OriginalExists -and $null -ne $entry.OriginalSha256) { throw 'Absent file must have a null original hash.' }
        if ($name -cin @(Get-BridgeReShadePaths) -and $entry.OriginalExists) { throw 'ReShade auxiliary files must not replace pre-existing files.' }
        Assert-BridgeInside (Join-Path $GameDir $name) $GameDir
    }
    return $m
}

function Find-BridgeManifest([string]$BackupRoot, [string]$GameDir) {
    if (-not (Test-Path -LiteralPath $BackupRoot -PathType Container)) { return $null }
    foreach ($dir in @(Get-ChildItem -LiteralPath $BackupRoot -Directory | Sort-Object Name -Descending)) {
        Assert-BridgeNoLinks $dir.FullName
        $path = Join-Path $dir.FullName 'manifest.json'
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        Assert-BridgeNoLinks $path
        $header = [IO.File]::ReadAllText($path) | ConvertFrom-Json
        if ($header.GameDir -ne $GameDir) { continue }
        $manifest = Read-BridgeManifest $path $BackupRoot $GameDir
        if ($manifest.Status -in @('Installing','Installed','Failed')) {
            return [pscustomobject]@{ Path = $path; Manifest = $manifest }
        }
    }
    return $null
}

function Test-BridgeGeneratedConfig($Manifest, $Entry) {
    return ($Manifest.SchemaVersion -eq 2 -and -not $Entry.OriginalExists -and
        $Entry.RelativePath -cin @('ReShade.ini', 'ErmcDepthPreset.ini'))
}

function Assert-BridgeInstalledFiles($Manifest) {
    foreach ($entry in $Manifest.Entries) {
        $target = Join-Path $Manifest.GameDir $entry.RelativePath
        Assert-BridgeInside $target $Manifest.GameDir
        if (Test-BridgeGeneratedConfig $Manifest $entry) {
            # Get-BridgeFileState rejects links/directories; missing generated configs also fail.
            if ($null -eq (Get-BridgeFileState $target)) { throw "Tracked generated config is missing: $target" }
        } else { Assert-BridgeFileState $target $entry.InstalledSha256 }
    }
}

function Get-BridgeRestorePlan($Manifest, [string]$ManifestPath, [switch]$PreserveGeneratedConfigs) {
    $plan = @()
    foreach ($entry in $Manifest.Entries) {
        $target = Join-Path $Manifest.GameDir $entry.RelativePath
        Assert-BridgeInside $target $Manifest.GameDir
        $actual = Get-BridgeFileState $target
        $original = $null
        if ($entry.OriginalExists) { $original = $entry.OriginalSha256 }
        $source = Join-Path (Split-Path -Parent $ManifestPath) $entry.BackupName
        Assert-BridgeInside $source (Split-Path -Parent $ManifestPath)
        if ($entry.OriginalExists -and (Get-BridgeHash $source) -ne $original) { throw "Backup hash mismatch: $source" }
        if ($actual -eq $original) { continue } # Already restored, or install never reached this file.
        $preserve = $null
        if ($actual -ne $entry.InstalledSha256) {
            # This exception is fixed by schema and exact path, never a manifest-supplied wildcard/policy.
            if ($PreserveGeneratedConfigs -and $actual -and (Test-BridgeGeneratedConfig $Manifest $entry)) {
                $session = Split-Path -Parent $ManifestPath
                $preserve = Join-Path $session ('preserved-' + $entry.RelativePath + '-' + $actual + '.bin')
                Assert-BridgeInside $preserve $session
                $saved = Get-BridgeFileState $preserve
                if ($saved -and $saved -ne $actual) { throw "Preserved config backup hash mismatch: $preserve" }
            } else { throw "File changed since install; removal refused before any writes: $target" }
        }
        $plan += [pscustomobject]@{ Target = $target; Source = $source; CurrentHash = $actual; OriginalHash = $original; PreservePath = $preserve }
    }
    return $plan
}

function Invoke-BridgeRestore($Plan, [string]$GameDir) {
    # Validate the complete plan again, then preserve ALL changed configs before any game write.
    foreach ($action in $Plan) {
        Assert-BridgeInside $action.Target $GameDir
        Assert-BridgeFileState $action.Target $action.CurrentHash
        if ($action.OriginalHash) { Assert-BridgeFileState $action.Source $action.OriginalHash }
    }
    foreach ($action in $Plan) {
        if ($action.PreservePath) {
            $saved = Get-BridgeFileState $action.PreservePath
            if ($saved) { Assert-BridgeFileState $action.PreservePath $action.CurrentHash }
            else {
                Write-BridgeCheckedFile $action.PreservePath $action.Target $null $action.CurrentHash (Split-Path -Parent $action.PreservePath)
            }
        }
    }
    foreach ($action in $Plan) { Assert-BridgeFileState $action.Target $action.CurrentHash }
    foreach ($action in $Plan) {
        Assert-BridgeStopped
        Assert-BridgeInside $action.Target $GameDir
        Assert-BridgeNoLinks $action.Target
        if ($action.PreservePath) { Assert-BridgeFileState $action.PreservePath $action.CurrentHash }
        if ($null -eq $action.OriginalHash) {
            Assert-BridgeFileState $action.Target $action.CurrentHash
            [IO.File]::Delete($action.Target) # One allowlisted file; never recursive deletion.
        } else {
            Write-BridgeCheckedFile $action.Target $action.Source $action.CurrentHash $action.OriginalHash $GameDir
        }
    }
    # Keep erbridge/loaded and directories: runtime copies are not installer-owned files.
}

function Get-BridgeSaveFiles {
    if (-not $env:APPDATA) { throw 'APPDATA is missing; cannot validate save backup location.' }
    $root = Get-BridgeFullPath (Join-Path $env:APPDATA 'EldenRing')
    Assert-BridgeNoLinks $root
    if (-not (Test-Path -LiteralPath $root -PathType Container)) { return }
    # Enumerate only directories and .sl2/.bak save files, never account/login configuration.
    $dirs = @($root) + @(Get-ChildItem -LiteralPath $root -Directory | ForEach-Object { $_.FullName })
    foreach ($dir in $dirs) {
        Assert-BridgeNoLinks $dir
        foreach ($file in @(Get-ChildItem -LiteralPath $dir -File | Where-Object { $_.Extension -in @('.sl2','.bak') })) {
            Assert-BridgeInside $file.FullName $root
            Assert-BridgeNoLinks $file.FullName
            [pscustomobject]@{ Source = $file.FullName; RelativePath = $file.FullName.Substring($root.Length + 1); Sha256 = (Get-BridgeHash $file.FullName) }
        }
    }
}
