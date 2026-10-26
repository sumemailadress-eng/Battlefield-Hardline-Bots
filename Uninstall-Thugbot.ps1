# Removes Thugbot from the Hardline server and restores the stock xinput9_1_0.dll.
param([string]$Server = '')
if ($Server) { & (Join-Path $PSScriptRoot 'Install-Thugbot.ps1') -Server $Server -Uninstall } else { & (Join-Path $PSScriptRoot 'Install-Thugbot.ps1') -Uninstall }
