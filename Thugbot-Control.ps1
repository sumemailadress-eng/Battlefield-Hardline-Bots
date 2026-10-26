# Thugbot Control: a small window to run the Hardline bot server. Start / stop it with a number of bots per team, pick a
# map and mode and switch to it now, edit the map rotation, restart or end the round, and open the learned map.
# Talks to the server over RCON (the port and password from !StartServer-Bots.bat). Run Thugbot-Control.bat.
# Also starts the backend (MySQL, the web server with the server-list API, the Hardline Blaze emulator) and finds the
# games running on the local network (every BF-Portable host's server-list API on this PC's /24 subnets).
param([string]$Portable = '')   # (empty: from Thugbot-Paths.ini)
. (Join-Path $PSScriptRoot 'Thugbot-Paths.ps1')
$cfg = Get-ThugbotConfig -Quiet
if (-not $Portable) { $Portable = $cfg.Portable }
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()

$server = if ($cfg.Server) { $cfg.Server } else { Join-Path $Portable 'Battlefield_BFH_Server' }
$launcher = Join-Path $server '!StartServer-Bots.bat'
$batText = if (Test-Path $launcher) { Get-Content $launcher -Raw } else { '' }
$rconPort = if ($batText -match '(?m)^set REMOTEADMINPORT=(\d+)') { [int]$Matches[1] } else { 47600 }
$rconPass = if ($batText -match '(?m)^set ADMINPASSWORD=(\S+)') { $Matches[1] } else { '' }
$botsNow = if ($batText -match '(?m)^set BOTS=(\d+)') { [int]$Matches[1] } else { 32 }

# ---- RCON ----
function Send-Packet($stream, [int]$seq, [string[]]$words) {
    $body = New-Object IO.MemoryStream
    foreach ($w in $words) {
        $b = [Text.Encoding]::ASCII.GetBytes($w)
        $body.Write([BitConverter]::GetBytes([int]$b.Length), 0, 4); $body.Write($b, 0, $b.Length); $body.WriteByte(0)
    }
    $wb = $body.ToArray()
    $hdr = [BitConverter]::GetBytes([int]($seq -band 0x3fffffff)) + [BitConverter]::GetBytes([int](12 + $wb.Length)) + [BitConverter]::GetBytes([int]$words.Count)
    $stream.Write($hdr + $wb, 0, 12 + $wb.Length)
}
function Read-Exact($stream, [int]$n) {
    $buf = New-Object byte[] $n; $got = 0
    while ($got -lt $n) { $r = $stream.Read($buf, $got, $n - $got); if ($r -le 0) { throw 'connection closed' }; $got += $r }
    return $buf
}
function Read-Packet($stream) {
    $h = Read-Exact $stream 12
    $size = [BitConverter]::ToInt32($h, 4); $count = [BitConverter]::ToInt32($h, 8)
    $b = Read-Exact $stream ($size - 12); $o = 0; $words = @()
    for ($i = 0; $i -lt $count; $i++) { $len = [BitConverter]::ToInt32($b, $o); $words += [Text.Encoding]::ASCII.GetString($b, $o + 4, $len); $o += 5 + $len }
    return ,$words
}
# Runs RCON commands (each a word array); returns one word array per command, or $null when the server isn't reachable.
function Invoke-Rcon([object[]]$commands) {
    try {
        $c = New-Object Net.Sockets.TcpClient
        $iar = $c.BeginConnect('127.0.0.1', $rconPort, $null, $null)
        if (-not $iar.AsyncWaitHandle.WaitOne(800)) { $c.Close(); return $null }
        $c.EndConnect($iar); $s = $c.GetStream(); $s.ReadTimeout = 5000; $seq = 1
        if ($rconPass) { Send-Packet $s $seq @('login.plainText', $rconPass); $seq++; Read-Packet $s | Out-Null }
        $out = @()
        foreach ($cmd in $commands) { Send-Packet $s $seq ([string[]]$cmd); $seq++; $out += ,(Read-Packet $s) }
        $c.Close()
        return ,$out
    } catch { return $null }
}

