# Thugbot route hotkeys: F-keys on this PC send the in-game route editor's chat commands straight to the bot core
# (no typing in chat). Works while the game has focus. Replies still come up in game chat.
#   F5  start recording (a point every 3 m)        F6  stop and save the route
#   F7  add one point where you are                F8  undo the last point
#   F9  clear the points                           F10 elevator / ladder end (press at the bottom, then the top)
#   F11 bots in your vehicle get out
# Started (hidden) by Start-Thugbot.ps1; Thugbot-Keys.bat starts it by hand. Quits a minute after the server is gone.
param([string]$Portable = '', [string]$Player = '')   # (empty: from Thugbot-Paths.ini)
. (Join-Path $PSScriptRoot 'Thugbot-Paths.ps1')
$cfg = Get-ThugbotConfig -Quiet
if (-not $Portable) { $Portable = $cfg.Portable }
if (-not $Player) { $Player = $cfg.Name }
if (-not $Player) { return }   # (no name set: no hotkeys)
$server = if ($cfg.Server) { $cfg.Server } else { Join-Path $Portable 'Battlefield_BFH_Server' }
$cmdFile = Join-Path $server 'Thugbot_cmd.txt'
$logFile = Join-Path $server 'Thugbot_keys.log'
function Log($t) { Add-Content $logFile "$(Get-Date -Format 'HH:mm:ss') $t" }
function Send([string[]]$texts) {
    foreach ($t in $texts) {
        for ($k = 0; $k -lt 10; $k++) { try { Add-Content $cmdFile "chat $Player|$t" -Encoding ascii -ErrorAction Stop; break } catch { Start-Sleep -Milliseconds 50 } }
    }
    Log ($texts -join ' ')
}

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ThugbotHotkeys {
    [DllImport("user32.dll")] public static extern bool RegisterHotKey(IntPtr hWnd, int id, uint mods, uint vk);
    [DllImport("user32.dll")] public static extern bool UnregisterHotKey(IntPtr hWnd, int id);
    [DllImport("user32.dll")] public static extern bool PeekMessage(out MSG msg, IntPtr hWnd, uint min, uint max, uint remove);
    [StructLayout(LayoutKind.Sequential)] public struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam; public IntPtr lParam; public uint time; public int x; public int y; }
}
'@

$keys = [ordered]@{
    0x74 = @('!rec')                 # F5
    0x75 = @('!stop', '!save')       # F6
    0x76 = @('!wp')                  # F7
    0x77 = @('!undo')                # F8
    0x78 = @('!clear')               # F9
    0x79 = @('!lift')                # F10
    0x7A = @('!out')                 # F11
}
$id = 1; $byId = @{}
foreach ($vk in $keys.Keys) {
    if ([ThugbotHotkeys]::RegisterHotKey([IntPtr]::Zero, $id, 0x4000, [uint32]$vk)) { $byId[$id] = $keys[$vk] }   # (0x4000: no auto-repeat)
    else { Log "F-key $('{0:X}' -f $vk) is taken by another program" }
    $id++
}
Log "hotkeys on for $Player ($($byId.Count) keys)"
$goneSince = $null
try {
    while ($true) {
        $msg = New-Object ThugbotHotkeys+MSG
        while ([ThugbotHotkeys]::PeekMessage([ref]$msg, [IntPtr]::Zero, 0x0312, 0x0312, 1)) {   # WM_HOTKEY
            $k = [int]$msg.wParam
            if ($byId.ContainsKey($k)) { Send $byId[$k] }
        }
        if (-not (Get-Process bfh_server_Final -ErrorAction SilentlyContinue)) {
            if (-not $goneSince) { $goneSince = Get-Date }
            if (((Get-Date) - $goneSince).TotalSeconds -gt 60) { Log 'server gone - quitting'; break }
        } else { $goneSince = $null }
        Start-Sleep -Milliseconds 50
    }
} finally { foreach ($k in $byId.Keys) { [void][ThugbotHotkeys]::UnregisterHotKey([IntPtr]::Zero, $k) } }
