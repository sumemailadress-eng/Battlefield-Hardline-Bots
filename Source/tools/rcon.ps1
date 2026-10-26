# Minimal Frostbite RCON client: .\rcon.ps1 -Port 47600 -Password pw -Command 'listPlayers all'
param([string]$HostName = '127.0.0.1', [int]$Port = 47600, [string]$Password, [string[]]$Command = @('listPlayers all'))

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

$c = New-Object Net.Sockets.TcpClient($HostName, $Port); $s = $c.GetStream(); $seq = 1
if ($Password) { Send-Packet $s $seq ('login.plainText', $Password); $seq++; (Read-Packet $s) -join ' ' | Out-Null }
foreach ($cmd in $Command) {
    Send-Packet $s $seq ($cmd -split ' '); $seq++
    (Read-Packet $s) -join ' | '
}
$c.Close()