# ---- maps and modes ----
$mapNames = [ordered]@{
    'mp_bank' = 'Bank Job'; 'mp_bloodout' = 'The Block'; 'mp_desert05' = 'Dust Bowl'; 'mp_downtown' = 'Downtown'
    'mp_eastside' = 'Derailed'; 'mp_glades' = 'Everglades'; 'mp_growhouse' = 'Growhouse'; 'mp_hills' = 'Hollywood Heights'
    'mp_offshore' = 'Riptide'; 'xp1_mallcops' = 'Black Friday'; 'xp1_nights' = 'Code Blue'; 'xp1_projects' = 'The Beat'
    'xp1_sawmill' = 'Backwoods'; 'xp2_museum02' = 'Museum'; 'xp2_precinct7' = 'Precinct 7'; 'xp4_alcatraz' = 'Alcatraz'
    'xp4_chinatown' = 'Chinatown'; 'xp4_cemetery' = 'Cemetery'
}
$modeNames = [ordered]@{
    'TWL0' = @('TurfWarLarge0', 'Conquest Large'); 'TWS0' = @('TurfWarSmall0', 'Conquest Small'); 'TDM0' = @('TeamDeathMatch0', 'Team Deathmatch')
    'HST0' = @('Heist0', 'Heist'); 'BM0' = @('BloodMoney0', 'Blood Money'); 'HOT0' = @('Hotwire0', 'Hotwire'); 'HIT0' = @('Hit0', 'Crosshair')
    'HOS0' = @('Hostage0', 'Rescue'); 'CG0' = @('CashGrab0', 'Bounty Hunter'); 'SQH0' = @('SquadHeist0', 'Squad Heist'); 'CTF0' = @('CaptureTheFlag0', 'Capture the Flag')
}
$botModes = @('TurfWarLarge0', 'TurfWarSmall0', 'TeamDeathMatch0')   # what the bots play properly today
function Map-Label($code) { $n = $mapNames[$code.ToLower()]; if ($n) { "$n ($code)" } else { $code } }
function Mode-Label($full) { foreach ($k in $modeNames.Keys) { if ($modeNames[$k][0] -eq $full) { return $modeNames[$k][1] } }; $full }

# Map -> its modes (full names). From the server when it runs, else every map with the bot-friendly modes.
$available = [ordered]@{}
function Load-Available {
    $r = Invoke-Rcon @(, @('mapList.availableMaps', 'perMap'))
    $available.Clear()
    if ($r -and $r[0][0] -eq 'OK') {
        $w = $r[0]; $cur = $null
        for ($i = 1; $i -lt $w.Count; $i++) {
            $x = $w[$i]
            if ($modeNames.Contains($x)) { if ($cur) { $available[$cur] += , $modeNames[$x][0] } }
            else { $cur = $x; $available[$cur] = @() }
        }
    }
    if ($available.Count -eq 0) { foreach ($m in $mapNames.Keys) { $available[$m] = $botModes } }
}

# ---- window ----
$f = New-Object Windows.Forms.Form
$f.Text = 'Thugbot Control - Battlefield Hardline bots'; $f.Size = New-Object Drawing.Size(760, 820); $f.StartPosition = 'CenterScreen'
$f.Font = New-Object Drawing.Font('Segoe UI', 9.5)
function Add-Ctl($ctl, $x, $y, $w, $h) { $ctl.Location = New-Object Drawing.Point($x, $y); $ctl.Size = New-Object Drawing.Size($w, $h); $f.Controls.Add($ctl); $ctl }

$status = Add-Ctl (New-Object Windows.Forms.Label) 12 10 720 44
$status.Font = New-Object Drawing.Font('Segoe UI', 10.5, [Drawing.FontStyle]::Bold)

# server box
$g1 = Add-Ctl (New-Object Windows.Forms.GroupBox) 12 58 720 70; $g1.Text = 'Server'
$lblBots = New-Object Windows.Forms.Label; $lblBots.Text = 'Bots per team:'; $lblBots.Location = '12,30'; $lblBots.AutoSize = $true; $g1.Controls.Add($lblBots)
$numBots = New-Object Windows.Forms.NumericUpDown; $numBots.Location = '110,27'; $numBots.Width = 60; $numBots.Minimum = 0; $numBots.Maximum = 30
$numBots.Value = [Math]::Max(0, [Math]::Min(30, [int]($botsNow / 2))); $g1.Controls.Add($numBots)
$btnStart = New-Object Windows.Forms.Button; $btnStart.Text = 'Start server'; $btnStart.Location = '190,25'; $btnStart.Size = '110,30'; $g1.Controls.Add($btnStart)
$btnRestart = New-Object Windows.Forms.Button; $btnRestart.Text = 'Restart with this many bots'; $btnRestart.Location = '306,25'; $btnRestart.Size = '190,30'; $g1.Controls.Add($btnRestart)
$btnStop = New-Object Windows.Forms.Button; $btnStop.Text = 'Stop server'; $btnStop.Location = '502,25'; $btnStop.Size = '100,30'; $g1.Controls.Add($btnStop)
$btnMap = New-Object Windows.Forms.Button; $btnMap.Text = 'Learned map'; $btnMap.Location = '608,25'; $btnMap.Size = '100,30'; $g1.Controls.Add($btnMap)

