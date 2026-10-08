<#
.SYNOPSIS
Sets WINDOW mode and preserves supported saved window dimensions, with a verified original backup.
.DESCRIPTION
Only edits ScreenMode and Resolution-WindowScreenWidth/Height in the existing GraphicsConfig.xml.
Preserves every other character, encoding, BOM presence, fullscreen/borderless values and settings.
Requires the game and Seamless launcher closed. Does not set or clear ReadOnly attributes.
DTD/external entities, duplicate fields, missing config, links and unexpected paths are refused.
Uses an atomic replacement and hash-guarded rollback; original bytes remain in bridge-backups/display.
.PARAMETER ConfigPath
Defaults to APPDATA/EldenRing/GraphicsConfig.xml. An explicit path must identify that same file.
.PARAMETER BackupDir
Optional package-local backup directory. Defaults to bridge-backups/display.
.PARAMETER ValidateOnly
Checks the entire edit without creating directories or files; used for launcher preflight.
.PARAMETER Width
Optional explicit window width, supplied together with Height, up to 2560x1440.
When both are omitted, preserve supported saved window dimensions. Oversized saved
dimensions fall back together to 1920x1080. Invalid/smaller-than-supported values are refused.
.PARAMETER Height
Optional explicit window height. Fullscreen/borderless resolutions remain unchanged.
.EXAMPLE
.\Set-BridgeDisplay.ps1 -WhatIf
#>
[CmdletBinding(SupportsShouldProcess=$true, ConfirmImpact='Medium')]
param([string]$ConfigPath, [string]$BackupDir, [switch]$ValidateOnly,
    [ValidateRange(640,2560)][int]$Width, [ValidateRange(480,1440)][int]$Height)
$explicitWidth=$PSBoundParameters.ContainsKey('Width'); $explicitHeight=$PSBoundParameters.ContainsKey('Height')
if ($explicitWidth -ne $explicitHeight) { throw 'Specify both Width and Height, or omit both to preserve the saved window resolution.' }
. (Join-Path $PSScriptRoot 'Bridge.Common.ps1')

