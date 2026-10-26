# Installs Thugbot into the BF-Portable Hardline server: the stock xinput9_1_0.dll is backed up to
# Thugbot_backup\ and renamed xinput9_1_0_orig.dll (the proxy forwards to it); the proxy and core go next to it.
# Uninstall-Thugbot.ps1 puts the stock DLL back. Stop the server first.
param([string]$Server = '', [switch]$Uninstall)
$ErrorActionPreference = 'Stop'
if (-not $Server) { . (Join-Path $PSScriptRoot 'Thugbot-Paths.ps1'); $cfg = Get-ThugbotConfig; Assert-ThugbotPath $cfg 'Server' 'The Hardline server (bfh_server_Final.exe)'; $Server = $cfg.Server }
$out = Join-Path $PSScriptRoot 'out'
$backup = Join-Path $Server 'Thugbot_backup'
if (Get-Process bfh_server_Final -ErrorAction SilentlyContinue) { throw 'Stop the Hardline server first.' }

if ($Uninstall) {
    $stock = Join-Path $backup 'xinput9_1_0.dll'
    if (-not (Test-Path $stock)) { throw "No backup at $stock" }
    Copy-Item $stock (Join-Path $Server 'xinput9_1_0.dll') -Force
    foreach ($f in 'xinput9_1_0_orig.dll', 'ThugbotCore.dll', 'ThugbotCore.loaded.dll') { Remove-Item (Join-Path $Server $f) -ErrorAction SilentlyContinue }
    Write-Host 'Thugbot removed; stock xinput9_1_0.dll restored.'
    return
}

if (-not (Test-Path (Join-Path $Server 'xinput9_1_0_orig.dll'))) {
    New-Item -ItemType Directory -Force $backup | Out-Null
    Copy-Item (Join-Path $Server 'xinput9_1_0.dll') (Join-Path $backup 'xinput9_1_0.dll')
    Copy-Item (Join-Path $Server 'xinput9_1_0.dll') (Join-Path $Server 'xinput9_1_0_orig.dll')
}
Copy-Item (Join-Path $out 'xinput9_1_0.dll') $Server -Force
Copy-Item (Join-Path $out 'ThugbotCore.dll') $Server -Force
# From the shared package: settings and what the bots learned so far. Files the server already has are kept.
$ini = Join-Path $PSScriptRoot 'Thugbot.ini'
if ((Test-Path $ini) -and -not (Test-Path (Join-Path $Server 'Thugbot.ini'))) { Copy-Item $ini $Server }
$data = Join-Path $PSScriptRoot 'Data'
if (Test-Path $data) {
    foreach ($d in Get-ChildItem $data -Directory) {
        $to = Join-Path $Server $d.Name; New-Item -ItemType Directory -Force $to | Out-Null
        foreach ($fi in Get-ChildItem $d.FullName -File) { $t = Join-Path $to $fi.Name; if (-not (Test-Path $t)) { Copy-Item $fi.FullName $t } }
    }
}
Write-Host "Thugbot installed in $Server (stock DLL backed up in $backup)."
