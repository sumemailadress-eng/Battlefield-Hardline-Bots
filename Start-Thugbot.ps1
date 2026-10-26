# One-click Thugbot launcher: starts the BF-Portable backend (PortableBF for MySQL / web, the Hardline Blaze emulator),
# installs the latest Thugbot build into the Hardline server and starts it with pseudo-player bots.
#   Start-Thugbot.bat              60 bots (30 vs 30)
#   Start-Thugbot.bat -Bots 24     another count
#   Start-Thugbot.bat -NoInstall   start without copying out\ into the server
#   Start-Thugbot.bat -NoBuild     install out\ as it is (skip build.bat)
param(
    [int]$Bots = 60,
    [string]$Portable = '',   # (empty: from Thugbot-Paths.ini - Setup-Thugbot.bat)
    [switch]$NoInstall,
    [switch]$NoBuild,
    [string]$Player = ''      # your in-game name, for the route hotkeys (F5 - F11); empty: from Thugbot-Paths.ini
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Thugbot-Paths.ps1')
$cfg = Get-ThugbotConfig -Quiet
if (-not $Portable) { $Portable = $cfg.Portable }
if (-not $Player) { $Player = $cfg.Name }
Assert-ThugbotPath $cfg 'Portable' 'The BF-Portable backend (PortableBF.exe)'
Assert-ThugbotPath $cfg 'Server' 'The Hardline server (bfh_server_Final.exe)'
Assert-ThugbotPath $cfg 'Emulator' 'The Hardline Blaze emulator (BlazeServerH.exe)'
$server = $cfg.Server
$emu = $cfg.Emulator

function Start-IfMissing([string]$name, [string]$exe, [string]$dir) {
    if (Get-Process $name -ErrorAction SilentlyContinue) { Write-Host "$name already running"; return }
    if (-not (Test-Path $exe)) { throw "Not found: $exe" }
    Write-Host "Starting $name"
    Start-Process -FilePath $exe -WorkingDirectory $dir | Out-Null
}

if (Get-Process bfh_server_Final -ErrorAction SilentlyContinue) { throw 'A Hardline server is already running. Close it first.' }

Start-IfMissing 'PortableBF' (Join-Path $Portable 'PortableBF.exe') $Portable
Start-Sleep -Seconds 3
Start-IfMissing 'BlazeServerH' (Join-Path $emu 'BlazeServerH.exe') $emu
Start-Sleep -Seconds 5

$vc = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (-not $NoBuild -and (-not (Test-Path (Join-Path $PSScriptRoot 'build.bat')) -or -not (Test-Path $vc))) {
    Write-Host 'No build tools here - using the prebuilt DLLs in out\'; $NoBuild = $true
}
if (-not $NoInstall -and -not $NoBuild) {
    Write-Host 'Building Thugbot (build.bat)'
    & cmd /c (Join-Path $PSScriptRoot 'build.bat')
    if ($LASTEXITCODE -ne 0) { throw 'build.bat failed - see the compiler errors above. The server was not started.' }
}
if (-not $NoInstall) { & (Join-Path $PSScriptRoot 'Install-Thugbot.ps1') -Server $server }

# Same settings as !StartServer.bat, plus the engine's pseudo-players for Thugbot to drive. KillRate / DamageRate /
# ScoreRate / SuicideRate -1: no simulated kills or score - only what the bots really do counts.
$bat = Join-Path $server '!StartServer-Bots.bat'
if (-not (Test-Path $bat)) {
    # First run on this PC: made from the server's own !StartServer.bat (its name, ports and admin password), with the
    # pseudo-player bots added to the start line.
    $stock = Join-Path $server '!StartServer.bat'
    if (-not (Test-Path $stock)) { throw "Not found: $stock (the BF-Portable Hardline server launcher)" }
    $lines = Get-Content $stock
    if (-not ($lines | Where-Object { $_ -match '^\s*start\s+.*bfh_server_Final\.exe' })) { throw "$stock has no 'start bfh_server_Final.exe' line" }
    $out = @('rem Thugbot launcher: a copy of !StartServer.bat plus the engine''s pseudo-player bots (Start-Thugbot.ps1 made it).',
             'rem BOTS = how many pseudo-players join (split across both teams). Delete this file to have it made again.',
             'set BOTS=60',
             'set PSEUDO=-PseudoPlayer.SpawnCount %BOTS% -PseudoPlayer.SetOnlineId true -PseudoPlayer.SuicideRate -1 -PseudoPlayer.ReconnectRate -1 -PseudoPlayer.UpdateInput true -PseudoPlayer.KillRate -1 -PseudoPlayer.DamageRate -1 -PseudoPlayer.ScoreRate -1')
    foreach ($l in $lines) { if ($l -match '^\s*start\s+.*bfh_server_Final\.exe') { $out += "$l %PSEUDO%" } elseif ($l -notmatch '^\s*pause\s*$') { $out += $l } }
    Set-Content $bat $out -Encoding ascii
    Write-Host "Made $bat from !StartServer.bat"
}
$text = Get-Content $bat -Raw
$text = $text -replace '(?m)^set BOTS=\d+', "set BOTS=$Bots"
Set-Content $bat $text -NoNewline -Encoding ascii
Write-Host "Starting the Hardline server with $Bots bots"
Start-Process -FilePath $bat -WorkingDirectory $server | Out-Null
# the chat bridge for the in-game route editor (!wp, !rec, !save ...): hidden, quits when the server is gone
$chat = Join-Path $PSScriptRoot 'Thugbot-Chat.ps1'
Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden', '-File', "`"$chat`"", '-Portable', "`"$Portable`"" -WindowStyle Hidden | Out-Null
# route hotkeys (F5 record, F6 save, F7 point, F8 undo, F9 clear, F10 elevator / ladder, F11 bots out): hidden too
$keys = Join-Path $PSScriptRoot 'Thugbot-Keys.ps1'
Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden', '-File', "`"$keys`"", '-Portable', "`"$Portable`"", '-Player', "`"$Player`"" -WindowStyle Hidden | Out-Null
Write-Host "Log: $server\Thugbot_25200.log   Commands: $server\Thugbot_cmd.txt"
