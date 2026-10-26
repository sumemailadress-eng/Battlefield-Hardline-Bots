# Where things are on this PC: read by every Thugbot script (dot-source it, then Get-ThugbotConfig). Setup-Thugbot.bat
# writes Thugbot-Paths.ini next to this file; with no file (or an empty entry) the folders are looked for on the
# fixed drives once and the answer is written there.
#   [Paths]
#   Portable=<folder with PortableBF.exe: the MySQL / web backend>
#   Server=<folder with bfh_server_Final.exe: the Hardline dedicated server>
#   Emulator=<folder with BlazeServerH.exe: the Hardline Blaze emulator>
#   Launcher=<BF Launcher.exe, optional: the game launcher, for "Join" and the LAN game list's API key>
#   Client=<folder with bfh.exe, optional: the Hardline game you play on>
#   [Player]
#   Name=<your in-game name, for the route hotkeys F5 - F11>
$script:ThugbotPathsIni = Join-Path $PSScriptRoot 'Thugbot-Paths.ini'

function Read-ThugbotIni([string]$path) {
    $h = @{}
    if (-not (Test-Path $path)) { return $h }
    foreach ($line in Get-Content $path) {
        if ($line -match '^\s*([A-Za-z]+)\s*=\s*(.*?)\s*$') { $h[$Matches[1]] = $Matches[2] }
    }
    return $h
}

function Write-ThugbotIni([hashtable]$c) {
    @(
        '; Thugbot: where things are on this PC (Setup-Thugbot.bat writes this; edit it or run Setup again)'
        '[Paths]'
        "Portable=$($c.Portable)"
        "Server=$($c.Server)"
        "Emulator=$($c.Emulator)"
        "Launcher=$($c.Launcher)"
        "Client=$($c.Client)"
        '[Player]'
        "Name=$($c.Name)"
    ) | Set-Content $script:ThugbotPathsIni -Encoding ascii
}

# Looks for a file on the fixed drives: common places first (drive roots, Games folders, Program Files), up to 4
# folders deep. Returns the folder holding it, or ''.
function Find-ThugbotFile([string]$name, [string[]]$near = @(), [string]$withSub = '') {
    foreach ($n in $near) { if ($n -and (Test-Path (Join-Path $n $name))) { return $n } }
    $drives = Get-PSDrive -PSProvider FileSystem | Where-Object { $_.Root -and (Test-Path $_.Root) -and $_.Free -ne $null } | ForEach-Object { $_.Root }
    $starts = foreach ($d in $drives) { $d; Join-Path $d 'Games'; Join-Path $d 'Program Files'; Join-Path $d 'Program Files (x86)'; Join-Path $d 'Program Files (x86)\Origin Games'; Join-Path $d 'Program Files\EA Games' }
    $first = ''
    foreach ($s in $starts) {
        if (-not (Test-Path $s)) { continue }
        # (every copy, not just the first: a downloaded update folder can hold a second PortableBF.exe - the one with
        # the server folder next to it is the real install)
        foreach ($hit in Get-ChildItem -Path $s -Filter $name -File -Recurse -Depth 4 -ErrorAction SilentlyContinue) {
            if (-not $withSub -or (Test-Path (Join-Path $hit.DirectoryName $withSub))) { return $hit.DirectoryName }
            if (-not $first) { $first = $hit.DirectoryName }
        }
    }
    return $first
}

# Fills in what the ini leaves empty (by looking), keeps what it has. -NoSearch: only the ini.
function Get-ThugbotConfig([switch]$NoSearch, [switch]$Quiet) {
    $c = Read-ThugbotIni $script:ThugbotPathsIni
    foreach ($k in 'Portable', 'Server', 'Emulator', 'Launcher', 'Client', 'Name') { if (-not $c.ContainsKey($k)) { $c[$k] = '' } }
    $changed = $false
    if (-not $NoSearch) {
        if (-not $c.Portable) { if (-not $Quiet) { Write-Host 'Looking for PortableBF.exe...' }; $c.Portable = Find-ThugbotFile 'PortableBF.exe' @() 'Battlefield_BFH_Server'; $changed = $true }
        if (-not $c.Server) {
            if (-not $Quiet) { Write-Host 'Looking for the Hardline server (bfh_server_Final.exe)...' }
            $c.Server = Find-ThugbotFile 'bfh_server_Final.exe' @($(if ($c.Portable) { Join-Path $c.Portable 'Battlefield_BFH_Server' })); $changed = $true
        }
        if (-not $c.Emulator) { $c.Emulator = Find-ThugbotFile 'BlazeServerH.exe' @($(if ($c.Portable) { Join-Path $c.Portable 'BFH-EMU' })); $changed = $true }
        if (-not $c.Launcher -and $c.Portable) {
            $l = Join-Path $c.Portable 'Client-Files\BF-Launcher\BF Launcher.exe'
            if (Test-Path $l) { $c.Launcher = $l; $changed = $true }
        }
        if ($changed -and ($c.Portable -or $c.Server)) { Write-ThugbotIni $c }
    }
    return $c
}

# Stops a script with a clear message when a folder it needs is not known.
function Assert-ThugbotPath([hashtable]$c, [string]$key, [string]$what) {
    if (-not $c[$key] -or -not (Test-Path $c[$key])) { throw "$what not found. Run Setup-Thugbot.bat and point it at the folder (or edit Thugbot-Paths.ini)." }
}
