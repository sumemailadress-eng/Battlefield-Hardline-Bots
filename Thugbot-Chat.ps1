# Thugbot chat bridge: listens to the server's chat over RCON events and passes "!" commands (the in-game route editor:
# !wp !rec !stop !undo !clear !save !delete !routes) to the bot core; sends the core's replies (Thugbot_say.txt) back to
# the player with admin.say. Started (hidden) by Start-Thugbot.ps1; quits a minute after the server is gone.
param([string]$Portable = '')   # (empty: from Thugbot-Paths.ini)
. (Join-Path $PSScriptRoot 'Thugbot-Paths.ps1')
$cfg = Get-ThugbotConfig -Quiet
if (-not $Portable) { $Portable = $cfg.Portable }
$server = if ($cfg.Server) { $cfg.Server } else { Join-Path $Portable 'Battlefield_BFH_Server' }
$bat = Get-Content (Join-Path $server '!StartServer-Bots.bat') -Raw
$port = if ($bat -match '(?m)^set REMOTEADMINPORT=(\d+)') { [int]$Matches[1] } else { 47600 }
$pass = if ($bat -match '(?m)^set ADMINPASSWORD=(\S+)') { $Matches[1] } else { '' }
$cmdFile = Join-Path $server 'Thugbot_cmd.txt'
$sayFile = Join-Path $server 'Thugbot_say.txt'
$logFile = Join-Path $server 'Thugbot_chat.log'
function Log($t) { Add-Content $logFile "$(Get-Date -Format 'HH:mm:ss') $t" }

function Packet([int]$seq, [string[]]$words, [bool]$response = $false) {
    $body = New-Object IO.MemoryStream
    foreach ($w in $words) { $b = [Text.Encoding]::ASCII.GetBytes($w); $body.Write([BitConverter]::GetBytes([int]$b.Length), 0, 4); $body.Write($b, 0, $b.Length); $body.WriteByte(0) }
    $wb = $body.ToArray()
    $s = $seq -band 0x3fffffff; if ($response) { $s = $s -bor 0x40000000 }
    return [BitConverter]::GetBytes([int]$s) + [BitConverter]::GetBytes([int](12 + $wb.Length)) + [BitConverter]::GetBytes([int]$words.Count) + $wb
}
function Parse([byte[]]$buf, [int]$len) {   # complete packets in buf[0..len): returns @(packets, consumed)
    $out = @(); $o = 0
    while ($len - $o -ge 12) {
        $size = [BitConverter]::ToInt32($buf, $o + 4)
        if ($size -lt 12 -or $len - $o -lt $size) { break }
        $seq = [BitConverter]::ToInt32($buf, $o); $count = [BitConverter]::ToInt32($buf, $o + 8); $p = $o + 12; $words = @()
        for ($i = 0; $i -lt $count; $i++) { $l = [BitConverter]::ToInt32($buf, $p); $words += [Text.Encoding]::UTF8.GetString($buf, $p + 4, $l); $p += 5 + $l }
        $out += , @($seq, $words); $o += $size
    }
    return , @($out, $o)
}
function Say($player, $text) {
    try {
        $c = New-Object Net.Sockets.TcpClient('127.0.0.1', $port); $s = $c.GetStream()
        $p = Packet 1 @('login.plainText', $pass); $s.Write($p, 0, $p.Length); Start-Sleep -Milliseconds 100
        $p = Packet 2 @('admin.say', $text, 'player', $player); $s.Write($p, 0, $p.Length); Start-Sleep -Milliseconds 100
        $c.Close()
    } catch { }
}
function Add-Command($line) {
    for ($k = 0; $k -lt 10; $k++) { try { Add-Content $cmdFile $line -Encoding ascii -ErrorAction Stop; return } catch { Start-Sleep -Milliseconds 50 } }
}

$goneSince = $null
while ($true) {
    if (-not (Get-Process bfh_server_Final -ErrorAction SilentlyContinue)) {
        if (-not $goneSince) { $goneSince = Get-Date }
        if (((Get-Date) - $goneSince).TotalSeconds -gt 60) { Log 'server gone - quitting'; break }
        Start-Sleep 5; continue
    }
    $goneSince = $null
    try {
        $c = New-Object Net.Sockets.TcpClient('127.0.0.1', $port); $s = $c.GetStream(); $seq = 1
        foreach ($w in @(@('login.plainText', $pass), @('admin.eventsEnabled', 'true'))) { $p = Packet $seq $w; $s.Write($p, 0, $p.Length); $seq++ }
        Log 'connected, events on'
        $buf = New-Object byte[] 65536; $len = 0
        while ($c.Connected) {
            if ($s.DataAvailable) {
                $n = $s.Read($buf, $len, $buf.Length - $len); if ($n -le 0) { break }; $len += $n
                $r = Parse $buf $len; $pk = $r[0]; $used = $r[1]
                if ($used -gt 0) { [Array]::Copy($buf, $used, $buf, 0, $len - $used); $len -= $used }
                foreach ($q in $pk) {
                    $words = $q[1]
                    if ($words[0] -eq 'player.onChat' -and $words.Count -ge 3 -and $words[2].StartsWith('!')) {
                        $line = "chat $($words[1])|$($words[2])"
                        Log $line; Add-Command $line
                    }
                    if (($q[0] -band 0x40000000) -eq 0 -and $words.Count -ge 1 -and $words[0] -match '^(player|server|punkBuster)\.') { $resp = Packet ($q[0] -band 0x3fffffff) @('OK') $true; $s.Write($resp, 0, $resp.Length) }   # (events from the server are requests: answer OK)
                }
            } else { Start-Sleep -Milliseconds 100 }
            if (Test-Path $sayFile) {
                $lines = @(); try { $lines = Get-Content $sayFile -ErrorAction Stop; Remove-Item $sayFile -ErrorAction Stop } catch { }
                foreach ($l in $lines) { $i = $l.IndexOf('|'); if ($i -gt 0) { Say $l.Substring(0, $i) $l.Substring($i + 1) } }
            }
        }
        $c.Close()
    } catch { Log "error: $($_.Exception.Message)"; Start-Sleep 5 }
}
