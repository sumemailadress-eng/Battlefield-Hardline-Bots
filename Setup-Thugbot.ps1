# Thugbot setup: finds (or lets you pick) the BF-Portable backend, the Hardline server, the Blaze emulator, the BF
# Launcher and the Hardline game, saves them in Thugbot-Paths.ini, installs the bots into the server and puts
# shortcuts on the desktop. Run Setup-Thugbot.bat. Uninstall: the button here, or Uninstall-Thugbot.ps1.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
. (Join-Path $PSScriptRoot 'Thugbot-Paths.ps1')
$cfg = Get-ThugbotConfig -NoSearch

# What each field is, the file that proves it is the right folder, and whether it is needed.
$fields = @(
    @{ Key = 'Portable'; Label = 'BF-Portable backend folder (PortableBF.exe)'; File = 'PortableBF.exe'; Need = $true; IsFile = $false }
    @{ Key = 'Server'; Label = 'Hardline server folder (bfh_server_Final.exe)'; File = 'bfh_server_Final.exe'; Need = $true; IsFile = $false }
    @{ Key = 'Emulator'; Label = 'Hardline Blaze emulator folder (BlazeServerH.exe)'; File = 'BlazeServerH.exe'; Need = $true; IsFile = $false }
    @{ Key = 'Launcher'; Label = 'BF Launcher (BF Launcher.exe) - optional'; File = 'BF Launcher.exe'; Need = $false; IsFile = $true }
    @{ Key = 'Client'; Label = 'Hardline game folder (bfh.exe) - optional'; File = 'bfh.exe'; Need = $false; IsFile = $false }
)

$form = New-Object Windows.Forms.Form
$form.Text = 'Thugbot setup'; $form.Size = New-Object Drawing.Size(720, 470); $form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'; $form.MaximizeBox = $false
$y = 12; $boxes = @{}; $marks = @{}
foreach ($f in $fields) {
    $l = New-Object Windows.Forms.Label; $l.Text = $f.Label; $l.Location = New-Object Drawing.Point(12, $y); $l.AutoSize = $true; $form.Controls.Add($l)
    $tb = New-Object Windows.Forms.TextBox; $tb.Location = New-Object Drawing.Point(12, ($y + 20)); $tb.Width = 520; $tb.Text = $cfg[$f.Key]; $form.Controls.Add($tb)
    $m = New-Object Windows.Forms.Label; $m.Location = New-Object Drawing.Point(538, ($y + 23)); $m.Width = 30; $form.Controls.Add($m)
    $b = New-Object Windows.Forms.Button; $b.Text = 'Browse...'; $b.Location = New-Object Drawing.Point(575, ($y + 18)); $b.Width = 110; $form.Controls.Add($b)
    $b.Tag = $f
    $b.Add_Click({
        $fd = $this.Tag
        if ($fd.IsFile) {
            $d = New-Object Windows.Forms.OpenFileDialog; $d.Filter = "$($fd.File)|$($fd.File)|Programs (*.exe)|*.exe"
            if ($d.ShowDialog() -eq 'OK') { $boxes[$fd.Key].Text = $d.FileName }
        } else {
            $d = New-Object Windows.Forms.FolderBrowserDialog; $d.Description = $fd.Label
            if ($boxes[$fd.Key].Text -and (Test-Path $boxes[$fd.Key].Text)) { $d.SelectedPath = $boxes[$fd.Key].Text }
            if ($d.ShowDialog() -eq 'OK') { $boxes[$fd.Key].Text = $d.SelectedPath }
        }
        Check-All
    })
    $boxes[$f.Key] = $tb; $marks[$f.Key] = $m
    $tb.Add_TextChanged({ Check-All })
    $y += 52
}
$ln = New-Object Windows.Forms.Label; $ln.Text = 'Your in-game name (for the route hotkeys F5 - F11; empty = no hotkeys)'; $ln.Location = New-Object Drawing.Point(12, $y); $ln.AutoSize = $true; $form.Controls.Add($ln)
$tbName = New-Object Windows.Forms.TextBox; $tbName.Location = New-Object Drawing.Point(12, ($y + 20)); $tbName.Width = 250; $tbName.Text = $cfg.Name; $form.Controls.Add($tbName)
$cbShort = New-Object Windows.Forms.CheckBox; $cbShort.Text = 'Desktop shortcuts (Thugbot Control, Start Thugbot)'; $cbShort.Location = New-Object Drawing.Point(290, ($y + 20)); $cbShort.AutoSize = $true; $cbShort.Checked = $true; $form.Controls.Add($cbShort)
$y += 56
$status = New-Object Windows.Forms.Label; $status.Location = New-Object Drawing.Point(12, $y); $status.Size = New-Object Drawing.Size(680, 36); $form.Controls.Add($status)
$y += 40
$bDetect = New-Object Windows.Forms.Button; $bDetect.Text = 'Auto-detect'; $bDetect.Location = New-Object Drawing.Point(12, $y); $bDetect.Width = 120; $form.Controls.Add($bDetect)
$bInstall = New-Object Windows.Forms.Button; $bInstall.Text = 'Save and install'; $bInstall.Location = New-Object Drawing.Point(300, $y); $bInstall.Width = 130; $form.Controls.Add($bInstall)
$bUninst = New-Object Windows.Forms.Button; $bUninst.Text = 'Uninstall'; $bUninst.Location = New-Object Drawing.Point(440, $y); $bUninst.Width = 110; $form.Controls.Add($bUninst)
$bClose = New-Object Windows.Forms.Button; $bClose.Text = 'Close'; $bClose.Location = New-Object Drawing.Point(575, $y); $bClose.Width = 110; $form.Controls.Add($bClose)