# map picker
$g2 = Add-Ctl (New-Object Windows.Forms.GroupBox) 12 134 350 330; $g2.Text = 'Pick a map'
$lstMaps = New-Object Windows.Forms.ListBox; $lstMaps.Location = '12,24'; $lstMaps.Size = '326,200'; $g2.Controls.Add($lstMaps)
$lblMode = New-Object Windows.Forms.Label; $lblMode.Text = 'Mode:'; $lblMode.Location = '12,234'; $lblMode.AutoSize = $true; $g2.Controls.Add($lblMode)
$cmbMode = New-Object Windows.Forms.ComboBox; $cmbMode.DropDownStyle = 'DropDownList'; $cmbMode.Location = '60,231'; $cmbMode.Width = 278; $g2.Controls.Add($cmbMode)
$btnNow = New-Object Windows.Forms.Button; $btnNow.Text = 'Play this map now'; $btnNow.Location = '12,266'; $btnNow.Size = '160,32'; $g2.Controls.Add($btnNow)
$btnAdd = New-Object Windows.Forms.Button; $btnAdd.Text = 'Add to rotation'; $btnAdd.Location = '178,266'; $btnAdd.Size = '160,32'; $g2.Controls.Add($btnAdd)
$hint = New-Object Windows.Forms.Label; $hint.Location = '12,302'; $hint.Size = '326,22'; $hint.ForeColor = [Drawing.Color]::DimGray
$hint.Text = 'Bots play Conquest and Team Deathmatch best.'; $g2.Controls.Add($hint)

# rotation
$g3 = Add-Ctl (New-Object Windows.Forms.GroupBox) 372 134 360 330; $g3.Text = 'Map rotation'
$lstRot = New-Object Windows.Forms.ListBox; $lstRot.Location = '12,24'; $lstRot.Size = '336,200'; $g3.Controls.Add($lstRot)
$btnRemove = New-Object Windows.Forms.Button; $btnRemove.Text = 'Remove'; $btnRemove.Location = '12,232'; $btnRemove.Size = '100,30'; $g3.Controls.Add($btnRemove)
$btnNext = New-Object Windows.Forms.Button; $btnNext.Text = 'Next map now'; $btnNext.Location = '118,232'; $btnNext.Size = '110,30'; $g3.Controls.Add($btnNext)
$btnRound = New-Object Windows.Forms.Button; $btnRound.Text = 'Restart round'; $btnRound.Location = '234,232'; $btnRound.Size = '114,30'; $g3.Controls.Add($btnRound)
$btnSel = New-Object Windows.Forms.Button; $btnSel.Text = 'Play selected next'; $btnSel.Location = '12,268'; $btnSel.Size = '160,30'; $g3.Controls.Add($btnSel)
$btnScan = New-Object Windows.Forms.Button; $btnScan.Text = 'Scan all maps'; $btnScan.Location = '178,268'; $btnScan.Size = '170,30'; $g3.Controls.Add($btnScan)

# backend services
$g4 = Add-Ctl (New-Object Windows.Forms.GroupBox) 12 470 720 64; $g4.Text = 'Services and bot options'
$lblSvc = New-Object Windows.Forms.Label; $lblSvc.Location = '12,28'; $lblSvc.Size = '200,22'; $g4.Controls.Add($lblSvc)
$chkVeh = New-Object Windows.Forms.CheckBox; $chkVeh.Text = 'Bots use vehicles'; $chkVeh.Location = '214,26'; $chkVeh.Size = '132,24'; $g4.Controls.Add($chkVeh)
$btnSql = New-Object Windows.Forms.Button; $btnSql.Text = 'Start MySQL'; $btnSql.Location = '348,22'; $btnSql.Size = '110,30'; $g4.Controls.Add($btnSql)
$btnWeb = New-Object Windows.Forms.Button; $btnWeb.Text = 'Start web'; $btnWeb.Location = '464,22'; $btnWeb.Size = '100,30'; $g4.Controls.Add($btnWeb)
$btnAll = New-Object Windows.Forms.Button; $btnAll.Text = 'Start all'; $btnAll.Location = '570,22'; $btnAll.Size = '138,30'; $g4.Controls.Add($btnAll)

