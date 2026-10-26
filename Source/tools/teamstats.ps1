# Per-team kills / deaths / score from RCON listPlayers, and tickets (serverInfo). Usage: tools\teamstats.ps1
$r = Join-Path $PSScriptRoot 'rcon.ps1'
. (Join-Path (Split-Path $PSScriptRoot) 'Thugbot-Paths.ps1')
$bat = Get-Content (Join-Path (Get-ThugbotConfig -NoSearch).Server '!StartServer-Bots.bat') -Raw
$pw = if ($bat -match '(?m)^set ADMINPASSWORD=(\S+)') { $Matches[1] } else { '' }   # (the server's own password, not written here)
$o = & $r -Password $pw -Command 'listPlayers all' | Out-String
$w = $o.Trim() -split ' \| '
$nf = [int]$w[1]; $n = [int]$w[2 + $nf]; $base = 3 + $nf; $t = @{}
for ($i = 0; $i -lt $n; $i++) {
    $row = $w[($base + $i * $nf)..($base + $i * $nf + $nf - 1)]
    $team = $row[2]; if (-not $t[$team]) { $t[$team] = @(0, 0, 0, 0) }
    $t[$team][0]++; $t[$team][1] += [int]$row[4]; $t[$team][2] += [int]$row[5]; $t[$team][3] += [int]$row[6]
}
$t.GetEnumerator() | Sort-Object Key | ForEach-Object { "team $($_.Key): $($_.Value[0]) players, kills $($_.Value[1]), deaths $($_.Value[2]), score $($_.Value[3])" }
$si = (& $r -Password $pw -Command 'serverInfo' | Out-String).Trim() -split ' \| '
"tickets: team 1 $($si[9]), team 2 $($si[10]) - map $($si[5]) $($si[4])"
