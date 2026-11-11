# Starts XAMPP MySQL the safe way (used by Thugbot Control and the FloodBots launchers).
# Why: when mysqld runs hidden with no console, its log lines get written INTO the privilege table
# files and corrupt them (MySQL then refuses to start). So this:
#   1. checks the mysql system tables and restores any broken one from XAMPP's clean copy (backup first),
#   2. starts mysqld through xampp\mysql_start.bat in its own (minimized) console window,
#   3. waits until port 3306 answers.
# Exit code 0 = MySQL is running.
param([string]$Portable = $PSScriptRoot, [int]$WaitSeconds = 40)
$ErrorActionPreference = 'Continue'
$xampp   = Join-Path $Portable 'xampp'
$data    = Join-Path $xampp 'mysql\data'
$sys     = Join-Path $data 'mysql'
$clean   = Join-Path $xampp 'mysql\backup\mysql'
$ariaChk = Join-Path $xampp 'mysql\bin\aria_chk.exe'

function Test-Port { try { $c = New-Object Net.Sockets.TcpClient; $c.Connect('127.0.0.1', 3306); $c.Close(); $true } catch { $false } }

if (Get-Process mysqld -ErrorAction SilentlyContinue) {
    if (Test-Port) { Write-Host 'MySQL is already running.'; exit 0 }
}

# 1. check + repair system tables (only while mysqld is stopped)
if (-not (Get-Process mysqld -ErrorAction SilentlyContinue) -and (Test-Path $ariaChk)) {
    Write-Host 'Checking MySQL system tables...'
    $bad = @()
    Push-Location $data
    foreach ($f in Get-ChildItem (Join-Path $sys '*.MAI')) {
        $out = & $ariaChk -s -e ("mysql\" + $f.BaseName) 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0 -or $out -match 'error|warning|corrupt') { $bad += $f.BaseName }
    }
    if ($bad) {
        $bk = Join-Path $xampp ("mysql\mysql-autofix-backup-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
        New-Item -ItemType Directory $bk -Force | Out-Null
        foreach ($t in $bad) {
            Copy-Item (Join-Path $sys "$t.*") $bk -Force
            # global_priv holds the user accounts (bf4, api): repair it in place, never reset it
            if ($t -ne 'global_priv' -and (Test-Path (Join-Path $clean "$t.MAI"))) {
                Copy-Item (Join-Path $clean "$t.MAD"), (Join-Path $clean "$t.MAI") $sys -Force
                Write-Host "  restored mysql.$t from XAMPP's clean copy"
            } else {
                & $ariaChk -r -f ("mysql\" + $t) | Out-Null
                Write-Host "  repaired mysql.$t"
            }
        }
        Write-Host "  (originals saved in $bk)"
    }
    Pop-Location
}

# 2. start through mysql_start.bat in a real console window (keeps the error log out of the tables)
Write-Host 'Starting MySQL...'
Start-Process -FilePath 'cmd.exe' -ArgumentList '/c', 'mysql_start.bat' -WorkingDirectory $xampp -WindowStyle Minimized | Out-Null

# 3. wait for the port
for ($i = 0; $i -lt $WaitSeconds; $i++) {
    Start-Sleep 1
    if (Test-Port) { Write-Host 'MySQL started.'; exit 0 }
}
Write-Host "MySQL did not come up - see $data\mysql_error.log"
exit 1