# LAN games
$g5 = Add-Ctl (New-Object Windows.Forms.GroupBox) 12 540 720 190; $g5.Text = 'Games on the local network'
$lvLan = New-Object Windows.Forms.ListView; $lvLan.Location = '12,24'; $lvLan.Size = '580,154'; $lvLan.View = 'Details'; $lvLan.FullRowSelect = $true
foreach ($c in @(@('Game', 60), @('Server', 170), @('Map', 130), @('Mode', 110), @('Players', 60), @('Host', 110))) { [void]$lvLan.Columns.Add($c[0], $c[1]) }
$g5.Controls.Add($lvLan)
$btnLan = New-Object Windows.Forms.Button; $btnLan.Text = 'Find LAN games'; $btnLan.Location = '600,24'; $btnLan.Size = '108,32'; $g5.Controls.Add($btnLan)
$btnLaunch = New-Object Windows.Forms.Button; $btnLaunch.Text = 'BF Launcher'; $btnLaunch.Location = '600,62'; $btnLaunch.Size = '108,32'; $g5.Controls.Add($btnLaunch)

$log = Add-Ctl (New-Object Windows.Forms.Label) 12 736 720 40; $log.ForeColor = [Drawing.Color]::DimGray

function Say($t) { $log.Text = "$(Get-Date -Format HH:mm:ss)  $t" }
$rotation = @()
function Refresh-Status {
    $r = Invoke-Rcon @(, @('serverInfo'), , @('mapList.list'), , @('mapList.getMapIndices'))
    if (-not $r) {
        $running = [bool](Get-Process bfh_server_Final -ErrorAction SilentlyContinue)
        $status.Text = if ($running) { 'Server starting (not answering RCON yet)...' } else { 'Server is not running. Press Start server.' }
        $status.ForeColor = [Drawing.Color]::DarkOrange
        return
    }
    $si = $r[0]
    $status.ForeColor = [Drawing.Color]::SeaGreen
    $status.Text = "Running: $(Map-Label $si[5]), $(Mode-Label $si[4]) - $($si[2]) of $($si[3]) players - round $([int]$si[6] + 1) of $($si[7])"
    $ml = $r[1]; $n = [int]$ml[1]; $per = [int]$ml[2]; $script:rotation = @()
    $idx = $r[2]; $cur = if ($idx.Count -gt 1) { [int]$idx[1] } else { -1 }; $next = if ($idx.Count -gt 2) { [int]$idx[2] } else { -1 }
    $sel = $lstRot.SelectedIndex; $lstRot.Items.Clear()
    for ($i = 0; $i -lt $n; $i++) {
        $m = $ml[3 + $i * $per]; $mode = $ml[4 + $i * $per]; $script:rotation += , @($m, $mode)
        $tag = if ($i -eq $cur) { '  << now' } elseif ($i -eq $next) { '  << next' } else { '' }
        [void]$lstRot.Items.Add("$(Map-Label $m) - $(Mode-Label $mode)$tag")
    }
    if ($sel -ge 0 -and $sel -lt $lstRot.Items.Count) { $lstRot.SelectedIndex = $sel }
}
function Fill-Maps {
    Load-Available
    $lstMaps.Items.Clear()
    foreach ($m in $available.Keys) { [void]$lstMaps.Items.Add((Map-Label $m)) }
    $i = [array]::IndexOf(@($available.Keys), 'mp_hills'); if ($i -ge 0) { $lstMaps.SelectedIndex = $i }
}
function Selected-Map { if ($lstMaps.SelectedIndex -lt 0) { return $null }; @($available.Keys)[$lstMaps.SelectedIndex] }
function Selected-Mode { if ($cmbMode.SelectedIndex -lt 0) { return $null }; $script:modeList[$cmbMode.SelectedIndex] }
$lstMaps.Add_SelectedIndexChanged({
    $m = Selected-Map; if (-not $m) { return }
    $script:modeList = @($available[$m] | Sort-Object { $i = [array]::IndexOf($botModes, $_); if ($i -lt 0) { 99 } else { $i } })
    $cmbMode.Items.Clear()
    foreach ($x in $script:modeList) { [void]$cmbMode.Items.Add((Mode-Label $x) + $(if ($botModes -contains $x) { '' } else { '  (bots just hunt)' })) }
    if ($cmbMode.Items.Count) { $cmbMode.SelectedIndex = 0 }
})

