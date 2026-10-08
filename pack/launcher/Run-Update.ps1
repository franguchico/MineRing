param([Parameter(Mandatory=$true)][string]$InstalledPackage,[string]$InstancesDirectory,[switch]$DryRun,[string]$GameDir,[string]$SteamPath)
$ErrorActionPreference='Stop'
$u=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\EldenMinecraft-Windows\elden-ring\windows\Update-CoopV2.ps1'))
$a=@{InstalledPackage=$InstalledPackage;Confirm=$false}
if($InstancesDirectory){$a.InstancesDirectory=@($InstancesDirectory)}
if($GameDir){$a.GameDir=$GameDir}
if($SteamPath){$a.SteamPath=@($SteamPath)}
if($DryRun){$a.WhatIf=$true}
try{
  $r=& $u @a
  if($DryRun){Write-Output 'RESULT|WhatIf|0'}
  else{Write-Output ('RESULT|'+$r.Status+'|'+$r.ProfilesVerified)}
}catch{
  $c=$_.Exception.Data['ErmcCode']
  Write-Output ('ERROR|'+$c+'|'+$_.Exception.Message)
  exit 2
}
