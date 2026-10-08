[CmdletBinding(SupportsShouldProcess=$true)]
param([string]$InstalledPackage,[string[]]$InstancesDirectory)
$ErrorActionPreference='Stop'
$kit=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $InstalledPackage) {
    Write-Host 'Use a pasta EldenMinecraft-Windows da instalacao ANTERIOR, que contem bridge-backups.'
    $InstalledPackage=(Read-Host 'Cole o caminho dessa pasta').Trim().Trim('"')
}
$InstalledPackage=[IO.Path]::GetFullPath($InstalledPackage)
$arguments=@{InstalledPackage=$InstalledPackage;WhatIf=[bool]$WhatIfPreference;Confirm=$false}
if ($InstancesDirectory) { $arguments.InstancesDirectory=$InstancesDirectory }
$updater=Join-Path $kit 'EldenMinecraft-Windows\elden-ring\windows\Update-CoopV2.ps1'
try { $result=& $updater @arguments }
catch {
    if ($InstancesDirectory -or $WhatIfPreference -or $_.Exception.Data['ErmcCode'] -ne 'BridgeProfilesNotFound') { throw }
    Write-Host 'O Prism esta usando outra pasta de dados. Nenhum arquivo da instalacao foi alterado.'
    Write-Host 'Localize a pasta instances do Prism usado para jogar e copie seu caminho. Ela contem as pastas dos seus perfis.'
    $customInstances=(Read-Host 'Cole o caminho da pasta instances').Trim().Trim('"')
    if (-not $customInstances) { throw 'Pasta nao informada. Atualizacao cancelada sem alterar a instalacao.' }
    $arguments.InstancesDirectory=@($customInstances)
    $result=& $updater @arguments
}
if ($WhatIfPreference) { return }
if ($result.Status -notin @('Installed','AlreadyInstalled') -or $result.ProfilesVerified -lt 1) {
    throw 'A atualizacao nao confirmou nenhum perfil Minecraft. Preserve as pastas e copie o erro.'
}
$localState=@{InstalledPackage=$InstalledPackage}
if ($arguments.InstancesDirectory) {
    $localState.InstancesDirectories=@($arguments.InstancesDirectory | ForEach-Object { [IO.Path]::GetFullPath($_) })
    Write-Host 'Pasta de instancias personalizada: abra o Prism pelo seu atalho habitual para usar esses mesmos perfis.'
}
[IO.File]::WriteAllText((Join-Path $kit 'configuracao-local.json'),($localState|ConvertTo-Json),(New-Object Text.UTF8Encoding($false)))
Write-Host ('Atualizacao concluida. Perfis Minecraft verificados: '+$result.ProfilesVerified)
Write-Host 'Usem os MESMOS perfis no Prism: Anfitriao ou Convidado, conforme seu papel. Seus mundos continuam nesses perfis.'
$result