function Rcon-Do([object[]]$cmds, [string]$what) {
    $r = Invoke-Rcon $cmds
    if (-not $r) { Say "Can't reach the server - is it running?"; return $false }
    $bad = @($r | Where-Object { $_[0] -ne 'OK' })
    if ($bad.Count) { Say "$what failed: $($bad[0] -join ' ')"; return $false }
    Say "$what - done."; Refresh-Status; return $true
}
$btnNow.Add_Click({
    $m = Selected-Map; $mode = Selected-Mode; if (-not $m -or -not $mode) { return }
    # Already in the rotation: make that entry next. Otherwise put it first (keeping the rest). Then end the round into it.
    Refresh-Status
    for ($i = 0; $i -lt $script:rotation.Count; $i++) {
        if ($script:rotation[$i][0] -eq $m -and $script:rotation[$i][1] -eq $mode) {
            if (Rcon-Do @(, @('mapList.setNextMapIndex', "$i")) "Queued $(Map-Label $m) $(Mode-Label $mode)") {
                Rcon-Do @(, @('mapList.runNextRound')) "Switching to $(Map-Label $m), $(Mode-Label $mode) (loading takes a moment)" | Out-Null
            }
            return
        }
    }
    if (Rcon-Do @(@('mapList.add', $m, $mode, '1', '0'), @('mapList.setNextMapIndex', '0'), @('mapList.save')) "Queued $(Map-Label $m) $(Mode-Label $mode)") {
        Rcon-Do @(, @('mapList.runNextRound')) "Switching to $(Map-Label $m), $(Mode-Label $mode) (loading takes a moment)" | Out-Null
    }
})
$btnAdd.Add_Click({
    $m = Selected-Map; $mode = Selected-Mode; if (-not $m -or -not $mode) { return }
    Rcon-Do @(@('mapList.add', $m, $mode, '1'), @('mapList.save')) "Added $(Map-Label $m) $(Mode-Label $mode) to the rotation" | Out-Null
})
$btnRemove.Add_Click({ $i = $lstRot.SelectedIndex; if ($i -lt 0) { return }; Rcon-Do @(@('mapList.remove', "$i"), @('mapList.save')) 'Removed from the rotation' | Out-Null })
$btnSel.Add_Click({ $i = $lstRot.SelectedIndex; if ($i -lt 0) { return }; Rcon-Do @(, @('mapList.setNextMapIndex', "$i")) 'Set as the next map' | Out-Null })
$btnNext.Add_Click({ Rcon-Do @(, @('mapList.runNextRound')) 'Moving to the next map' | Out-Null })
$btnRound.Add_Click({ Rcon-Do @(, @('mapList.restartRound')) 'Restarting the round' | Out-Null })

