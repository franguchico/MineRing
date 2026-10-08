<# Sends the mod's normal quit command. Minecraft saves its own world before exiting. #>
[CmdletBinding()]
param([string]$IpcDir = (Join-Path $env:USERPROFILE 'Documents\EldenMinecraft\ipc'))
$ErrorActionPreference = 'Stop'
$channelPath = Join-Path $IpcDir 'bridge.shm'
function Read-MinecraftHeader {
    $stream = [IO.File]::Open($channelPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        $bytes = New-Object byte[] 40
        if ($stream.Read($bytes, 0, $bytes.Length) -ne $bytes.Length) { throw 'Canal do bridge incompleto.' }
        if ([BitConverter]::ToUInt32($bytes, 0) -ne 0x434D484D) { throw 'Canal do bridge desconhecido.' }
        return [pscustomobject]@{ Pid = [BitConverter]::ToUInt32($bytes, 0x24); Heartbeat = [BitConverter]::ToUInt64($bytes, 0x18) }
    } finally { $stream.Dispose() }
}
if (-not (Test-Path -LiteralPath $channelPath -PathType Leaf)) {
    Write-Host 'Minecraft do bridge ja esta fechado.'
    return
}
$before = Read-MinecraftHeader
Start-Sleep -Milliseconds 250
$after = Read-MinecraftHeader
$minecraftProcess = Get-Process -Id $after.Pid -ErrorAction SilentlyContinue
if ($after.Pid -eq 0 -or $before.Pid -ne $after.Pid -or $before.Heartbeat -eq $after.Heartbeat -or
    -not $minecraftProcess -or $minecraftProcess.ProcessName -notin @('java', 'javaw')) {
    Write-Host 'Minecraft do bridge ja esta fechado ou nao responde; nenhum processo foi encerrado.'
    return
}
# CreateNew refuses to replace a command the mod has not consumed yet.
$command = [IO.File]::Open((Join-Path $IpcDir 'mc_cmd.txt'), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try {
    $quit = [Text.Encoding]::UTF8.GetBytes('quit')
    $command.Write($quit, 0, $quit.Length)
} finally { $command.Dispose() }
Write-Host 'Minecraft recebeu o pedido de salvar e fechar.'
if ($minecraftProcess.WaitForExit(10000)) { Write-Host 'Minecraft fechado.' }
else { Write-Host 'O pedido foi enviado; aguarde o Minecraft terminar de salvar.' }
