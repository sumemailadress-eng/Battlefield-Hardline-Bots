# Builds the shareable package: dist\ThugbotHardline-<date>\ and dist\ThugbotHardline-<date>.zip
#   powershell -ExecutionPolicy Bypass -File tools\make-dist.ps1
# Takes the mod (out\), the scripts, README.txt, Thugbot.ini defaults, the server's learned data (current grid / route
# versions only) and the source. Never the server's own launchers (they hold its admin password) or logs.
param([string]$Server = '')
if (-not $Server) { . (Join-Path (Split-Path $PSScriptRoot) 'Thugbot-Paths.ps1'); $Server = (Get-ThugbotConfig -NoSearch).Server }
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$name = "ThugbotHardline-$(Get-Date -Format yyyy-MM-dd-HHmm)"
$dst = Join-Path $root "dist\$name"
if (Test-Path $dst) { Remove-Item $dst -Recurse -Force }
New-Item -ItemType Directory -Force $dst, "$dst\out", "$dst\Data", "$dst\Source" | Out-Null

foreach ($f in 'README.txt', 'Setup-Thugbot.bat', 'Setup-Thugbot.ps1', 'Thugbot-Paths.ps1', 'Start-Thugbot.bat', 'Start-Thugbot.ps1', 'Thugbot-Control.bat', 'Thugbot-Control.ps1', 'Thugbot-Keys.bat',
               'Thugbot-Keys.ps1', 'Thugbot-Chat.ps1', 'Open-Thugbot-Map.bat', 'Install-Thugbot.ps1', 'Uninstall-Thugbot.ps1') {
    Copy-Item (Join-Path $root $f) $dst
}
Copy-Item "$root\out\xinput9_1_0.dll", "$root\out\ThugbotCore.dll" "$dst\out"

# Default settings (the same text the mod writes on first run)
@'
[Thugbot]
; Bot gunfight level: easy, normal, hard, veteran, elite
Difficulty=hard
; 0 = cautious: takes cover early when hurt or outnumbered, falls back when there is no cover
; 1 = normal: fights back, covers when badly hurt, pops up from cover to shoot
; 2 = aggressive: covers only when nearly dead, pushes in close and knifes
Aggression=2
; Part of each team that stays to defend flags it owns (0 - 0.8)
DefendShare=0.15
; 0 = the simple first-version combat (no cover, no pitch aim)
Combat=1
; Bots in cover stand up to shoot back now and then (Aggression 1 and 2)
Peek=1
; Bots get in and drive vehicles (1) or stay on foot and leave vehicles they spawn in (0)
Vehicles=0
'@ | Set-Content "$dst\Thugbot.ini" -Encoding ascii

# Learned data: everything learned, and only the current grid / route versions (older ones are rebuilt anyway)
$navVer = if ((Get-Content "$root\src\brain.inc" -Raw) -match '"Thugbot_nav", "\.(v\d+)\.nav"') { $Matches[1] } else { throw 'grid version not found in brain.inc' }
$routeVer = if ((Get-Content "$root\src\routes.inc" -Raw) -match '"Thugbot_routes", "\.(v\d+)\.routes"') { $Matches[1] } else { throw 'route version not found in routes.inc' }
New-Item -ItemType Directory -Force "$dst\Data\Thugbot_learned", "$dst\Data\Thugbot_nav", "$dst\Data\Thugbot_routes" | Out-Null
Get-ChildItem "$Server\Thugbot_learned" -File | Copy-Item -Destination "$dst\Data\Thugbot_learned"
Get-ChildItem "$Server\Thugbot_nav" -File -Filter "*.$navVer.nav" | Copy-Item -Destination "$dst\Data\Thugbot_nav"
Get-ChildItem "$Server\Thugbot_routes" -File -Filter "*.$routeVer.routes" | Copy-Item -Destination "$dst\Data\Thugbot_routes"

# Source
Copy-Item "$root\src" "$dst\Source\src" -Recurse
Copy-Item "$root\tools" "$dst\Source\tools" -Recurse
Copy-Item "$root\build.bat" "$dst\Source"
# (PORT-PLAN.md - the work log - stays out: it names this PC's folders and players. Thugbot-Paths.ini too: each PC
# makes its own with Setup-Thugbot.bat.)

# Source comments quote the tester by name: neutral in the package.
Get-ChildItem "$dst\Source" -Recurse -File -Include *.c, *.h, *.inc, *.ps1, *.sh, *.py | ForEach-Object {
    $c = [IO.File]::ReadAllText($_.FullName); $n = $c -replace '\bRob\b', 'the tester' -replace "\bRob's\b", "the tester's"
    if ($n -ne $c) { [IO.File]::WriteAllText($_.FullName, $n) }
}
# Nothing personal in the package: no player name, no folders or addresses of this PC.
. (Join-Path $root 'Thugbot-Paths.ps1'); $me = Get-ThugbotConfig -NoSearch
$bad = @(('D:' + '\BF-Portable'), ('/d/' + 'BF-Portable'), $env:USERNAME, $env:COMPUTERNAME, $me.Name, $me.Portable, $me.Server) | Where-Object { $_ -and $_.Length -ge 3 } | Select-Object -Unique
$text = '*.ps1', '*.bat', '*.txt', '*.md', '*.ini', '*.c', '*.h', '*.inc', '*.sh', '*.py'
$hits = @(Get-ChildItem $dst -Recurse -File -Include $text | Select-String -SimpleMatch -Pattern $bad)
foreach ($ip in @((Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object { $_.IPAddress -notlike '127.*' }).IPAddress)) {
    $hits += @(Get-ChildItem $dst -Recurse -File -Include $text | Select-String -SimpleMatch -Pattern $ip)
}
if ($hits.Count) { $hits | ForEach-Object { Write-Host "personal: $($_.Path):$($_.LineNumber) $($_.Line.Trim())" }; throw 'The package names this PC (above) - fix those files first.' }

$zip = Join-Path $root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $dst -DestinationPath $zip
$n = (Get-ChildItem $dst -Recurse -File).Count
Write-Host ("{0}: {1} files, {2:N1} MB (grids {3}, routes {4})" -f $zip, $n, ((Get-Item $zip).Length / 1MB), $navVer, $routeVer)