function Start-Server {
    $bots = [int]$numBots.Value * 2
    Say "Starting BF-Portable and the server with $bots bots ($($numBots.Value) per team)..."
    $f.Refresh()
    # (Rob: "make the UI start the MySQL server") - before Blaze, which needs the database
    if (-not (Start-MySql)) { return }
    Start-Web
    Say "Starting the server with $bots bots ($($numBots.Value) per team)..."
    $ps = Join-Path $PSScriptRoot 'Start-Thugbot.ps1'
    Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$ps`"", '-Bots', "$bots" -WindowStyle Minimized | Out-Null
}
function Stop-Server {
    $r = Invoke-Rcon @(, @('admin.shutDown'))
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Process bfh_server_Final -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
    Get-Process bfh_server_Final -ErrorAction SilentlyContinue | Stop-Process -Force
}
$btnStart.Add_Click({ if (Get-Process bfh_server_Final -ErrorAction SilentlyContinue) { Say 'The server is already running (use Restart to change the bot count).'; return }; Start-Server })
$btnStop.Add_Click({ Say 'Stopping the server...'; $f.Refresh(); Stop-Server; Say 'Server stopped.'; Refresh-Status })
$btnRestart.Add_Click({ Say 'Restarting...'; $f.Refresh(); Stop-Server; Start-Sleep 2; Start-Server })
$btnMap.Add_Click({
    $p = Join-Path $server 'Thugbot_report\latest.html'
    if (Test-Path $p) { Start-Process $p } else { Say 'No learned map yet - it appears a minute after a round starts.' }
})

# ---- backend: MySQL (xampp), Apache (the server-list API the BF Launcher reads), Blaze emulator for Hardline ----
$xampp = Join-Path $Portable 'xampp'
function Svc-Text {
    $on = { param($n) if (Get-Process $n -ErrorAction SilentlyContinue) { 'on' } else { 'OFF' } }
    $lblSvc.Text = "MySQL $(& $on 'mysqld')   Web $(& $on 'httpd')   Blaze $(& $on 'BlazeServerH')"
    $lblSvc.ForeColor = if ($lblSvc.Text -match 'OFF') { [Drawing.Color]::DarkOrange } else { [Drawing.Color]::SeaGreen }
}
function Start-MySql {
    if (Get-Process mysqld -ErrorAction SilentlyContinue) { Say 'MySQL is already running.'; return $true }
    $exe = Join-Path $xampp 'mysql\bin\mysqld.exe'
    if (-not (Test-Path $exe)) { Say "MySQL not found: $exe"; return $false }
    # same as xampp\mysql_start.bat (run from the xampp folder: the ini path is relative), without its console window
    Start-Process -FilePath $exe -ArgumentList '--defaults-file=mysql\bin\my.ini', '--standalone' -WorkingDirectory $xampp -WindowStyle Hidden | Out-Null
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 500
        try { $c = New-Object Net.Sockets.TcpClient; $c.Connect('127.0.0.1', 3306); $c.Close(); Say 'MySQL started.'; Svc-Text; return $true } catch {}
        if (-not (Get-Process mysqld -ErrorAction SilentlyContinue)) { break }
    }
    Say 'MySQL did not come up (see xampp\mysql\data\*.err, or Repair-MySQL-Privileges.bat).'; Svc-Text; return $false
}
function Start-Web {
    if (Get-Process httpd -ErrorAction SilentlyContinue) { Say 'The web server is already running.'; return }
    $exe = Join-Path $xampp 'apache\bin\httpd.exe'
    if (-not (Test-Path $exe)) { Say "Apache not found: $exe"; return }
    Start-Process -FilePath $exe -WorkingDirectory $xampp -WindowStyle Hidden | Out-Null
    Start-Sleep 1; Say 'Web server started.'; Svc-Text
}
function Start-Blaze {
    if (Get-Process BlazeServerH -ErrorAction SilentlyContinue) { return }
    $dir = if ($cfg.Emulator) { $cfg.Emulator } else { Join-Path $Portable 'BFH-EMU' }
    Start-Process -FilePath (Join-Path $dir 'BlazeServerH.exe') -WorkingDirectory $dir | Out-Null
}
# Bots in vehicles (Thugbot.ini Vehicles=, off by default): saved in the ini and sent to the running server.
$iniPath = Join-Path $server 'Thugbot.ini'
function Ini-Get([string]$key, [string]$def) {
    if (Test-Path $iniPath) { foreach ($l in Get-Content $iniPath) { if ($l -match "^\s*$key\s*=\s*(.*)$") { return $Matches[1].Trim() } } }
    return $def
}
function Ini-Set([string]$key, [string]$val) {
    $lines = @(if (Test-Path $iniPath) { Get-Content $iniPath } else { '[Thugbot]' })
    $done = $false
    for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match "^\s*$key\s*=") { $lines[$i] = "$key=$val"; $done = $true } }
    if (-not $done) { $lines += '; Bots get in and drive vehicles (1) or stay on foot (0)'; $lines += "$key=$val" }
    Set-Content $iniPath $lines -Encoding ascii
}
$chkVeh.Checked = (Ini-Get 'Vehicles' '0') -eq '1'
$chkVeh.Add_CheckedChanged({
    $v = if ($chkVeh.Checked) { '1' } else { '0' }
    Ini-Set 'Vehicles' $v
    try { Set-Content (Join-Path $server 'Thugbot_cmd.txt') "vehicles $v" -Encoding ascii } catch {}
    Say $(if ($chkVeh.Checked) { 'Bots now use vehicles.' } else { 'Bots stay on foot (they get out of vehicles they spawn in).' })
})
$btnSql.Add_Click({ Start-MySql | Out-Null })
$btnWeb.Add_Click({ Start-Web })
$btnAll.Add_Click({ if (Start-MySql) { Start-Web; Start-Sleep 1; Start-Blaze; Say 'MySQL, web server and Blaze running.'; Svc-Text } })

# ---- LAN games: every host on this PC's /24 subnets that answers BF-Portable's server-list API (the same call the BF
# Launcher makes, with its API key). Hosts whose API refuses our key are listed too: they run BF-Portable, but their
# server list needs their own key.
$apiKey = ''
$launcherIni = if ($cfg.Launcher) { Join-Path (Split-Path $cfg.Launcher) 'Settings.ini' } else { Join-Path $Portable 'Client-Files\BF-Launcher\Settings.ini' }
if (Test-Path $launcherIni) { foreach ($l in Get-Content $launcherIni) { if ($l -match '^APIKey=(\S+)') { $apiKey = $Matches[1] } } }
$games = [ordered]@{ 'BFH' = 'get_servers_bfh.php'; 'BF4' = 'get_servers_bf4.php'; 'BF3' = 'get_servers_bf3.php'; 'BC2' = 'get_servers_bfbc.php'; 'MOHW' = 'get_servers_mohw.php' }
Add-Type -AssemblyName System.Net.Http
function Find-LanGames {
    $lvLan.Items.Clear(); Say 'Looking for games on the local network...'; $f.Refresh()
    $mine = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object { $_.IPAddress -notmatch '^(127|169\.254)\.' -and $_.PrefixOrigin -ne 'WellKnown' } | ForEach-Object { $_.IPAddress })
    $hosts = New-Object Collections.Generic.List[string]; $hosts.Add('127.0.0.1')
    foreach ($ip in $mine) { $b = $ip -replace '\.\d+$', ''; for ($i = 1; $i -le 254; $i++) { $h = "$b.$i"; if ($h -ne $ip -and -not $hosts.Contains($h)) { $hosts.Add($h) } } }
    $http = New-Object Net.Http.HttpClient; $http.Timeout = [TimeSpan]::FromMilliseconds(1500)
    # first pass: which hosts have a web server with BF-Portable's API (asks for the Hardline list)
    $tasks = @{}
    foreach ($h in $hosts) { $tasks[$h] = $http.GetAsync("http://$h/api/$($games['BFH'])?key=$apiKey") }
    try { [void][Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($tasks.Values), 2500) } catch {}
    $found = 0; $portable = @()
    foreach ($h in $hosts) {
        $t = $tasks[$h]
        if (-not $t.IsCompleted -or $t.IsFaulted -or $t.IsCanceled) { continue }
        $code = [int]$t.Result.StatusCode
        if ($code -eq 404) { continue }
        if ($code -eq 401 -or $code -eq 403) {
            $it = New-Object Windows.Forms.ListViewItem('?'); foreach ($x in @('BF-Portable host (its API key differs)', '', '', '', $h)) { [void]$it.SubItems.Add($x) }; [void]$lvLan.Items.Add($it); continue
        }
        $portable += $h
    }
    # second pass: every game's list on the hosts that answered
    foreach ($h in $portable) {
        foreach ($g in $games.Keys) {
            try { $body = $http.GetStringAsync("http://$h/api/$($games[$g])?key=$apiKey").Result } catch { continue }
            try { $list = $body | ConvertFrom-Json } catch { continue }
            foreach ($srv in @($list)) {
                if (-not $srv -or -not $srv.gnam) { continue }
                $it = New-Object Windows.Forms.ListViewItem($g)
                $map = if ($g -eq 'BFH') { Map-Label $srv.level } else { $srv.level }
                $mode = if ($g -eq 'BFH') { Mode-Label $srv.levellocation } else { $srv.levellocation }
                $where = if ($h -eq '127.0.0.1') { 'this PC' } else { $h }
                foreach ($x in @($srv.gnam, $map, $mode, $srv.PLAYERS, $where)) { [void]$it.SubItems.Add("$x") }
                [void]$lvLan.Items.Add($it); $found++
            }
        }
    }
    $http.Dispose()
    Say "Found $found game(s) on $($portable.Count) BF-Portable host(s) ($($hosts.Count) addresses checked). Join them from the BF Launcher."
}
$btnLan.Add_Click({ Find-LanGames })
$btnLaunch.Add_Click({
    $exe = if ($cfg.Launcher) { $cfg.Launcher } else { Join-Path $Portable 'Client-Files\BF-Launcher\BF Launcher.exe' }
    if (Test-Path $exe) { Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) } else { Say "Not found: $exe" }
})

# ---- Scan all maps: plays every map once (Conquest Large where it exists, else Conquest Small / TDM) until its grid is
# built and its saved routes stop changing, then moves on - so a map is ready (scanned, routes made and tested by the
# bots) before anyone plays it. A map that already has its grid just gets 90 s of bots running its routes.
$logPath = Join-Path $server 'Thugbot_25200.log'
$scan = @{ on = $false; queue = @(); i = 0; started = $null; level = ''; lastRouteAt = $null; built = $false }
function Log-Lines-Since($since) {
    if (-not (Test-Path $logPath)) { return @() }
    $lines = Get-Content $logPath -Tail 400
    return $lines | Where-Object { $_.Length -ge 8 -and $_.Substring(0, 8) -ge $since }
}
function Scan-Next {
    if ($scan.i -ge $scan.queue.Count) { $scan.on = $false; $btnScan.Text = 'Scan all maps'; Say "Map scan finished: $($scan.queue.Count) map(s) ready."; return }
    $m = $scan.queue[$scan.i]
    $r = Invoke-Rcon @(@('mapList.add', $m[0], $m[1], '1', '0'), @('mapList.setNextMapIndex', '0'), @('mapList.runNextRound'))
    if (-not $r) { $scan.on = $false; $btnScan.Text = 'Scan all maps'; Say 'Map scan stopped: the server is not answering.'; return }
    $scan.started = Get-Date; $scan.since = (Get-Date).ToString('HH:mm:ss'); $scan.level = "$($m[0])_$($m[1])".ToLower(); $scan.built = $false; $scan.lastRouteAt = Get-Date
    Say "Scanning $($scan.i + 1) of $($scan.queue.Count): $(Map-Label $m[0]), $(Mode-Label $m[1])..."
}
function Scan-Tick {
    if (-not $scan.on) { return }
    $age = ((Get-Date) - $scan.started).TotalSeconds
    $lines = Log-Lines-Since $scan.since
    foreach ($l in $lines) {
        if ($l -match "nav: (built|loaded) Thugbot_nav\\$([regex]::Escape($scan.level))" -or $l -match "nav: built $([regex]::Escape($scan.level))") { $scan.built = $true }
        if ($l -match 'routes: (spawn|flag)') { $scan.lastRouteAt = Get-Date }
    }
    $quiet = ((Get-Date) - $scan.lastRouteAt).TotalSeconds
    if (($scan.built -and $age -gt 90 -and $quiet -gt 25) -or $age -gt 360) {
        $scan.i++; Scan-Next
    } else {
        Say "Scanning $($scan.i + 1) of $($scan.queue.Count): $($scan.level) - grid $(if ($scan.built) { 'ready' } else { 'building' }), $([int]$age) s"
    }
}
$btnScan.Add_Click({
    if ($scan.on) { $scan.on = $false; $btnScan.Text = 'Scan all maps'; Say 'Map scan stopped.'; return }
    Load-Available
    $q = @()
    foreach ($m in $available.Keys) {
        $modes = @($available[$m])
        $mode = @('TurfWarLarge0', 'TurfWarSmall0', 'TeamDeathMatch0') | Where-Object { $modes -contains $_ } | Select-Object -First 1
        if ($mode) { $q += , @($m, $mode) }
    }
    if (-not $q.Count) { Say 'No maps to scan (is the server running?)'; return }
    $ok = [Windows.Forms.MessageBox]::Show("Play all $($q.Count) maps one after another (about 2 - 6 minutes each) so the bots scan them and test their routes?`nAnyone on the server goes along for the ride.", 'Scan all maps', 'YesNo')
    if ($ok -ne 'Yes') { return }
    $scan.queue = $q; $scan.i = 0; $scan.on = $true; $btnScan.Text = 'Stop the map scan'
    Scan-Next
})
$scanTimer = New-Object Windows.Forms.Timer; $scanTimer.Interval = 5000; $scanTimer.Add_Tick({ Scan-Tick }); $scanTimer.Start()

$timer = New-Object Windows.Forms.Timer; $timer.Interval = 5000; $timer.Add_Tick({ Refresh-Status; Svc-Text }); $timer.Start()
if ($env:THUGBOT_CONTROL_TEST) {   # self-check without showing the window
    Fill-Maps; Refresh-Status
    "status: $($status.Text)"; "maps: $($lstMaps.Items.Count), selected $($lstMaps.SelectedItem), modes: $(@($cmbMode.Items) -join ', ')"
    "rotation: $(@($lstRot.Items) -join ' / ')"
    Svc-Text; "services: $($lblSvc.Text)"; "bots use vehicles: $($chkVeh.Checked)"
    Find-LanGames; "lan: $($log.Text)"; foreach ($it in $lvLan.Items) { '  ' + (@($it.SubItems | ForEach-Object { $_.Text }) -join ' | ') }
    return
}
$f.Add_Shown({ Fill-Maps; Refresh-Status; Svc-Text })
[void]$f.ShowDialog()