function Say([string]$t) { $status.Text = $t; $form.Refresh() }
function Field-Ok($f) {
    $v = $boxes[$f.Key].Text.Trim()
    if (-not $v) { return -not $f.Need }
    if ($f.IsFile) { return (Test-Path $v -PathType Leaf) }
    return (Test-Path (Join-Path $v $f.File))
}
function Check-All {
    $ok = $true
    foreach ($f in $fields) {
        $good = Field-Ok $f
        $marks[$f.Key].Text = if (-not $boxes[$f.Key].Text.Trim()) { if ($f.Need) { 'x' } else { '' } } elseif ($good) { 'OK' } else { 'x' }
        $marks[$f.Key].ForeColor = if ($good) { [Drawing.Color]::Green } else { [Drawing.Color]::Red }
        if (-not $good) { $ok = $false }
    }
    $bInstall.Enabled = $ok
    return $ok
}

$bDetect.Add_Click({
    $form.Cursor = 'WaitCursor'
    try {
        foreach ($f in $fields) {
            if ((Field-Ok $f) -and $boxes[$f.Key].Text.Trim()) { continue }
            Say "Looking for $($f.File) on your drives (this can take a minute)..."
            $near = @()
            $p = $boxes['Portable'].Text.Trim()
            if ($p) { $near = @((Join-Path $p 'Battlefield_BFH_Server'), (Join-Path $p 'BFH-EMU'), (Join-Path $p 'Client-Files\BF-Launcher')) }
            $dir = Find-ThugbotFile $f.File $near $(if ($f.Key -eq 'Portable') { 'Battlefield_BFH_Server' } else { '' })
            if ($dir) { $boxes[$f.Key].Text = if ($f.IsFile) { Join-Path $dir $f.File } else { $dir } }
        }
        if (Check-All) { Say 'Found everything needed. Check the folders, then "Save and install".' }
        else { Say 'Some folders were not found (marked x): use Browse to point at them.' }
    } finally { $form.Cursor = 'Default' }
})

$bInstall.Add_Click({
    if (-not (Check-All)) { Say 'Fix the folders marked x first.'; return }
    $c = @{ Name = $tbName.Text.Trim() }
    foreach ($f in $fields) { $c[$f.Key] = $boxes[$f.Key].Text.Trim() }
    if (Get-Process bfh_server_Final -ErrorAction SilentlyContinue) { Say 'The Hardline server is running: close it first, then install.'; return }
    try {
        Write-ThugbotIni $c
        Say 'Installing the bots into the server...'
        & (Join-Path $PSScriptRoot 'Install-Thugbot.ps1') -Server $c.Server | Out-Null
        if ($cbShort.Checked) {
            $sh = New-Object -ComObject WScript.Shell; $desk = [Environment]::GetFolderPath('Desktop')
            foreach ($s in @(@('Thugbot Control', 'Thugbot-Control.bat'), @('Start Thugbot', 'Start-Thugbot.bat'))) {
                $lnk = $sh.CreateShortcut((Join-Path $desk "$($s[0]).lnk")); $lnk.TargetPath = Join-Path $PSScriptRoot $s[1]; $lnk.WorkingDirectory = $PSScriptRoot; $lnk.Save()
            }
        }
        Say "Installed. Settings saved in Thugbot-Paths.ini. Start with Thugbot Control (or Start-Thugbot.bat)."
    } catch { Say "Install failed: $($_.Exception.Message)" }
})

$bUninst.Add_Click({
    $srv = $boxes['Server'].Text.Trim()
    if (-not $srv -or -not (Test-Path $srv)) { Say 'Set the Hardline server folder first.'; return }
    if ([Windows.Forms.MessageBox]::Show('Remove Thugbot from the server and put the original xinput9_1_0.dll back? (What the bots learned stays in the server folder.)', 'Thugbot', 'YesNo') -ne 'Yes') { return }
    try { & (Join-Path $PSScriptRoot 'Install-Thugbot.ps1') -Server $srv -Uninstall | Out-Null; Say 'Thugbot removed from the server.' }
    catch { Say "Uninstall failed: $($_.Exception.Message)" }
})
$bClose.Add_Click({ $form.Close() })

[void](Check-All)
if (-not $cfg.Portable -and -not $cfg.Server) { $form.Add_Shown({ $bDetect.PerformClick() }) }
else { Say 'Saved folders loaded. Auto-detect fills in any that are missing.' }
[void]$form.ShowDialog()