function Assert-DisplayStopped {
    Assert-BridgeStopped
    if (@(Get-Process -Name ersc_launcher -ErrorAction SilentlyContinue).Count) { throw 'Seamless launcher is running; close it before editing display settings.' }
}
function Get-DisplayHash([byte[]]$Bytes) {
    $sha=[Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Read-DisplayXml([byte[]]$Bytes) {
    $settings=New-Object Xml.XmlReaderSettings
    $settings.DtdProcessing=[Xml.DtdProcessing]::Prohibit; $settings.XmlResolver=$null
    $settings.MaxCharactersInDocument=1MB; $settings.MaxCharactersFromEntities=1024
    $stream=New-Object IO.MemoryStream(,$Bytes)
    $reader=$null
    try {
        $reader=[Xml.XmlReader]::Create($stream,$settings)
        $doc=New-Object Xml.XmlDocument; $doc.PreserveWhitespace=$true; $doc.XmlResolver=$null
        $doc.Load($reader)
        if ($doc.DocumentElement.Name -cne 'config' -or $doc.DocumentElement.NamespaceURI) { throw 'Expected an unnamespaced config root.' }
        return ,$doc
    } finally { if ($reader) { $reader.Dispose() }; $stream.Dispose() }
}

Assert-BridgeWindows; Assert-DisplayStopped
if (-not $env:APPDATA -or -not [IO.Path]::IsPathRooted($env:APPDATA) -or $env:APPDATA -match '^[a-zA-Z]:[^\\/]') { throw 'APPDATA must be an absolute Windows directory.' }
$configRoot=Get-BridgeFullPath (Join-Path $env:APPDATA 'EldenRing')
$expected=Join-Path $configRoot 'GraphicsConfig.xml'
if (-not $ConfigPath) { $ConfigPath=$expected }
$ConfigPath=Get-BridgeFullPath $ConfigPath
if ($ConfigPath -ne $expected) { throw 'ConfigPath must be exactly APPDATA/EldenRing/GraphicsConfig.xml.' }
Assert-BridgeInside $ConfigPath $configRoot; Assert-BridgeNoLinks $ConfigPath
if (-not (Test-Path -LiteralPath $ConfigPath -PathType Leaf)) { throw 'GraphicsConfig.xml is missing; create it through the game settings before launching with the bridge.' }
$item=Get-Item -LiteralPath $ConfigPath
if ($item.Length -gt 1MB) { throw 'GraphicsConfig.xml exceeds the 1 MiB limit.' }
if (($item.Attributes -band [IO.FileAttributes]::ReadOnly) -ne 0) { throw 'GraphicsConfig.xml is read-only; no attributes were changed.' }
$original=[IO.File]::ReadAllBytes($ConfigPath)
$oldHash=Get-DisplayHash $original
$doc=Read-DisplayXml $original
$bom=0
if ($original.Length -ge 2 -and $original[0] -eq 0xff -and $original[1] -eq 0xfe) {
    $encoding=New-Object Text.UnicodeEncoding($false,$false,$true); $bom=2
} elseif ($original.Length -ge 2 -and $original[0] -eq 0xfe -and $original[1] -eq 0xff) {
    $encoding=New-Object Text.UnicodeEncoding($true,$false,$true); $bom=2
} elseif ($original.Length -ge 4 -and $original[0] -eq 0x3c -and $original[1] -eq 0 -and $original[3] -eq 0) {
    $encoding=New-Object Text.UnicodeEncoding($false,$false,$true)
} elseif ($original.Length -ge 4 -and $original[0] -eq 0 -and $original[1] -eq 0x3c -and $original[2] -eq 0) {
    $encoding=New-Object Text.UnicodeEncoding($true,$false,$true)
} else {
    $encoding=New-Object Text.UTF8Encoding($false,$true)
    if ($original.Length -ge 3 -and $original[0] -eq 0xef -and $original[1] -eq 0xbb -and $original[2] -eq 0xbf) { $bom=3 }
}
$text=$encoding.GetString($original,$bom,$original.Length-$bom)
$resolutionSource='Explicit'
if (-not $explicitWidth) {
    $dimensions=@{}
    foreach ($name in @('Resolution-WindowScreenWidth','Resolution-WindowScreenHeight')) {
        $nodes=$doc.SelectNodes('/config/'+$name)
        if ($nodes.Count -ne 1 -or $nodes[0].Attributes.Count -ne 0 -or $nodes[0].SelectNodes('*').Count -ne 0 -or $nodes[0].InnerText.Trim() -notmatch '^[0-9]{3,5}$') { throw "Missing/duplicate/invalid saved window dimension: $name" }
        $dimensions[$name]=[int]$nodes[0].InnerText.Trim()
    }
    $savedWidth=$dimensions['Resolution-WindowScreenWidth']; $savedHeight=$dimensions['Resolution-WindowScreenHeight']
    if ($savedWidth -lt 640 -or $savedHeight -lt 480) { throw 'Saved window resolution is below 640x480; select supported dimensions explicitly.' }
    $resolutionSource='SavedWindow'
    if ($savedWidth -gt 2560 -or $savedHeight -gt 1440) { $Width=1920; $Height=1080; $resolutionSource='Fallback1080p' }
    else { $Width=$savedWidth; $Height=$savedHeight }
}
$changes=[ordered]@{ScreenMode='WINDOW'; 'Resolution-WindowScreenWidth'=$Width.ToString([Globalization.CultureInfo]::InvariantCulture); 'Resolution-WindowScreenHeight'=$Height.ToString([Globalization.CultureInfo]::InvariantCulture)}
foreach ($name in $changes.Keys) {
    $nodes=$doc.SelectNodes('/config/'+$name)
    if ($nodes.Count -ne 1 -or $nodes[0].Attributes.Count -ne 0 -or $nodes[0].SelectNodes('*').Count -ne 0) { throw "Missing/duplicate/non-scalar display setting: $name" }
    # Patch only the exact text spans; XmlDocument.Save would reformat unrelated content.
    $matches=[regex]::Matches($text,('<'+[regex]::Escape($name)+'\s*>(?<value>[^<]*)</'+[regex]::Escape($name)+'\s*>'))
    if ($matches.Count -ne 1) { throw "Ambiguous display setting text: $name" }
    $span=$matches[0].Groups['value']
    $text=$text.Substring(0,$span.Index)+$changes[$name]+$text.Substring($span.Index+$span.Length)
}
$encoded=$encoding.GetBytes($text)
$updated=New-Object byte[] ($bom+$encoded.Length)
if ($bom) { [Array]::Copy($original,0,$updated,0,$bom) }
[Array]::Copy($encoded,0,$updated,$bom,$encoded.Length)
$null=Read-DisplayXml $updated
$newHash=Get-DisplayHash $updated
if (-not $BackupDir) { $BackupDir=Join-Path $script:BridgePackageRoot 'bridge-backups\display' }
$backup=Get-BridgeBackupDir $BackupDir ''
$result=[pscustomobject]@{Status='Validated'; Changed=($oldHash -ne $newHash); ConfigPath=$ConfigPath;
    ScreenMode='WINDOW'; Width=$Width; Height=$Height; ResolutionSource=$resolutionSource; Encoding=$encoding.WebName; BomBytes=$bom;
    OriginalSha256=$oldHash; UpdatedSha256=$newHash; BackupPath=$null; JournalPath=$null}
if ($ValidateOnly) { return $result }
if (-not $result.Changed) { $result.Status='Unchanged'; return $result }
if (-not $PSCmdlet.ShouldProcess($ConfigPath,('Back up original graphics configuration and set WINDOW '+$Width+'x'+$Height))) { return }
Assert-DisplayStopped; Assert-BridgeFileState $ConfigPath $oldHash; Assert-BridgeNoLinks $backup
$session=Join-Path $backup ((Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssfffffffZ')+'-'+[guid]::NewGuid().ToString('N'))
Assert-BridgeInside $session $backup; Assert-BridgeNoLinks $session
if (Test-Path -LiteralPath $session) { throw 'Display backup session already exists.' }
$null=[IO.Directory]::CreateDirectory($session)
$saved=Join-Path $session 'GraphicsConfig.original.xml'; $staged=Join-Path $session 'GraphicsConfig.window.xml'
$journalPath=Join-Path $session 'display-change.json'
$journal=[pscustomobject]@{SchemaVersion=1; Kind='ErmcDisplay'; Status='Preparing'; ConfigPath=$ConfigPath;
    OriginalSha256=$oldHash; UpdatedSha256=$newHash; Encoding=$encoding.WebName; BomBytes=$bom; CreatedUtc=[DateTime]::UtcNow.ToString('o')}
try {
    [IO.File]::WriteAllBytes($saved,$original); Assert-BridgeFileState $saved $oldHash
    [IO.File]::WriteAllBytes($staged,$updated); Assert-BridgeFileState $staged $newHash
    Save-BridgeManifest $journal $journalPath $backup
    Assert-DisplayStopped; Assert-BridgeFileState $ConfigPath $oldHash
    Write-BridgeCheckedFile $ConfigPath $staged $oldHash $newHash $configRoot
    $writtenHash=Get-BridgeHash $ConfigPath
    if ($writtenHash -ne $newHash) { throw 'Display configuration changed during verification.' }
    $journal.Status='Updated'; Save-BridgeManifest $journal $journalPath $backup
} catch {
    $failure=$_
    try {
        Assert-DisplayStopped
        $actual=Get-BridgeFileState $ConfigPath
        if ($actual -eq $newHash) {
            Assert-BridgeFileState $saved $oldHash
            Write-BridgeCheckedFile $ConfigPath $saved $newHash $oldHash $configRoot
        } elseif ($actual -ne $oldHash) { throw 'Unrecognized config change; preserve it for recovery.' }
        $journal.Status='RolledBack'; Save-BridgeManifest $journal $journalPath $backup
    } catch { Write-Warning "Display rollback could not safely finish. Original backup: $saved" }
    throw $failure
}
$result.Status='Updated'; $result.BackupPath=$saved; $result.JournalPath=$journalPath
return $result
