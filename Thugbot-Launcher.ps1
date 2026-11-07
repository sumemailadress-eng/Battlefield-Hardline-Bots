# Thugbot Launcher: one window for the whole Hardline bot setup - orange and white, with police lights (the FloodBots
# Launcher made for Battlefield Hardline).
#   - starts the backend: MySQL, the web server (Apache), the redirector and the Hardline emulator (BlazeServerH)
#   - Servers 1 - 4 (BF-Portable's !StartServer.bat ... !StartServer4.bat): start / stop each, bots per server,
#     and each server's own map list (Instance*\Admin\MapList.txt) with the mode per map
#   - JOIN: starts the game straight into the selected server (the same bfhlan:// link the web server browser uses)
# Thugbot-UseOut.txt next to this file (optional): while it exists, out\ThugbotCore.dll is installed as it is, no rebuild.
# Run Thugbot-Launcher.bat. THUGBOT_LAUNCHER_TEST=1: reads everything and prints what it found, starts nothing.
param([string]$Portable = '')
$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'Thugbot-Paths.ps1')
$cfg = Get-ThugbotConfig -Quiet
if (-not $Portable) { $Portable = $cfg.Portable }
$server = if ($cfg.Server) { $cfg.Server } else { Join-Path $Portable 'Battlefield_BFH_Server' }
$emuDir = if ($cfg.Emulator) { $cfg.Emulator } else { Join-Path $Portable 'BFH-EMU' }
$xampp = Join-Path $Portable 'xampp'
$redirDir = Join-Path $Portable 'Redirector'
$clientDir = $cfg.Client
$reportDir = Join-Path $server 'Thugbot_report'
$map3dDir = Join-Path $PSScriptRoot 'tools\Map3D'; $map3dPort = 8643
$libDir = Join-Path $PSScriptRoot 'lib\WebView2'
$webData = Join-Path $env:LOCALAPPDATA 'Thugbot\WebView2'
$launcherDir = if ($cfg.Launcher) { Split-Path $cfg.Launcher } else { Join-Path $Portable 'Client-Files\BF-Launcher' }

# ---------------------------------------------------------------- launcher settings (bots per server etc.)
$setPath = Join-Path $PSScriptRoot 'Thugbot-Launcher.ini'
$set = @{ Selected = '1'; Persona = ''; Bots1 = '63'; Bots2 = '63'; Bots3 = '63'; Bots4 = '63'; BotsOn1 = '1'; BotsOn2 = '1'; BotsOn3 = '1'; BotsOn4 = '1' }
if (Test-Path $setPath) { foreach ($l in Get-Content $setPath) { if ($l -match '^\s*([A-Za-z0-9]+)\s*=\s*(.*?)\s*$') { $set[$Matches[1]] = $Matches[2] } } }
function Save-Settings {
    $lines = @('; Thugbot Launcher settings (the launcher writes this)')
    foreach ($k in ($set.Keys | Sort-Object)) { $lines += "$k=$($set[$k])" }
    try { Set-Content $setPath $lines -Encoding ascii } catch {}
}

# ---------------------------------------------------------------- maps and modes
$mapNames = [ordered]@{
    'mp_bank' = 'Bank Job'; 'mp_bloodout' = 'The Block'; 'mp_desert05' = 'Dust Bowl'; 'mp_downtown' = 'Downtown'
    'mp_eastside' = 'Derailed'; 'mp_glades' = 'Everglades'; 'mp_growhouse' = 'Growhouse'; 'mp_hills' = 'Hollywood Heights'
    'mp_offshore' = 'Riptide'; 'xp1_mallcops' = 'Black Friday'; 'xp1_nights' = 'Code Blue'; 'xp1_projects' = 'The Beat'
    'xp1_sawmill' = 'Backwoods'; 'xp2_museum02' = 'Museum'; 'xp2_precinct7' = 'Precinct 7'; 'xp2_coastal' = 'Break Pointe'
    'xp2_cargoship' = 'The Docks'; 'xp25_bank' = 'Bank Job (xp25)'; 'xp25_sawmill' = 'Backwoods (xp25)'; 'xp3_border' = 'Double Cross'
    'xp3_cistern02' = 'Diversion'; 'xp3_highway' = 'Pacific Highway'; 'xp3_traindodge' = 'Train Dodge'; 'xp4_alcatraz' = 'Alcatraz'
    'xp4_cemetery' = 'Cemetery'; 'xp4_chinatown' = 'Chinatown'; 'xp4_snowcrash' = 'Snowcrash'
}
# the codes as BF-Portable's map lists spell them
$mapCodes = @('mp_bank', 'mp_bloodout', 'mp_desert05', 'mp_downtown', 'mp_eastside', 'mp_glades', 'mp_growhouse', 'mp_hills', 'mp_offshore',
    'xp1_mallcops', 'xp1_nights', 'xp1_projects', 'xp1_sawmill', 'xp2_museum02', 'xp2_precinct7', 'xp2_coastal', 'xp2_cargoship',
    'xp25_bank', 'xp25_sawmill', 'xp3_border', 'xp3_cistern02', 'xp3_highway', 'xp3_traindodge', 'xp4_alcatraz', 'xp4_cemetery', 'xp4_chinatown', 'xp4_snowcrash')
# code, name, bots play it (1) or just hunt (0)
$modes = @(
    @('TurfWarLarge0', 'Conquest Large', 1), @('TurfWarSmall0', 'Conquest Small', 1), @('TeamDeathMatch0', 'Team Deathmatch', 1),
    @('Heist0', 'Heist', 0), @('BloodMoney0', 'Blood Money', 0), @('Hotwire0', 'Hotwire', 0), @('Hit0', 'Crosshair', 0),
    @('Hostage0', 'Rescue', 0), @('CashGrab0', 'Bounty Hunter', 0), @('SquadHeist0', 'Squad Heist', 0), @('CaptureTheFlag0', 'Capture the Flag', 0)
)
# Hardline map pictures (see Get-Thumb below)
$thumbFiles = @{}
foreach ($dir in @((Join-Path $xampp 'htdocs\bfh\img\maps'))) {
    if (-not (Test-Path $dir)) { continue }
    foreach ($f in Get-ChildItem $dir -File -ErrorAction SilentlyContinue) {
        $k = $f.BaseName.ToLower(); if ($f.Extension -match '^\.(jpg|jpeg|png)$' -and -not $thumbFiles.ContainsKey($k)) { $thumbFiles[$k] = $f.FullName }
    }
}
function Map-Name($code) { $n = $mapNames[([string]$code).ToLower()]; if ($n) { $n } else { $code } }
function Mode-Name($code) { foreach ($m in $modes) { if ($m[0] -ieq $code) { return $m[1] } }; $code }

# ---------------------------------------------------------------- servers 1 - 4 (read from BF-Portable's own start files)
function Read-Bat($path) {
    $h = @{}
    if (-not (Test-Path $path)) { return $h }
    $t = Get-Content $path -Raw
    foreach ($k in 'INSTANCEDIRECTORY', 'SERVERNAME', 'GAMEPORT', 'REMOTEADMINPORT', 'ADMINPASSWORD') { if ($t -match "(?m)^set $k=(.*?)\s*$") { $h[$k] = $Matches[1] } }
    if ($t -match '-Login\s+(\S+)') { $h['LOGIN'] = $Matches[1] }
    return $h
}
$srv = @{}
foreach ($n in 1..4) {
    $stock = Join-Path $server $(if ($n -eq 1) { '!StartServer.bat' } else { "!StartServer$n.bat" })
    $b = Read-Bat $stock
    $inst = if ($b.INSTANCEDIRECTORY) { $b.INSTANCEDIRECTORY -replace '%~dp0', ($server.TrimEnd('\') + '\') } else { Join-Path $server $(if ($n -eq 1) { 'Instance' } else { "Instance$n" }) }
    $srv[$n] = @{
        N = $n; Stock = $stock; BotBat = Join-Path $server $(if ($n -eq 1) { '!StartServer-Bots.bat' } else { "!StartServer-Bots$n.bat" })
        Name = $(if ($b.SERVERNAME) { $b.SERVERNAME } else { "BF Hardline Server $n" })
        GamePort = $(if ($b.GAMEPORT) { [int]$b.GAMEPORT } else { 25199 + $n })
        RconPort = $(if ($b.REMOTEADMINPORT) { [int]$b.REMOTEADMINPORT } else { 47599 + $n })
        Pass = $b.ADMINPASSWORD; Login = $(if ($b.LOGIN) { $b.LOGIN } else { "bfh_$n" })
        MapList = Join-Path $inst 'Admin\MapList.txt'
        State = 'off'; Info = $null; Pid = 0; PicMap = ''
    }
}

# ---------------------------------------------------------------- RCON (same protocol code as FloodBots-Control.ps1)
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
    return , $words
}
# one word array per command, or $null when that server doesn't answer
function Invoke-Rcon($s, [object[]]$commands) {
    try {
        $c = New-Object Net.Sockets.TcpClient
        $iar = $c.BeginConnect('127.0.0.1', $s.RconPort, $null, $null)
        if (-not $iar.AsyncWaitHandle.WaitOne(600)) { $c.Close(); return $null }
        $c.EndConnect($iar); $st = $c.GetStream(); $st.ReadTimeout = 4000; $seq = 1
        if ($s.Pass) { Send-Packet $st $seq @('login.plainText', $s.Pass); $seq++; Read-Packet $st | Out-Null }
        $out = @()
        foreach ($cmd in $commands) { Send-Packet $st $seq ([string[]]$cmd); $seq++; $out += , (Read-Packet $st) }
        $c.Close()
        return , $out
    } catch { return $null }
}

# ---------------------------------------------------------------- processes
function Get-ServerProcs {
    if (-not (Get-Process bfh_server_Final -ErrorAction SilentlyContinue)) { return @() }
    return @(Get-CimInstance Win32_Process -Filter "Name='bfh_server_Final.exe'" -ErrorAction SilentlyContinue)
}
function Find-ServerPid($s, $procs) {
    foreach ($p in $procs) { if ($p.CommandLine -and $p.CommandLine -match "-GamePort\s+$($s.GamePort)(\D|$)") { return [int]$p.ProcessId } }
    return 0
}
function Port-Open([int]$port) {
    try { $c = New-Object Net.Sockets.TcpClient; $iar = $c.BeginConnect('127.0.0.1', $port, $null, $null); $ok = $iar.AsyncWaitHandle.WaitOne(300) -and $c.Connected; $c.Close(); return $ok } catch { return $false }
}
$services = [ordered]@{
    mysql = @{ Name = 'MYSQL'; Proc = 'mysqld' }
    web   = @{ Name = 'WEB SERVER'; Proc = 'httpd' }
    redir = @{ Name = 'REDIRECTOR'; Proc = 'gosredirector.ea.com' }
    emu   = @{ Name = 'HARDLINE EMU'; Proc = 'BlazeServerH' }
}
function Svc-Running($key) { [bool](Get-Process $services[$key].Proc -ErrorAction SilentlyContinue) }

# ================================================================ window
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase

$cardXaml = ''
foreach ($n in 1..4) {
    $cardXaml += @"
<Button x:Name="Card$n" Grid.Column="$($n - 1)" Style="{StaticResource Card}" Tag="$n" Margin="0,0,$(if ($n -lt 4) { '10' } else { '0' }),0">
  <Grid>
  <Image x:Name="CardImg$n" ClipToBounds="True" Stretch="UniformToFill" Opacity="0.55" Margin="-14,-10,-14,-10"/>
  <Rectangle Margin="-14,-10,-14,-10">
    <Rectangle.Fill>
      <LinearGradientBrush StartPoint="0,0" EndPoint="1,0">
        <GradientStop Color="#F2141416" Offset="0"/><GradientStop Color="#B0141416" Offset="0.55"/><GradientStop Color="#30141416" Offset="1"/>
      </LinearGradientBrush>
    </Rectangle.Fill>
  </Rectangle>
  <Grid>
    <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/></Grid.RowDefinitions>
    <DockPanel>
      <TextBlock DockPanel.Dock="Right" x:Name="CardState$n" Text="OFFLINE" FontFamily="Consolas" FontSize="12" Foreground="{StaticResource Dim}" VerticalAlignment="Center"/>
      <TextBlock Text="SERVER $n" FontFamily="{StaticResource Head}" FontSize="22" Foreground="{StaticResource Txt}"/>
    </DockPanel>
    <TextBlock Grid.Row="1" x:Name="CardName$n" Text="" FontSize="11" Foreground="{StaticResource Dim}" TextTrimming="CharacterEllipsis"/>
    <TextBlock Grid.Row="2" x:Name="CardMap$n" Text="-" Margin="0,6,0,0" FontSize="13" Foreground="{StaticResource Txt}" TextTrimming="CharacterEllipsis"/>
    <TextBlock Grid.Row="3" x:Name="CardPlayers$n" Text="" FontFamily="Consolas" FontSize="12" Foreground="{StaticResource Blue}"/>
  </Grid>
  </Grid>
</Button>
"@
}

$xamlText = @'
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="Thugbot Launcher" Width="1220" Height="900" MinWidth="1060" MinHeight="760"
        WindowStartupLocation="CenterScreen" Background="#08080A" FontFamily="Bahnschrift, Segoe UI" Foreground="#EEE7DF">
  <Window.Resources>
    <SolidColorBrush x:Key="Blue" Color="#FFFFFF"/>
    <SolidColorBrush x:Key="Orange" Color="#FF6A00"/>
    <SolidColorBrush x:Key="Txt" Color="#F6F2ED"/>
    <SolidColorBrush x:Key="Dim" Color="#8E857B"/>
    <SolidColorBrush x:Key="Panel" Color="#141416"/>
    <SolidColorBrush x:Key="Edge" Color="#38322C"/>
    <FontFamily x:Key="Head">Bahnschrift SemiBold Condensed, Impact, Segoe UI</FontFamily>

    <Style x:Key="Btn" TargetType="Button">
      <Setter Property="Background" Value="#1B1B1E"/>
      <Setter Property="Foreground" Value="#F3EEE8"/>
      <Setter Property="BorderBrush" Value="#FFFFFF"/>
      <Setter Property="BorderThickness" Value="1,1,1,3"/>
      <Setter Property="FontFamily" Value="Bahnschrift SemiBold Condensed, Segoe UI"/>
      <Setter Property="FontSize" Value="15"/>
      <Setter Property="Padding" Value="12,6"/>
      <Setter Property="Margin" Value="0,0,8,8"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="Button">
            <Border Background="{TemplateBinding Background}" BorderBrush="{TemplateBinding BorderBrush}" BorderThickness="{TemplateBinding BorderThickness}" Padding="{TemplateBinding Padding}">
              <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
            </Border>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
      <Style.Triggers>
        <Trigger Property="IsMouseOver" Value="True"><Setter Property="Background" Value="#FFFFFF"/><Setter Property="Foreground" Value="#0A0A0A"/></Trigger>
        <Trigger Property="IsPressed" Value="True"><Setter Property="Background" Value="#FF6A00"/><Setter Property="Foreground" Value="#0A0A0A"/></Trigger>
        <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.35"/></Trigger>
      </Style.Triggers>
    </Style>
    <Style x:Key="OBtn" TargetType="Button" BasedOn="{StaticResource Btn}">
      <Setter Property="BorderBrush" Value="#FF6A00"/>
      <Setter Property="Foreground" Value="#FFD6B8"/>
      <Setter Property="Background" Value="#1A0F06"/>
      <Style.Triggers>
        <Trigger Property="IsMouseOver" Value="True"><Setter Property="Background" Value="#FF6A00"/><Setter Property="Foreground" Value="#0A0A0A"/></Trigger>
        <Trigger Property="IsPressed" Value="True"><Setter Property="Background" Value="#FFFFFF"/><Setter Property="Foreground" Value="#0A0A0A"/></Trigger>
        <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.35"/></Trigger>
      </Style.Triggers>
    </Style>
    <Style x:Key="Card" TargetType="Button">
      <Setter Property="Background" Value="#141416"/>
      <Setter Property="BorderBrush" Value="#38322C"/>
      <Setter Property="BorderThickness" Value="1,1,1,3"/>
      <Setter Property="Foreground" Value="#F6F2ED"/>
      <Setter Property="Padding" Value="14,10"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="HorizontalContentAlignment" Value="Stretch"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="Button">
            <Grid>
              <Border Background="{TemplateBinding Background}" BorderBrush="{TemplateBinding BorderBrush}" BorderThickness="{TemplateBinding BorderThickness}" Padding="{TemplateBinding Padding}">
                <ContentPresenter HorizontalAlignment="Stretch"/>
              </Border>
              <Border x:Name="hover" Background="#FFFFFF" Opacity="0" IsHitTestVisible="False"/>
            </Grid>
            <ControlTemplate.Triggers>
              <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="hover" Property="Opacity" Value="0.08"/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <Style TargetType="ListBox">
      <Setter Property="Background" Value="#0E0E10"/>
      <Setter Property="Foreground" Value="#EEE7DF"/>
      <Setter Property="BorderBrush" Value="#38322C"/>
      <Setter Property="BorderThickness" Value="1"/>
      <Setter Property="ScrollViewer.HorizontalScrollBarVisibility" Value="Disabled"/>
    </Style>
    <Style TargetType="ListBoxItem">
      <Setter Property="Foreground" Value="#EEE7DF"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="ListBoxItem">
            <Border x:Name="b" Background="Transparent" BorderBrush="Transparent" BorderThickness="3,0,0,0" Padding="8,3">
              <ContentPresenter/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="b" Property="Background" Value="#26221E"/></Trigger>
              <Trigger Property="IsSelected" Value="True"><Setter TargetName="b" Property="Background" Value="#33281F"/><Setter TargetName="b" Property="BorderBrush" Value="#FF6A00"/><Setter Property="Foreground" Value="#FFFFFF"/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <Style x:Key="Chip" TargetType="ListBoxItem">
      <Setter Property="Foreground" Value="#F3EEE8"/>
      <Setter Property="Margin" Value="0,0,6,6"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="ListBoxItem">
            <Border x:Name="b" Background="#1B1B1E" BorderBrush="#38322C" BorderThickness="1" Padding="9,4">
              <ContentPresenter/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="b" Property="BorderBrush" Value="#FFFFFF"/></Trigger>
              <Trigger Property="IsSelected" Value="True"><Setter TargetName="b" Property="Background" Value="#FF6A00"/><Setter TargetName="b" Property="BorderBrush" Value="#FF6A00"/><Setter Property="Foreground" Value="#0A0A0A"/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <Style TargetType="ScrollBar">
      <Setter Property="Width" Value="8"/>
      <Setter Property="MinWidth" Value="8"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="ScrollBar">
            <Grid Background="#101012">
              <Track x:Name="PART_Track" IsDirectionReversed="True">
                <Track.Thumb>
                  <Thumb>
                    <Thumb.Template>
                      <ControlTemplate TargetType="Thumb"><Border Background="#FFFFFF" Opacity="0.55"/></ControlTemplate>
                    </Thumb.Template>
                  </Thumb>
                </Track.Thumb>
              </Track>
            </Grid>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <Style x:Key="H" TargetType="TextBlock">
      <Setter Property="FontFamily" Value="Bahnschrift SemiBold Condensed, Impact, Segoe UI"/>
      <Setter Property="FontSize" Value="16"/>
      <Setter Property="Foreground" Value="#FFFFFF"/>
      <Setter Property="Margin" Value="0,0,0,8"/>
    </Style>
  </Window.Resources>

  <Grid>
    <!-- police lights: soft red / blue glow from the top corners -->
    <Rectangle x:Name="GlowR" IsHitTestVisible="False" Opacity="0" VerticalAlignment="Top" Height="320">
      <Rectangle.Fill>
        <RadialGradientBrush Center="0.12,0" GradientOrigin="0.12,0" RadiusX="0.42" RadiusY="1">
          <GradientStop Color="#88FF1E1E" Offset="0"/><GradientStop Color="#00FF1E1E" Offset="1"/>
        </RadialGradientBrush>
      </Rectangle.Fill>
    </Rectangle>
    <Rectangle x:Name="GlowB" IsHitTestVisible="False" Opacity="0" VerticalAlignment="Top" Height="320">
      <Rectangle.Fill>
        <RadialGradientBrush Center="0.88,0" GradientOrigin="0.88,0" RadiusX="0.42" RadiusY="1">
          <GradientStop Color="#882A5CFF" Offset="0"/><GradientStop Color="#002A5CFF" Offset="1"/>
        </RadialGradientBrush>
      </Rectangle.Fill>
    </Rectangle>
    <!-- the light bar -->
    <Grid VerticalAlignment="Top" Height="7" IsHitTestVisible="False" Panel.ZIndex="5">
      <Grid.ColumnDefinitions><ColumnDefinition/><ColumnDefinition Width="0.25*"/><ColumnDefinition/></Grid.ColumnDefinitions>
      <Rectangle x:Name="LightR" Fill="#FF2020" Opacity="0.15"/>
      <Rectangle x:Name="LightW" Grid.Column="1" Fill="#FFFFFF" Opacity="0.1"/>
      <Rectangle x:Name="LightB" Grid.Column="2" Fill="#2A5CFF" Opacity="0.15"/>
    </Grid>
    <!-- faint grid behind everything -->
    <Rectangle IsHitTestVisible="False" Opacity="0.5">
      <Rectangle.Fill>
        <DrawingBrush TileMode="Tile" Viewport="0,0,32,32" ViewportUnits="Absolute">
          <DrawingBrush.Drawing>
            <GeometryDrawing Brush="#08080A">
              <GeometryDrawing.Pen><Pen Brush="#17171A" Thickness="1"/></GeometryDrawing.Pen>
              <GeometryDrawing.Geometry><RectangleGeometry Rect="0,0,32,32"/></GeometryDrawing.Geometry>
            </GeometryDrawing>
          </DrawingBrush.Drawing>
        </DrawingBrush>
      </Rectangle.Fill>
    </Rectangle>

    <Grid Margin="22,20,22,12">
      <Grid.RowDefinitions>
        <RowDefinition Height="Auto"/>
        <RowDefinition Height="Auto"/>
        <RowDefinition Height="Auto"/>
        <RowDefinition Height="*"/>
        <RowDefinition Height="Auto"/>
      </Grid.RowDefinitions>

      <!-- header: glitch title + backend -->
      <Grid Grid.Row="0">
        <Grid.ColumnDefinitions><ColumnDefinition Width="Auto"/><ColumnDefinition Width="*"/></Grid.ColumnDefinitions>
        <StackPanel>
          <Grid x:Name="TitleBox" Height="80" Width="440" HorizontalAlignment="Left" ClipToBounds="True">
            <TextBlock x:Name="TitleO" Text="THUGBOT" FontFamily="Bahnschrift SemiBold Condensed, Impact" FontSize="76" Foreground="#FF6A00" Opacity="0.9"/>
            <TextBlock x:Name="TitleB" Text="THUGBOT" FontFamily="Bahnschrift SemiBold Condensed, Impact" FontSize="76" Foreground="#FFFFFF" Opacity="0.9"/>
            <TextBlock x:Name="TitleW" Text="THUGBOT" FontFamily="Bahnschrift SemiBold Condensed, Impact" FontSize="76" Foreground="#FFFFFF"/>
            <Rectangle x:Name="Slice1" Height="3" Fill="#FFFFFF" VerticalAlignment="Top" Opacity="0" IsHitTestVisible="False"/>
            <Rectangle x:Name="Slice2" Height="2" Fill="#FF6A00" VerticalAlignment="Top" Opacity="0" IsHitTestVisible="False"/>
          </Grid>
          <StackPanel Orientation="Horizontal" Margin="2,0,0,0">
            <Rectangle Width="26" Height="4" Fill="#FF6A00" Margin="0,0,6,0"/>
            <Rectangle Width="10" Height="4" Fill="#FFFFFF" Margin="0,0,10,0"/>
            <TextBlock x:Name="Sub" Text="HARDLINE // COPS VS CRIMINALS // LAN BOTS" FontFamily="Consolas" FontSize="13" Foreground="#8E857B"/>
          </StackPanel>
          <StackPanel Orientation="Horizontal" Margin="0,12,0,0">
            <Button x:Name="TabServers" Style="{StaticResource OBtn}" Content="SERVERS" Width="120" Padding="0,4" Margin="0,0,8,0"/>
            <Button x:Name="TabMaps" Style="{StaticResource Btn}" Content="MAPS" Width="120" Padding="0,4" Margin="0"/>
          </StackPanel>
        </StackPanel>

        <Border Grid.Column="1" Margin="30,4,0,0" Background="#141416" BorderBrush="#38322C" BorderThickness="1,1,1,3" Padding="14,10" VerticalAlignment="Top">
          <Grid>
            <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/></Grid.RowDefinitions>
            <DockPanel Margin="0,0,0,8">
              <Button x:Name="BtnBackStop" DockPanel.Dock="Right" Style="{StaticResource Btn}" Content="STOP BACKEND" Margin="8,0,0,0" Padding="10,3" FontSize="13"/>
              <Button x:Name="BtnBackStart" DockPanel.Dock="Right" Style="{StaticResource OBtn}" Content="START BACKEND" Margin="0" Padding="12,3" FontSize="13"/>
              <TextBlock Text="BACKEND" Style="{StaticResource H}" Margin="0" VerticalAlignment="Center"/>
            </DockPanel>
            <UniformGrid Grid.Row="1" Columns="4">
              <StackPanel Orientation="Horizontal" Margin="0,0,10,0"><Ellipse x:Name="DotMysql" Width="10" Height="10" Fill="#3A1A0A" Margin="0,0,8,0"/><StackPanel><TextBlock Text="MYSQL" FontSize="13" Foreground="#F6F2ED"/><TextBlock x:Name="TxtMysql" Text="OFF" FontFamily="Consolas" FontSize="11" Foreground="#8E857B"/></StackPanel></StackPanel>
              <StackPanel Orientation="Horizontal" Margin="0,0,10,0"><Ellipse x:Name="DotWeb" Width="10" Height="10" Fill="#3A1A0A" Margin="0,0,8,0"/><StackPanel><TextBlock Text="WEB SERVER" FontSize="13" Foreground="#F6F2ED"/><TextBlock x:Name="TxtWeb" Text="OFF" FontFamily="Consolas" FontSize="11" Foreground="#8E857B"/></StackPanel></StackPanel>
              <StackPanel Orientation="Horizontal" Margin="0,0,10,0"><Ellipse x:Name="DotRedir" Width="10" Height="10" Fill="#3A1A0A" Margin="0,0,8,0"/><StackPanel><TextBlock Text="REDIRECTOR" FontSize="13" Foreground="#F6F2ED"/><TextBlock x:Name="TxtRedir" Text="OFF" FontFamily="Consolas" FontSize="11" Foreground="#8E857B"/></StackPanel></StackPanel>
              <StackPanel Orientation="Horizontal"><Ellipse x:Name="DotEmu" Width="10" Height="10" Fill="#3A1A0A" Margin="0,0,8,0"/><StackPanel><TextBlock Text="HARDLINE EMU" FontSize="13" Foreground="#F6F2ED"/><TextBlock x:Name="TxtEmu" Text="OFF" FontFamily="Consolas" FontSize="11" Foreground="#8E857B"/></StackPanel></StackPanel>
            </UniformGrid>
          </Grid>
        </Border>
      </Grid>

      <Grid x:Name="PageServers" Grid.Row="1" Grid.RowSpan="3">
      <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="*"/></Grid.RowDefinitions>
      <!-- server cards -->
      <Grid Grid.Row="0" Margin="0,16,0,0">
        <Grid.ColumnDefinitions><ColumnDefinition/><ColumnDefinition/><ColumnDefinition/><ColumnDefinition/></Grid.ColumnDefinitions>
        %CARDS%
      </Grid>

      <Rectangle Grid.Row="1" Height="2" Margin="0,14,0,14">
        <Rectangle.Fill>
          <LinearGradientBrush StartPoint="0,0" EndPoint="1,0">
            <GradientStop Color="#FF6A00" Offset="0"/><GradientStop Color="#FF6A00" Offset="0.18"/>
            <GradientStop Color="#00000000" Offset="0.181"/><GradientStop Color="#00000000" Offset="0.2"/>
            <GradientStop Color="#FFFFFF" Offset="0.201"/><GradientStop Color="#00FFFFFF" Offset="1"/>
          </LinearGradientBrush>
        </Rectangle.Fill>
      </Rectangle>

      <!-- selected server -->
      <Grid Grid.Row="2">
        <Grid.ColumnDefinitions><ColumnDefinition Width="330"/><ColumnDefinition Width="*"/><ColumnDefinition Width="380"/></Grid.ColumnDefinitions>

        <!-- control column -->
        <Border Background="#141416" BorderBrush="#38322C" BorderThickness="1" Margin="0,0,14,0">
          <ScrollViewer VerticalScrollBarVisibility="Auto" HorizontalScrollBarVisibility="Disabled">
          <StackPanel>
            <Grid Height="104" ClipToBounds="True">
              <Image x:Name="SelImg" Stretch="UniformToFill"/>
              <Rectangle>
                <Rectangle.Fill>
                  <LinearGradientBrush StartPoint="0,0" EndPoint="0,1">
                    <GradientStop Color="#20141416" Offset="0"/><GradientStop Color="#90141416" Offset="0.55"/><GradientStop Color="#FF141416" Offset="1"/>
                  </LinearGradientBrush>
                </Rectangle.Fill>
              </Rectangle>
              <Rectangle Width="70" Height="3" Fill="#FF6A00" HorizontalAlignment="Left" VerticalAlignment="Top"/>
              <Rectangle Width="24" Height="3" Fill="#FFFFFF" HorizontalAlignment="Left" VerticalAlignment="Top" Margin="74,0,0,0"/>
              <StackPanel VerticalAlignment="Bottom" Margin="16,0,16,6">
                <TextBlock x:Name="SelTitle" Text="SERVER 1" FontFamily="Bahnschrift SemiBold Condensed, Impact" FontSize="30" Foreground="#FFFFFF"/>
                <TextBlock x:Name="SelMap" Text="" FontSize="13" Foreground="#FFB27A" TextTrimming="CharacterEllipsis"/>
              </StackPanel>
            </Grid>
            <StackPanel Margin="16,8,16,12">
            <TextBlock x:Name="SelName" Text="" FontSize="12" Foreground="#8E857B" TextTrimming="CharacterEllipsis"/>
            <TextBlock x:Name="SelPorts" Text="" FontFamily="Consolas" FontSize="11" Foreground="#8E857B" Margin="0,2,0,8"/>
            <TextBlock x:Name="SelStatus" Text="OFFLINE" FontFamily="Consolas" FontSize="13" Foreground="#8E857B" TextWrapping="Wrap" Margin="0,0,0,12"/>

            <TextBlock Text="BOTS" Style="{StaticResource H}"/>
            <StackPanel Orientation="Horizontal" Margin="0,0,0,10">
              <Button x:Name="BtnBotsDn" Style="{StaticResource Btn}" Content="-" Width="34" Padding="0,4"/>
              <Border BorderBrush="#38322C" BorderThickness="1" Width="64" Height="32" Margin="0,0,8,8" Background="#0E0E10">
                <TextBlock x:Name="TxtBots" Text="63" FontFamily="Consolas" FontSize="18" Foreground="#FF6A00" HorizontalAlignment="Center" VerticalAlignment="Center"/>
              </Border>
              <Button x:Name="BtnBotsUp" Style="{StaticResource Btn}" Content="+" Width="34" Padding="0,4"/>
              <Button x:Name="BtnBotsOn" Style="{StaticResource Btn}" Content="THUGBOT: ON" Width="128"/>
            </StackPanel>

            <TextBlock Text="SERVER" Style="{StaticResource H}"/>
            <UniformGrid Columns="2">
              <Button x:Name="BtnStart" Style="{StaticResource OBtn}" Content="START SERVER"/>
              <Button x:Name="BtnStop" Style="{StaticResource Btn}" Content="STOP SERVER"/>
              <Button x:Name="BtnRestart" Style="{StaticResource Btn}" Content="RESTART SERVER"/>
              <Button x:Name="BtnRound" Style="{StaticResource Btn}" Content="RESTART ROUND"/>
              <Button x:Name="BtnNext" Style="{StaticResource Btn}" Content="NEXT MAP"/>
              <Button x:Name="BtnVeh" Style="{StaticResource Btn}" Content="VEHICLES: OFF"/>
            </UniformGrid>

            <Button x:Name="BtnJoin" Style="{StaticResource OBtn}" Margin="0,10,0,10" Padding="0,12" FontSize="26" BorderThickness="2,2,2,5">
              <TextBlock x:Name="JoinText" Text="&#9654; JOIN SERVER 1"/>
            </Button>

            <TextBlock Text="TOOLS" Style="{StaticResource H}"/>
            <UniformGrid Columns="3">
              <Button x:Name="BtnLearned" Style="{StaticResource Btn}" Content="LEARNED MAP" FontSize="12" Padding="4,5"/>
              <Button x:Name="Btn3D" Style="{StaticResource Btn}" Content="3D MAP" FontSize="12" Padding="4,5"/>
              <Button x:Name="BtnClassic" Style="{StaticResource Btn}" Content="OLD PANEL" FontSize="12" Padding="4,5"/>
            </UniformGrid>
            </StackPanel>
          </StackPanel>
          </ScrollViewer>
        </Border>

        <!-- this server's map list -->
        <Border Grid.Column="1" Background="#141416" BorderBrush="#38322C" BorderThickness="1" Padding="16,12" Margin="0,0,14,0">
          <Grid>
            <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="*"/><RowDefinition Height="Auto"/></Grid.RowDefinitions>
            <DockPanel Margin="0,0,0,8">
              <TextBlock x:Name="RotCount" DockPanel.Dock="Right" FontFamily="Consolas" FontSize="12" Foreground="#8E857B" VerticalAlignment="Center"/>
              <TextBlock x:Name="RotTitle" Text="MAP LIST // SERVER 1" Style="{StaticResource H}" Margin="0"/>
            </DockPanel>
            <ListBox x:Name="LstRot" Grid.Row="1" FontFamily="Consolas" FontSize="13"/>
            <StackPanel Grid.Row="2" Margin="0,10,0,0">
              <WrapPanel>
                <Button x:Name="BtnPlay" Style="{StaticResource OBtn}" Content="PLAY THIS FIRST / NOW"/>
                <Button x:Name="BtnUp" Style="{StaticResource Btn}" Content="UP" Width="54"/>
                <Button x:Name="BtnDown" Style="{StaticResource Btn}" Content="DOWN" Width="64"/>
                <Button x:Name="BtnRemove" Style="{StaticResource Btn}" Content="REMOVE"/>
              </WrapPanel>
              <TextBlock Text="The server starts on the first map in this list. Changes save straight to this server's MapList.txt." FontSize="11" Foreground="#8E857B" TextWrapping="Wrap"/>
            </StackPanel>
          </Grid>
        </Border>

        <!-- add maps -->
        <Border Grid.Column="2" Background="#141416" BorderBrush="#38322C" BorderThickness="1" Padding="16,12">
          <Grid>
            <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="*"/><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/></Grid.RowDefinitions>
            <TextBlock Text="ADD MAPS" Style="{StaticResource H}"/>
            <ListBox x:Name="LstMaps" Grid.Row="1" FontSize="13" SelectionMode="Extended"/>
            <TextBlock Grid.Row="2" Text="MODE   (&#9733; = bots play the objective)" Style="{StaticResource H}" Margin="0,10,0,6" FontSize="14"/>
            <ListBox x:Name="LstModes" Grid.Row="3" ItemContainerStyle="{StaticResource Chip}" Background="Transparent" BorderThickness="0" FontSize="12" MaxHeight="150">
              <ListBox.ItemsPanel><ItemsPanelTemplate><WrapPanel/></ItemsPanelTemplate></ListBox.ItemsPanel>
            </ListBox>
            <WrapPanel Grid.Row="4" Margin="0,8,0,0">
              <Button x:Name="BtnAdd" Style="{StaticResource OBtn}" Content="ADD SELECTED"/>
              <Button x:Name="BtnAllMode" Style="{StaticResource Btn}" Content="WHOLE LIST TO MODE"/>
              <Button x:Name="BtnFill" Style="{StaticResource Btn}" Content="ALL MAPS IN MODE"/>
            </WrapPanel>
          </Grid>
        </Border>
      </Grid>

      </Grid>

      <!-- maps page: 3D map and learned maps in a WebView2 -->
      <Grid x:Name="PageMaps" Grid.Row="1" Grid.RowSpan="3" Margin="0,16,0,0" Visibility="Collapsed">
        <Grid.ColumnDefinitions><ColumnDefinition Width="280"/><ColumnDefinition Width="*"/></Grid.ColumnDefinitions>
        <Border Background="#141416" BorderBrush="#38322C" BorderThickness="1" Padding="14,12" Margin="0,0,14,0">
          <Grid>
            <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="*"/><RowDefinition Height="Auto"/></Grid.RowDefinitions>
            <TextBlock Text="3D MAP" Style="{StaticResource H}"/>
            <Button Grid.Row="1" x:Name="BtnShow3D" Style="{StaticResource OBtn}" Content="3D MAP / ROUTE EDITOR" Margin="0,0,0,14"/>
            <TextBlock Grid.Row="2" Text="LEARNED MAPS" Style="{StaticResource H}"/>
            <ListBox Grid.Row="3" x:Name="LstReports" FontSize="13"/>
            <WrapPanel Grid.Row="4" Margin="0,10,0,0">
              <Button x:Name="BtnMapReload" Style="{StaticResource Btn}" Content="RELOAD" FontSize="13"/>
              <Button x:Name="BtnMapBrowser" Style="{StaticResource Btn}" Content="OPEN IN BROWSER" FontSize="13"/>
            </WrapPanel>
          </Grid>
        </Border>
        <Border Grid.Column="1" Background="#0E0E10" BorderBrush="#38322C" BorderThickness="1">
          <Grid x:Name="WebHost">
            <TextBlock x:Name="WebMsg" Text="Pick the 3D map or a learned map on the left." Foreground="#8E857B" FontSize="15" HorizontalAlignment="Center" VerticalAlignment="Center" TextWrapping="Wrap" TextAlignment="Center" Margin="40"/>
          </Grid>
        </Border>
      </Grid>

      <!-- log -->
      <Border Grid.Row="4" Margin="0,12,0,0" BorderBrush="#38322C" BorderThickness="0,1,0,0" Padding="0,8,0,0">
        <DockPanel>
          <TextBlock DockPanel.Dock="Left" Text="&gt;_" FontFamily="Consolas" FontSize="13" Foreground="#FF6A00" Margin="0,0,10,0"/>
          <TextBlock x:Name="Log" FontFamily="Consolas" FontSize="12" Foreground="#CFC4B7" TextWrapping="Wrap"/>
        </DockPanel>
      </Border>
    </Grid>

    <!-- scanlines over everything -->
    <Rectangle IsHitTestVisible="False" Opacity="0.35">
      <Rectangle.Fill>
        <LinearGradientBrush StartPoint="0,0" EndPoint="0,3" MappingMode="Absolute" SpreadMethod="Repeat">
          <GradientStop Color="#00000000" Offset="0"/><GradientStop Color="#00000000" Offset="0.5"/>
          <GradientStop Color="#40000000" Offset="0.5"/><GradientStop Color="#40000000" Offset="1"/>
        </LinearGradientBrush>
      </Rectangle.Fill>
    </Rectangle>
  </Grid>
</Window>
'@
$xamlText = $xamlText.Replace('%CARDS%', $cardXaml)

if ($env:THUGBOT_LAUNCHER_TEST) {
    # self-check: parse the window (no Show), read every server, start nothing
    $w = [Windows.Markup.XamlReader]::Parse($xamlText)
    "window: ok ($($w.Title))"
    "paths: server=$server emu=$emuDir xampp=$xampp redirector=$redirDir client=$clientDir"
    foreach ($k in $services.Keys) { "service $($services[$k].Name): $(if (Svc-Running $k) { 'running' } else { 'off' })" }
    $procs = Get-ServerProcs
    foreach ($n in 1..4) {
        $s = $srv[$n]; $rows = @(if (Test-Path $s.MapList) { Get-Content $s.MapList | Where-Object { $_.Trim() } })
        "server ${n}: '$($s.Name)' game $($s.GamePort) rcon $($s.RconPort) login $($s.Login) pid $(Find-ServerPid $s $procs) maps $($rows.Count) first '$(if ($rows.Count) { $rows[0] })'"
    }
    "map pictures: $($thumbFiles.Count) found, missing for: $(@($mapCodes | Where-Object { -not $thumbFiles[$_.ToLower()] }) -join ', ')"
    "webview2 lib: $(Test-Path (Join-Path $libDir 'Microsoft.Web.WebView2.Wpf.dll'))   3d map: $(Test-Path (Join-Path $map3dDir 'server.js'))   learned maps: $(@(Get-ChildItem $reportDir -Filter *.html -ErrorAction SilentlyContinue).Count)"
    "useout: $(Test-Path (Join-Path $PSScriptRoot 'Thugbot-UseOut.txt'))   bfhlan handler: $(Test-Path 'Registry::HKEY_CLASSES_ROOT\bfhlan')"
    return
}

try { $win = [Windows.Markup.XamlReader]::Parse($xamlText) } catch { Write-Host "Thugbot Launcher: the window could not be built: $($_.Exception.Message)"; exit 1 }
function Find-Ctl([string]$name) { $win.FindName($name) }
$ui = @{}
foreach ($nm in 'GlowR', 'GlowB', 'LightR', 'LightW', 'LightB', 'TitleBox', 'TitleO', 'TitleB', 'TitleW', 'Slice1', 'Slice2', 'Sub', 'BtnBackStart', 'BtnBackStop', 'DotMysql', 'TxtMysql', 'DotWeb', 'TxtWeb', 'DotRedir', 'TxtRedir', 'DotEmu', 'TxtEmu',
    'SelTitle', 'SelMap', 'SelImg', 'SelName', 'SelPorts', 'SelStatus', 'BtnBotsDn', 'BtnBotsUp', 'TxtBots', 'BtnBotsOn', 'BtnStart', 'BtnStop', 'BtnRestart', 'BtnRound', 'BtnNext', 'BtnVeh',
    'BtnJoin', 'JoinText', 'BtnLearned', 'Btn3D', 'BtnClassic', 'RotCount', 'RotTitle', 'LstRot', 'BtnPlay', 'BtnUp', 'BtnDown', 'BtnRemove', 'LstMaps', 'LstModes',
    'BtnAdd', 'BtnAllMode', 'BtnFill', 'Log', 'TabServers', 'TabMaps', 'PageServers', 'PageMaps', 'BtnShow3D', 'LstReports', 'BtnMapReload', 'BtnMapBrowser', 'WebHost', 'WebMsg') { $ui[$nm] = Find-Ctl $nm }
foreach ($n in 1..4) { foreach ($p in 'Card', 'CardImg', 'CardState', 'CardName', 'CardMap', 'CardPlayers') { $ui["$p$n"] = Find-Ctl "$p$n" } }

function Brush([string]$hex) { $b = New-Object Windows.Media.SolidColorBrush ([Windows.Media.ColorConverter]::ConvertFromString($hex)); $b.Freeze(); $b }
$cWhite = Brush '#FFFFFF'; $cOrange = Brush '#FF6A00'; $cDim = Brush '#8E857B'; $cTxt = Brush '#F6F2ED'; $cEdge = Brush '#38322C'; $cOffDot = Brush '#3A1A0A'; $cRed = Brush '#FF3B3B'; $cDark = Brush '#0B0B0D'

# ---------------------------------------------------------------- map pictures
# Battlelog's own Hardline map pictures, already in BF-Portable's web folder (htdocs\bfh\img\maps) -
# no internet needed. A map without a picture just shows the dark box.
$thumbCache = @{}
function Get-Thumb($code) {
    $k = ([string]$code).ToLower()
    if ($thumbCache.ContainsKey($k)) { return $thumbCache[$k] }
    $img = $null
    if ($thumbFiles[$k]) {
        try {
            $img = New-Object Windows.Media.Imaging.BitmapImage
            $img.BeginInit(); $img.UriSource = New-Object Uri ($thumbFiles[$k]); $img.CacheOption = [Windows.Media.Imaging.BitmapCacheOption]::OnLoad
            $img.DecodePixelWidth = 335; $img.EndInit(); $img.Freeze()
        } catch { $img = $null }
    }
    $thumbCache[$k] = $img
    return $img
}
# a list row: the map's picture, then the text
function Map-Item($code, [string]$text) {
    $sp = New-Object Windows.Controls.StackPanel; $sp.Orientation = [Windows.Controls.Orientation]::Horizontal
    $b = New-Object Windows.Controls.Border
    $b.Width = 76; $b.Height = 36; $b.Margin = [Windows.Thickness]::new(0, 2, 10, 2); $b.BorderThickness = [Windows.Thickness]::new(0, 0, 0, 2)
    $b.BorderBrush = $cWhite; $b.Background = $cDark
    $t = Get-Thumb $code
    if ($t) { $im = New-Object Windows.Controls.Image; $im.Source = $t; $im.Stretch = [Windows.Media.Stretch]::UniformToFill; $im.ClipToBounds = $true; $b.Child = $im }
    $tb = New-Object Windows.Controls.TextBlock; $tb.Text = $text; $tb.VerticalAlignment = [Windows.VerticalAlignment]::Center
    [void]$sp.Children.Add($b); [void]$sp.Children.Add($tb)
    return $sp
}

$logLines = New-Object Collections.Generic.List[string]
function Say([string]$t) {
    $logLines.Add("$(Get-Date -Format HH:mm:ss)  $t"); while ($logLines.Count -gt 3) { $logLines.RemoveAt(0) }
    $ui.Log.Text = ($logLines -join "`n")
    Pump
}
# lets the window redraw during a longer step (the log line shows before the wait)
function Pump { try { $win.Dispatcher.Invoke([Action] {}, [Windows.Threading.DispatcherPriority]::Background) } catch {} }
function Wait-Ms([int]$ms) { $end = (Get-Date).AddMilliseconds($ms); while ((Get-Date) -lt $end) { Pump; Start-Sleep -Milliseconds 50 } }
# one button action at a time (the waits keep the window drawing, so a second click could start a second action)
$script:busy = $false
function Safe([scriptblock]$b) {
    if ($script:busy) { Say 'Busy - one moment...'; return }
    $script:busy = $true
    try { & $b } catch { Say "Error: $($_.Exception.Message)" } finally { $script:busy = $false }
}

# ---------------------------------------------------------------- backend
function Start-Svc([string]$key) {
    if (Svc-Running $key) { return $true }
    switch ($key) {
        'mysql' {
            $exe = Join-Path $xampp 'mysql\bin\mysqld.exe'
            if (-not (Test-Path $exe)) { Say "MySQL not found: $exe"; return $false }
            Say 'Starting MySQL...'
            # same as xampp\mysql_start.bat (from the xampp folder: the ini path is relative), without its console window
            Start-Process -FilePath $exe -ArgumentList '--defaults-file=mysql\bin\my.ini', '--standalone' -WorkingDirectory $xampp -WindowStyle Hidden | Out-Null
            for ($i = 0; $i -lt 30; $i++) { Wait-Ms 400; if (Port-Open 3306) { return $true }; if ($i -gt 4 -and -not (Svc-Running 'mysql')) { break } }
            Say 'MySQL did not come up (see xampp\mysql\data\*.err, or Repair-MySQL-Privileges.bat).'; return $false
        }
        'web' {
            $exe = Join-Path $xampp 'apache\bin\httpd.exe'
            if (-not (Test-Path $exe)) { Say "Apache not found: $exe"; return $false }
            Say 'Starting the web server...'
            Start-Process -FilePath $exe -WorkingDirectory $xampp -WindowStyle Hidden | Out-Null
            Wait-Ms 1200; return (Svc-Running 'web')
        }
        'redir' {
            $exe = Join-Path $redirDir 'gosredirector.ea.com.exe'
            if (-not (Test-Path $exe)) { Say "Redirector not found: $exe"; return $false }
            Say 'Starting the redirector...'
            # same as Redirector\Redirector.cmd
            try { Start-Process -FilePath $exe -ArgumentList '/console' -WorkingDirectory $redirDir -WindowStyle Minimized | Out-Null }
            catch { Start-Process -FilePath $exe -ArgumentList '/console' -WorkingDirectory $redirDir -WindowStyle Minimized -Verb RunAs | Out-Null }
            Wait-Ms 1200; return (Svc-Running 'redir')
        }
        'emu' {
            $exe = Join-Path $emuDir 'BlazeServerH.exe'
            if (-not (Test-Path $exe)) { Say "Hardline emulator not found: $exe"; return $false }
            Say 'Starting the Hardline emulator...'
            Start-Process -FilePath $exe -WorkingDirectory $emuDir -WindowStyle Minimized | Out-Null
            Wait-Ms 3000; return (Svc-Running 'emu')
        }
    }
    return $false
}
# order matters: the emulator and the web server need the database
function Start-Backend {
    foreach ($k in 'mysql', 'web', 'redir', 'emu') {
        if (-not (Start-Svc $k)) { Say "$($services[$k].Name) did not start - the rest of the backend was not started."; Update-Services; return $false }
        Update-Services
    }
    return $true
}
function Stop-Backend {
    if (Get-ServerProcs) { Say 'Stop the Hardline servers first (they need the backend).'; return }
    foreach ($k in 'emu', 'redir', 'web', 'mysql') { Get-Process $services[$k].Proc -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue }
    Wait-Ms 800; Update-Services; Say 'Backend stopped.'
}
function Update-Services {
    $map = @{ mysql = @('DotMysql', 'TxtMysql'); web = @('DotWeb', 'TxtWeb'); redir = @('DotRedir', 'TxtRedir'); emu = @('DotEmu', 'TxtEmu') }
    foreach ($k in $map.Keys) {
        $on = Svc-Running $k
        $ui[$map[$k][0]].Fill = $(if ($on) { $cWhite } else { $cOffDot })
        $ui[$map[$k][1]].Text = $(if ($on) { 'ONLINE' } else { 'OFF' })
        $ui[$map[$k][1]].Foreground = $(if ($on) { $cWhite } else { $cDim })
    }
}

# ---------------------------------------------------------------- servers
$sel = [int]$set.Selected; if ($sel -lt 1 -or $sel -gt 4) { $sel = 1 }
function Cur-Srv { $srv[$script:sel] }

function Read-Rows($s) {
    if (-not (Test-Path $s.MapList)) { return , @() }
    $rows = @()
    foreach ($l in Get-Content $s.MapList) { $p = $l.Trim() -split '\s+'; if ($p.Count -ge 2 -and $p[0]) { $rows += , @($p[0], $p[1], $(if ($p.Count -ge 3) { $p[2] } else { '1' })) } }
    return , $rows
}
function Write-Rows($s, $rows) {
    $dir = Split-Path $s.MapList
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    $lines = @(foreach ($r in $rows) { "$($r[0]) $($r[1]) $($r[2])" })
    [IO.File]::WriteAllLines($s.MapList, [string[]]$lines)
    # a running server reads the file again (BF-Portable: mapList.add / remove bursts lost maps - the file is the safe way)
    if ($s.State -eq 'live') { Invoke-Rcon $s @(, @('mapList.load')) | Out-Null }
}

function Refresh-Servers {
    $procs = Get-ServerProcs
    foreach ($n in 1..4) {
        $s = $srv[$n]
        $s.Pid = Find-ServerPid $s $procs
        $s.Info = $null
        if ($s.Pid) {
            $r = Invoke-Rcon $s @(, @('serverInfo'))
            if ($r -and $r[0][0] -eq 'OK') { $s.State = 'live'; $s.Info = $r[0] } else { $s.State = 'starting' }
        } else { $s.State = 'off' }
        $st = $ui["CardState$n"]; $mp = $ui["CardMap$n"]; $pl = $ui["CardPlayers$n"]
        $ui["CardName$n"].Text = $s.Name
        $picMap = ''
        if ($s.State -eq 'live') { $picMap = $s.Info[5] } else { $rws = Read-Rows $s; if ($rws.Count) { $picMap = $rws[0][0] } }
        $s.PicMap = $picMap
        $ui["CardImg$n"].Source = $(if ($picMap) { Get-Thumb $picMap } else { $null })
        switch ($s.State) {
            'live' {
                $st.Text = 'LIVE'; $st.Foreground = $cWhite
                $mp.Text = "$(Map-Name $s.Info[5])  /  $(Mode-Name $s.Info[4])"
                $pl.Text = "$($s.Info[2])/$($s.Info[3]) PLAYERS   ROUND $([int]$s.Info[6] + 1)/$($s.Info[7])"
            }
            'starting' { $st.Text = 'STARTING'; $st.Foreground = $cOrange; $mp.Text = 'loading...'; $pl.Text = '' }
            default {
                $st.Text = 'OFFLINE'; $st.Foreground = $cDim
                $rows = Read-Rows $s
                $mp.Text = $(if ($rows.Count) { "first: $(Map-Name $rows[0][0])  /  $(Mode-Name $rows[0][1])" } else { 'no maps in the list' })
                $pl.Text = "$($set["Bots$n"]) bots$(if ($set["BotsOn$n"] -ne '1') { ' (Thugbot off)' })"
            }
        }
        $ui["Card$n"].BorderBrush = $(if ($n -eq $script:sel) { $cOrange } elseif ($s.State -eq 'live') { $cWhite } else { $cEdge })
    }
    Update-Selected
}

function Update-Selected {
    $s = Cur-Srv; $n = $s.N
    $ui.SelTitle.Text = "SERVER $n"; $ui.SelName.Text = $s.Name
    $ui.SelImg.Source = $(if ($s.PicMap) { Get-Thumb $s.PicMap } else { $null })
    $ui.SelMap.Text = $(if ($s.State -eq 'live' -and $s.Info) { "NOW: $(Map-Name $s.Info[5])  /  $(Mode-Name $s.Info[4])" } elseif ($s.PicMap) { "STARTS ON: $(Map-Name $s.PicMap)" } else { '' })
    $ui.SelPorts.Text = "GAME $($s.GamePort)   RCON $($s.RconPort)   $($s.Login)"
    $ui.JoinText.Text = "$([char]0x25B6) JOIN SERVER $n"
    $ui.RotTitle.Text = "MAP LIST // SERVER $n"
    switch ($s.State) {
        'live' { $ui.SelStatus.Text = "LIVE: $(Map-Name $s.Info[5]), $(Mode-Name $s.Info[4])`n$($s.Info[2]) of $($s.Info[3]) players"; $ui.SelStatus.Foreground = $cWhite }
        'starting' { $ui.SelStatus.Text = 'STARTING (not answering RCON yet)...'; $ui.SelStatus.Foreground = $cOrange }
        default { $ui.SelStatus.Text = 'OFFLINE'; $ui.SelStatus.Foreground = $cDim }
    }
    $ui.TxtBots.Text = $set["Bots$n"]
    $on = $set["BotsOn$n"] -eq '1'
    $ui.BtnBotsOn.Content = $(if ($on) { 'THUGBOT: ON' } else { 'THUGBOT: OFF' })
    $ui.BtnStart.IsEnabled = $s.State -eq 'off'
    $ui.BtnStop.IsEnabled = $s.State -ne 'off'
    $ui.BtnRestart.IsEnabled = $s.State -ne 'off'
    $ui.BtnRound.IsEnabled = $s.State -eq 'live'
    $ui.BtnNext.IsEnabled = $s.State -eq 'live'
    $ui.BtnJoin.IsEnabled = $s.State -eq 'live'
}

function Fill-Rotation([int]$keep = -1) {
    $s = Cur-Srv; $rows = Read-Rows $s
    $curMap = ''; $curMode = ''
    if ($s.State -eq 'live' -and $s.Info) { $curMap = $s.Info[5]; $curMode = $s.Info[4] }
    $ui.LstRot.Items.Clear()
    for ($i = 0; $i -lt $rows.Count; $i++) {
        $r = $rows[$i]
        $tag = if ($curMap -and $r[0] -ieq $curMap -and $r[1] -ieq $curMode) { '  << NOW' } elseif ($i -eq 0) { '  << FIRST' } else { '' }
        [void]$ui.LstRot.Items.Add((Map-Item $r[0] ('{0,2}  {1,-24} {2}{3}' -f ($i + 1), (Map-Name $r[0]), (Mode-Name $r[1]), $tag)))
    }
    $ui.RotCount.Text = "$($rows.Count) MAPS"
    if ($keep -ge 0 -and $keep -lt $ui.LstRot.Items.Count) { $ui.LstRot.SelectedIndex = $keep }
}

function Select-Server([int]$n) {
    $script:sel = $n; $set.Selected = "$n"; Save-Settings
    Refresh-Servers; Fill-Rotation
}

# the bot start file for server n: made once from BF-Portable's own !StartServerN.bat (its name, ports, password), with the
# engine's pseudo-players added - the same way Start-FloodBots.ps1 makes !StartServer-FloodBots.bat for server 1
function Get-BotBat($s, [int]$bots) {
    $bat = $s.BotBat
    if (-not (Test-Path $bat)) {
        if (-not (Test-Path $s.Stock)) { throw "Not found: $($s.Stock)" }
        $lines = Get-Content $s.Stock
        if (-not ($lines | Where-Object { $_ -match '^\s*start\s+.*bfh_server_Final\.exe' })) { throw "$($s.Stock) has no 'start bfh_server_Final.exe' line" }
        $out = @("rem Thugbot launcher for server $($s.N): a copy of $(Split-Path $s.Stock -Leaf) plus the engine's pseudo-player bots (Thugbot-Launcher made it).",
            'set BOTS=60',
            'set PSEUDO=-PseudoPlayer.SpawnCount %BOTS% -PseudoPlayer.SetOnlineId true -PseudoPlayer.SuicideRate -1 -PseudoPlayer.ReconnectRate -1 -PseudoPlayer.UpdateInput true -PseudoPlayer.KillRate -1 -PseudoPlayer.DamageRate -1 -PseudoPlayer.ScoreRate -1')
        foreach ($l in $lines) { if ($l -match '^\s*start\s+.*bfh_server_Final\.exe') { $out += "$l %PSEUDO%" } elseif ($l -notmatch '^\s*pause\s*$') { $out += $l } }
        Set-Content $bat $out -Encoding ascii
        Say "Made $(Split-Path $bat -Leaf) from $(Split-Path $s.Stock -Leaf)"
    }
    $text = Get-Content $bat -Raw
    $text = $text -replace '(?m)^set BOTS=\d+', "set BOTS=$bots"
    Set-Content $bat $text -NoNewline -Encoding ascii
    return $bat
}

# runs a program, waits, returns @(exitcode, output) - no console window, stderr kept apart (PowerShell 5 turns
# redirected stderr into errors)
function Run-Exe([string]$exe, [string]$argLine, [string]$dir = $PSScriptRoot) {
    $psi = New-Object Diagnostics.ProcessStartInfo $exe, $argLine
    $psi.WorkingDirectory = $dir; $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    $p = [Diagnostics.Process]::Start($psi)
    $errTask = $p.StandardError.ReadToEndAsync(); $outTask = $p.StandardOutput.ReadToEndAsync()
    while (-not $p.HasExited) { Pump; Start-Sleep -Milliseconds 100 }
    return , @($p.ExitCode, ($outTask.Result + $errTask.Result))
}

# Thugbot into the server folder - only while no Hardline server runs (all four servers share that folder and its DLLs)
function Install-Bots {
    if (Get-ServerProcs) { Say 'Thugbot already in place (another server is running from the same folder).'; return $true }
    $useOut = Test-Path (Join-Path $PSScriptRoot 'Thugbot-UseOut.txt')
    $vc = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
    $buildBat = Join-Path $PSScriptRoot 'build.bat'
    if (-not $useOut -and (Test-Path $buildBat) -and (Test-Path $vc)) {
        Say 'Building Thugbot (build.bat)...'
        $r = Run-Exe 'cmd.exe' "/c `"`"$buildBat`"`""
        if ($r[0] -ne 0) { Say 'build.bat failed - the server was not started. (Run build.bat by hand to see the errors.)'; return $false }
    }
    Say $(if ($useOut) { 'Installing out\ThugbotCore.dll as it is (Thugbot-UseOut.txt)...' } else { 'Installing Thugbot...' })
    $r = Run-Exe 'powershell.exe' "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $PSScriptRoot 'Install-Thugbot.ps1')`" -Server `"$server`""
    if ($r[0] -ne 0) { Say "Install failed: $(($r[1] -split "`n" | Where-Object { $_.Trim() } | Select-Object -Last 1))"; return $false }
    return $true
}

function Start-Helpers {
    # chat bridge (!wp / !rec / !save ...) and route hotkeys (F5 - F11), hidden; both quit once the server is gone
    $running = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue | ForEach-Object { $_.CommandLine })
    foreach ($h in @(@('Thugbot-Chat.ps1', @('-Portable', "`"$Portable`"")), @('Thugbot-Keys.ps1', @('-Portable', "`"$Portable`"", '-Player', "`"$($cfg.Name)`"")))) {
        if ($running | Where-Object { $_ -and $_ -match [regex]::Escape($h[0]) }) { continue }
        $p = Join-Path $PSScriptRoot $h[0]
        if (-not (Test-Path $p)) { continue }
        Start-Process powershell -ArgumentList (@('-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden', '-File', "`"$p`"") + $h[1]) -WindowStyle Hidden | Out-Null
    }
}

function Start-Bf4([int]$n) {
    $s = $srv[$n]
    Refresh-Servers
    if ($s.State -ne 'off') { Say "Server $n is already running."; return }
    if (-not (Start-Backend)) { return }
    $botsOn = $set["BotsOn$n"] -eq '1'
    $bots = [int]$set["Bots$n"]
    if ($botsOn) { if (-not (Install-Bots)) { return } }
    if ($botsOn -and $bots -gt 0) { $bat = Get-BotBat $s $bots } else { $bat = $s.Stock }
    if (-not (Test-Path $bat)) { Say "Not found: $bat"; return }
    $rows = Read-Rows $s
    Say "Starting server $n$(if ($botsOn -and $bots -gt 0) { " with $bots bots" } else { ' (no bots)' })$(if ($rows.Count) { " on $(Map-Name $rows[0][0]), $(Mode-Name $rows[0][1])" })..."
    Start-Process -FilePath $bat -WorkingDirectory $server | Out-Null
    if ($botsOn -and $n -eq 1) { Start-Helpers }
    Wait-Ms 1500; Refresh-Servers; Fill-Rotation
}

function Stop-Bf4([int]$n) {
    $s = $srv[$n]
    Refresh-Servers
    if ($s.State -eq 'off') { Say "Server $n is not running."; return }
    Say "Stopping server $n..."
    Invoke-Rcon $s @(, @('admin.shutDown')) | Out-Null
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Process -Id $s.Pid -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Wait-Ms 400 }
    if (Get-Process -Id $s.Pid -ErrorAction SilentlyContinue) { Stop-Process -Id $s.Pid -Force -ErrorAction SilentlyContinue }
    # (2026-10-30: the restart refused because the old process was still there - wait until it has really gone)
    $gone = (Get-Date).AddSeconds(10)
    while ((Get-Process -Id $s.Pid -ErrorAction SilentlyContinue) -and (Get-Date) -lt $gone) { Wait-Ms 300 }
    Say "Server $n stopped."
    Refresh-Servers; Fill-Rotation
}

function Rcon-Do($s, [object[]]$cmds, [string]$what) {
    $r = Invoke-Rcon $s $cmds
    if (-not $r) { Say "Server $($s.N) is not answering."; return $false }
    $bad = @($r | Where-Object { $_[0] -ne 'OK' })
    if ($bad.Count) { Say "$what failed: $($bad[0] -join ' ')"; return $false }
    Say "$what."; return $true
}

# ---------------------------------------------------------------- join
# The web server browser's own link (htdocs\getServerBrowserh.php): bfhlan:// + base64 of the game's start line, with the
# server's game id from the database. The bfhlan handler (BFUlauncher) starts lanbfh.exe with it.
function Get-GameId($s) {
    $mysql = Join-Path $xampp 'mysql\bin\mysql.exe'
    if (Test-Path $mysql) {
        $q = "SELECT gid FROM a_bf_gameservers WHERE mail='$($s.Login)' AND last > UNIX_TIMESTAMP(NOW() - INTERVAL 60 SECOND) ORDER BY gid DESC LIMIT 1"
        $r = Run-Exe $mysql "-h localhost -u api -papi -N -B bf -e `"$q`"" $xampp
        $m = [regex]::Match($r[1], '(?m)^\s*(\d+)\s*$')
        if ($m.Success) { return [int]$m.Groups[1].Value }
    }
    # fallback: the server-list API (as the BF Launcher calls it), matched by server name
    $ini = Join-Path $launcherDir 'Settings.ini'; $key = ''
    if (Test-Path $ini) { foreach ($l in Get-Content $ini) { if ($l -match '^APIKey=(\S+)') { $key = $Matches[1] } } }
    try {
        $list = Invoke-RestMethod -Uri "http://127.0.0.1/api/get_servers_bfh.php?key=$key" -TimeoutSec 4
        foreach ($x in @($list)) { if ($x.gnam -eq $s.Name) { return [int]$x.gid } }
    } catch {}
    return 0
}
function Get-Login {
    # what the last working join used (the game's own log), else the BF Launcher's saved auth code
    $auth = ''; $persona = $set.Persona
    $glog = if ($clientDir) { Join-Path $clientDir 'bf_global.log' } else { '' }
    if ($glog -and (Test-Path $glog)) {
        $t = Get-Content $glog -Raw
        $a = [regex]::Matches($t, '-AuthCode (\S+)'); if ($a.Count) { $auth = $a[$a.Count - 1].Groups[1].Value }
        $p = [regex]::Matches($t, 'personaref=\\?"(\d+)'); if ($p.Count -and -not $persona) { $persona = $p[$p.Count - 1].Groups[1].Value }
    }
    if (-not $auth) { $f = Join-Path $launcherDir 'authcode.txt'; if (Test-Path $f) { $auth = (Get-Content $f -Raw).Trim() } }
    if ($persona -and -not $set.Persona) { $set.Persona = $persona; Save-Settings }
    return , @($auth, $persona)
}
function Join-Bf4([int]$n) {
    $s = $srv[$n]
    if ($s.State -ne 'live') { Say "Server $n is not up yet."; return }
    Say "Finding server $n's game id..."
    $gid = Get-GameId $s
    if (-not $gid) { Say "Server $n is not in the server list yet (give it a few seconds after it loads, then JOIN again)."; return }
    $lg = Get-Login
    if (-not $lg[0] -or -not $lg[1]) { Say 'No login found: join once from the web server browser (that saves it), then JOIN works from here.'; return }
    $blaze = '127.0.0.1'
    $line = " -blazeIp $blaze -Render.DebugRendererEnable false -Core.IgnoreAlways true -Core.EnableJuice false -DisplayAsserts false -WaterInteract.Enable 0 -super layout.toc -dataPlatform Win32 -webMode MP -Origin_NoAppFocus -AuthCode $($lg[0]) -requestState State_ClaimReservation -requestStateParams `"<data levelmode=\`"mp\`" personaref=\`"$($lg[1])\`" isspectator=\`"false\`" role=\`"soldier\`" gameid=\`"$gid\`" putinsquad=\`"true\`"></data>`"|$blaze|"
    $url = 'bfhlan://' + [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes($line))
    Say "Joining $($s.Name) (game $gid)..."
    $handler = Test-Path 'Registry::HKEY_CLASSES_ROOT\bfhlan'
    $ulaunch = if ($clientDir) { Join-Path $clientDir 'BFUlauncher.exe' } else { '' }
    if ($handler) { Start-Process $url }
    elseif ($ulaunch -and (Test-Path $ulaunch)) { Start-Process -FilePath $ulaunch -ArgumentList $url -WorkingDirectory $clientDir }
    else { Say 'The bfhlan:// link handler is not installed (BF-Portable client setup), and BFUlauncher.exe was not found.' }
}

# ---------------------------------------------------------------- vehicles (Thugbot.ini, shared by all servers)
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
function Update-Veh { $on = (Ini-Get 'Vehicles' '0') -eq '1'; $ui.BtnVeh.Content = $(if ($on) { 'VEHICLES: ON' } else { 'VEHICLES: OFF' }) }

# ---------------------------------------------------------------- wire up
foreach ($c in $mapCodes) { [void]$ui.LstMaps.Items.Add((Map-Item $c (Map-Name $c))) }
foreach ($m in $modes) { [void]$ui.LstModes.Items.Add($(if ($m[2]) { "$([char]0x2605) $($m[1])" } else { $m[1] })) }
$ui.LstModes.SelectedIndex = 0

foreach ($n in 1..4) { $ui["Card$n"].Add_Click({ $t = [int]$this.Tag; Safe { Select-Server $t } }) }
$ui.BtnBackStart.Add_Click({ Safe { if (Start-Backend) { Say 'Backend running: MySQL, web server, redirector, Hardline emulator.' } } })
$ui.BtnBackStop.Add_Click({ Safe { Stop-Backend } })
$ui.BtnBotsDn.Add_Click({ Safe { $n = $script:sel; $v = [Math]::Max(0, [int]$set["Bots$n"] - 1); $set["Bots$n"] = "$v"; Save-Settings; Update-Selected } })
$ui.BtnBotsUp.Add_Click({ Safe { $n = $script:sel; $v = [Math]::Min(63, [int]$set["Bots$n"] + 1); $set["Bots$n"] = "$v"; Save-Settings; Update-Selected } })
$ui.BtnBotsDn.Add_MouseRightButtonUp({ Safe { $n = $script:sel; $set["Bots$n"] = '0'; Save-Settings; Update-Selected } })
$ui.BtnBotsUp.Add_MouseRightButtonUp({ Safe { $n = $script:sel; $set["Bots$n"] = '63'; Save-Settings; Update-Selected } })
$ui.BtnBotsOn.Add_Click({ Safe { $n = $script:sel; $set["BotsOn$n"] = $(if ($set["BotsOn$n"] -eq '1') { '0' } else { '1' }); Save-Settings; Update-Selected; Say "Server ${n}: Thugbot $(if ($set["BotsOn$n"] -eq '1') { 'on' } else { 'off' }) from its next start." } })
$ui.BtnStart.Add_Click({ Safe { Start-Bf4 $script:sel } })
$ui.BtnStop.Add_Click({ Safe { Stop-Bf4 $script:sel } })
$ui.BtnRestart.Add_Click({ Safe { $n = $script:sel; Stop-Bf4 $n; Wait-Ms 2000; Start-Bf4 $n } })
$ui.BtnRound.Add_Click({ Safe { Rcon-Do (Cur-Srv) @(, @('mapList.restartRound')) 'Restarting the round' | Out-Null } })
$ui.BtnNext.Add_Click({ Safe { Rcon-Do (Cur-Srv) @(, @('mapList.runNextRound')) 'Moving to the next map' | Out-Null } })
$ui.BtnVeh.Add_Click({ Safe {
    $v = if ((Ini-Get 'Vehicles' '0') -eq '1') { '0' } else { '1' }
    Ini-Set 'Vehicles' $v
    try { Set-Content (Join-Path $server 'Thugbot_cmd.txt') "vehicles $v" -Encoding ascii } catch {}
    Update-Veh; Say $(if ($v -eq '1') { 'Bots now use vehicles (all servers).' } else { 'Bots stay on foot (all servers).' })
} })
$ui.BtnJoin.Add_Click({ Safe { Join-Bf4 $script:sel } })
$ui.BtnLearned.Add_Click({ Safe { $p = Join-Path $reportDir 'latest.html'; if (Test-Path $p) { Show-Web (New-Object Uri $p).AbsoluteUri 'the learned map (latest)' } else { Say 'No learned map yet - it appears a minute after a round starts.' } } })
$ui.Btn3D.Add_Click({ Safe { Show-3D } })
$ui.BtnClassic.Add_Click({ Safe { $p = Join-Path $PSScriptRoot 'Thugbot-Control.bat'; if (Test-Path $p) { Start-Process -FilePath $p -WorkingDirectory $PSScriptRoot } } })

# map list editing (the selected server's MapList.txt)
$ui.BtnUp.Add_Click({ Safe {
    $i = $ui.LstRot.SelectedIndex; if ($i -lt 1) { return }
    $rows = Read-Rows (Cur-Srv); $t = $rows[$i - 1]; $rows[$i - 1] = $rows[$i]; $rows[$i] = $t; Write-Rows (Cur-Srv) $rows; Fill-Rotation ($i - 1)
} })
$ui.BtnDown.Add_Click({ Safe {
    $i = $ui.LstRot.SelectedIndex; $rows = Read-Rows (Cur-Srv); if ($i -lt 0 -or $i -ge $rows.Count - 1) { return }
    $t = $rows[$i + 1]; $rows[$i + 1] = $rows[$i]; $rows[$i] = $t; Write-Rows (Cur-Srv) $rows; Fill-Rotation ($i + 1)
} })
$ui.BtnRemove.Add_Click({ Safe {
    $i = $ui.LstRot.SelectedIndex; $rows = Read-Rows (Cur-Srv); if ($i -lt 0 -or $i -ge $rows.Count) { return }
    if ($rows.Count -le 1) { Say 'A server needs at least one map.'; return }
    $new = @(); for ($k = 0; $k -lt $rows.Count; $k++) { if ($k -ne $i) { $new += , $rows[$k] } }
    Write-Rows (Cur-Srv) $new; Fill-Rotation ([Math]::Min($i, $new.Count - 1)); Refresh-Servers
} })
# PLAY: the chosen row goes to the top (the list rotated, nothing lost - BF-Portable's server always loads entry 0);
# a running server reloads the list and ends the round into it
$ui.BtnPlay.Add_Click({ Safe {
    $s = Cur-Srv; $i = $ui.LstRot.SelectedIndex; $rows = Read-Rows $s; if ($i -lt 0 -or $i -ge $rows.Count) { Say 'Pick a map in the list first.'; return }
    $r = $rows[$i]
    $new = @($rows[$i..($rows.Count - 1)]); if ($i -gt 0) { $new += $rows[0..($i - 1)] }
    $was = $s.State; $s.State = 'off'; Write-Rows $s $new; $s.State = $was
    if ($s.State -eq 'live') {
        if (Rcon-Do $s @(@('mapList.load'), @('mapList.setNextMapIndex', '0')) "Queued $(Map-Name $r[0]), $(Mode-Name $r[1])") {
            Rcon-Do $s @(, @('mapList.runNextRound')) "Switching server $($s.N) to $(Map-Name $r[0]), $(Mode-Name $r[1]) (loading takes a moment)" | Out-Null
        }
    } else { Say "Server $($s.N) will start on $(Map-Name $r[0]), $(Mode-Name $r[1])." }
    Fill-Rotation 0; Refresh-Servers
} })
function Sel-Mode { $i = $ui.LstModes.SelectedIndex; if ($i -lt 0) { $i = 0 }; $modes[$i] }
$ui.BtnAdd.Add_Click({ Safe {
    $picked = @($ui.LstMaps.SelectedItems | ForEach-Object { $ui.LstMaps.Items.IndexOf($_) } | Sort-Object)
    if (-not $picked.Count) { Say 'Pick one or more maps on the right first (Ctrl / Shift for several).'; return }
    $m = Sel-Mode; $rows = Read-Rows (Cur-Srv); $added = 0
    foreach ($k in $picked) {
        $code = $mapCodes[$k]
        if ($rows | Where-Object { $_[0] -ieq $code -and $_[1] -ieq $m[0] }) { continue }
        $rows += , @($code, $m[0], '1'); $added++
    }
    Write-Rows (Cur-Srv) $rows; Fill-Rotation; Refresh-Servers
    Say "Added $added map(s) in $($m[1]) to server $script:sel."
} })
$ui.BtnAllMode.Add_Click({ Safe {
    $m = Sel-Mode; $rows = Read-Rows (Cur-Srv); if (-not $rows.Count) { return }
    $new = @(); $seen = @{}
    foreach ($r in $rows) { $k = $r[0].ToLower(); if ($seen[$k]) { continue }; $seen[$k] = 1; $new += , @($r[0], $m[0], $r[2]) }
    Write-Rows (Cur-Srv) $new; Fill-Rotation; Refresh-Servers
    Say "Server ${script:sel}: every map now plays $($m[1])."
} })
$ui.BtnFill.Add_Click({ Safe {
    $m = Sel-Mode
    $ok = [Windows.MessageBox]::Show("Replace server $($script:sel)'s map list with all $($mapCodes.Count) maps in $($m[1])?", 'Thugbot', 'YesNo')
    if ("$ok" -ne 'Yes') { return }
    $rows = @(foreach ($c in $mapCodes) { , @($c, $m[0], '1') })
    Write-Rows (Cur-Srv) $rows; Fill-Rotation; Refresh-Servers
    Say "Server ${script:sel}: all maps in $($m[1])."
} })

# ---------------------------------------------------------------- maps page (3D map + learned maps in a WebView2)
$script:wv = $null; $script:webUrl = ''; $script:nodeProc = $null; $script:reportPaths = @()
function Show-Page([string]$page) {
    $maps = $page -eq 'maps'
    $ui.PageServers.Visibility = $(if ($maps) { 'Collapsed' } else { 'Visible' })
    $ui.PageMaps.Visibility = $(if ($maps) { 'Visible' } else { 'Collapsed' })
    $ui.TabServers.Style = $win.FindResource($(if ($maps) { 'Btn' } else { 'OBtn' }))
    $ui.TabMaps.Style = $win.FindResource($(if ($maps) { 'OBtn' } else { 'Btn' }))
    if ($maps) { Fill-Reports }
}
# learned maps: latest.html (the map being played) first, then every map's own page, newest first
function Fill-Reports {
    $ui.LstReports.Items.Clear(); $script:reportPaths = @()
    if (-not (Test-Path $reportDir)) { return }
    $latest = Join-Path $reportDir 'latest.html'
    if (Test-Path $latest) { [void]$ui.LstReports.Items.Add((Map-Item '' 'LATEST (map being played)')); $script:reportPaths += $latest }
    foreach ($f in @(Get-ChildItem $reportDir -Filter *.html -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne 'latest.html' } | Sort-Object LastWriteTime -Descending)) {
        $base = $f.BaseName; $i = $base.LastIndexOf('_')
        $code = if ($i -gt 0) { $base.Substring(0, $i) } else { $base }; $mode = if ($i -gt 0) { $base.Substring($i + 1) } else { '' }
        [void]$ui.LstReports.Items.Add((Map-Item $code "$(Map-Name $code)`n$(Mode-Name $mode)  $($f.LastWriteTime.ToString('dd MMM HH:mm'))"))
        $script:reportPaths += $f.FullName
    }
}
# Microsoft's WebView2 .NET files (the Edge engine itself comes with Windows) - downloaded once, Microsoft-signed files only
function Install-WebView2Lib {
    $ver = '1.0.2210.55'
    $url = "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/$ver"
    $tmp = Join-Path $env:TEMP "Microsoft.Web.WebView2.$ver.zip"
    Say "Downloading Microsoft WebView2 $ver from nuget.org..."
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $wc = New-Object Net.WebClient
    $t = $wc.DownloadFileTaskAsync($url, $tmp)
    while (-not $t.IsCompleted) { Pump; Start-Sleep -Milliseconds 100 }
    if ($t.IsFaulted) { Say "Download failed: $($t.Exception.InnerException.Message)"; return $false }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    New-Item -ItemType Directory -Force $libDir | Out-Null
    $zip = [IO.Compression.ZipFile]::OpenRead($tmp)
    try {
        foreach ($name in 'Microsoft.Web.WebView2.Core.dll', 'Microsoft.Web.WebView2.Wpf.dll') {
            $e = @($zip.Entries | Where-Object { $_.FullName -match ('^lib/net4[0-9]*/' + [regex]::Escape($name) + '$') } | Sort-Object FullName)[0]
            if (-not $e) { Say "The WebView2 package has no $name"; return $false }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, (Join-Path $libDir $name), $true)
        }
        $e = @($zip.Entries | Where-Object { $_.FullName -eq 'runtimes/win-x64/native/WebView2Loader.dll' })[0]
        if (-not $e) { Say 'The WebView2 package has no 64-bit WebView2Loader.dll'; return $false }
        [IO.Compression.ZipFileExtensions]::ExtractToFile($e, (Join-Path $libDir 'WebView2Loader.dll'), $true)
    } finally { $zip.Dispose(); Remove-Item $tmp -ErrorAction SilentlyContinue }
    foreach ($f in Get-ChildItem $libDir -Filter *.dll) {
        $sig = Get-AuthenticodeSignature $f.FullName
        if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'Microsoft') {
            Remove-Item $libDir -Recurse -Force -ErrorAction SilentlyContinue
            Say "$($f.Name) is not signed by Microsoft - removed, maps open in your browser instead."; return $false
        }
    }
    Say 'WebView2 ready.'
    return $true
}
function Ensure-WebView {
    if ($script:wv) { return $true }
    if (-not (Test-Path (Join-Path $libDir 'Microsoft.Web.WebView2.Wpf.dll')) -or -not (Test-Path (Join-Path $libDir 'WebView2Loader.dll'))) {
        $ok = [Windows.MessageBox]::Show("The 3D map and learned maps show inside this window with Microsoft Edge WebView2.`n`nDownload Microsoft's WebView2 package (about 10 MB, from nuget.org) into lib\WebView2 now? (No: they open in your browser.)", 'Maps', 'YesNo')
        if ("$ok" -ne 'Yes') { return $false }
        if (-not (Install-WebView2Lib)) { return $false }
    }
    try {
        if ($env:PATH -notlike "*$libDir*") { $env:PATH = "$libDir;$env:PATH" }   # WebView2Loader.dll is found on the PATH
        Add-Type -Path (Join-Path $libDir 'Microsoft.Web.WebView2.Core.dll')
        Add-Type -Path (Join-Path $libDir 'Microsoft.Web.WebView2.Wpf.dll')
        try { [void][Microsoft.Web.WebView2.Core.CoreWebView2Environment]::GetAvailableBrowserVersionString() }
        catch { Say 'The Microsoft Edge WebView2 Runtime is not installed (get it from https://go.microsoft.com/fwlink/p/?LinkId=2124703) - opening in your browser.'; return $false }
        $w = New-Object Microsoft.Web.WebView2.Wpf.WebView2
        $cp = New-Object Microsoft.Web.WebView2.Wpf.CoreWebView2CreationProperties
        $cp.UserDataFolder = $webData
        $w.CreationProperties = $cp
        [void]$ui.WebHost.Children.Add($w)
        $script:wv = $w
        return $true
    } catch { Say "The built-in map view could not start ($($_.Exception.Message)) - opening in your browser."; return $false }
}
function Show-Web([string]$url, [string]$what) {
    Show-Page 'maps'
    $script:webUrl = $url
    if (Ensure-WebView) { $ui.WebMsg.Visibility = 'Collapsed'; $script:wv.Source = New-Object Uri $url; Say "Showing $what." }
    else { Start-Process $url; Say "Opened $what in your browser." }
}
function Show-3D {
    if (-not (Test-Path (Join-Path $map3dDir 'server.js'))) { Say "3D map not found: $map3dDir"; return }
    if (-not (Port-Open $map3dPort)) {
        $node = Get-Command node -ErrorAction SilentlyContinue
        if (-not $node) { Say 'Node.js is not installed - get it from https://nodejs.org (the 3D map needs it).'; return }
        Say 'Starting the 3D map server...'
        # (hidden; stopped again when the launcher closes)
        $script:nodeProc = Start-Process -FilePath $node.Source -ArgumentList 'server.js' -WorkingDirectory $map3dDir -WindowStyle Hidden -PassThru
        for ($i = 0; $i -lt 30 -and -not (Port-Open $map3dPort); $i++) { Wait-Ms 300 }
        if (-not (Port-Open $map3dPort)) { Say 'The 3D map server did not start (run Open-Thugbot-3DMap.bat to see why).'; return }
    }
    Show-Web "http://localhost:$map3dPort/" 'the 3D map'
}
function Show-Report([int]$i) {
    if ($i -lt 0 -or $i -ge $script:reportPaths.Count) { return }
    Show-Web (New-Object Uri $script:reportPaths[$i]).AbsoluteUri "the learned map ($([IO.Path]::GetFileNameWithoutExtension($script:reportPaths[$i])))"
}
$ui.TabServers.Add_Click({ Safe { Show-Page 'servers' } })
$ui.TabMaps.Add_Click({ Safe { Show-Page 'maps' } })
$ui.BtnShow3D.Add_Click({ Safe { Show-3D } })
$ui.LstReports.Add_SelectionChanged({ Safe { Show-Report $ui.LstReports.SelectedIndex } })
$ui.BtnMapReload.Add_Click({ Safe { if ($script:wv -and $script:webUrl) { $script:wv.Reload() }; Fill-Reports } })
$ui.BtnMapBrowser.Add_Click({ Safe { if ($script:webUrl) { Start-Process $script:webUrl } else { Say 'Pick a map first.' } } })

# ---------------------------------------------------------------- glitch title
$rand = New-Object Random
$tO = New-Object Windows.Media.TranslateTransform; $tB = New-Object Windows.Media.TranslateTransform; $tW = New-Object Windows.Media.TranslateTransform
$ui.TitleO.RenderTransform = $tO; $ui.TitleB.RenderTransform = $tB; $ui.TitleW.RenderTransform = $tW
$gl = @{ burst = 0; wait = 30; tick = 0 }
$glyphs = @('THUGBOT', 'THU6BOT', 'THUGB0T', 'T|-|UGBOT', 'THUG8OT', 'THUGBO7', 'THUG_OT')
$subs = @('HARDLINE // COPS VS CRIMINALS // LAN BOTS', 'HARDL1NE // COPS VS CRIM1NALS // LAN BOTS', 'HARDLINE // C0PS VS CRIMINALS // L4N BOTS', 'HARDLINE // ##PS VS CRIMINALS // LAN BOTS')
function Band { $y = $rand.Next(0, 70); $h = $rand.Next(6, 22); [Windows.Media.RectangleGeometry]::new([Windows.Rect]::new(0, $y, 440, $h)) }
$glTimer = New-Object Windows.Threading.DispatcherTimer
$glTimer.Interval = [TimeSpan]::FromMilliseconds(55)
$glTimer.Add_Tick({
    $gl.tick++
    if ($gl.burst -gt 0) {
        $gl.burst--
        $tO.X = $rand.Next(-12, 13); $tO.Y = $rand.Next(-3, 4); $tB.X = $rand.Next(-12, 13); $tB.Y = $rand.Next(-3, 4)
        $tW.X = $(if ($rand.Next(3) -eq 0) { $rand.Next(-5, 6) } else { 0 })
        $ui.TitleO.Clip = Band; $ui.TitleB.Clip = Band
        $ui.TitleW.Text = $glyphs[$rand.Next($glyphs.Count)]
        $ui.Slice1.Margin = [Windows.Thickness]::new(0, $rand.Next(0, 76), 0, 0); $ui.Slice1.Opacity = $(if ($rand.Next(2)) { 0.9 } else { 0 })
        $ui.Slice2.Margin = [Windows.Thickness]::new($rand.Next(0, 200), $rand.Next(0, 76), 0, 0); $ui.Slice2.Opacity = $(if ($rand.Next(2)) { 0.9 } else { 0 })
        if ($rand.Next(3) -eq 0) { $ui.Sub.Text = $subs[$rand.Next($subs.Count)] }
    } else {
        # resting: a steady split of the two colours with a slow drift
        $d = [Math]::Sin($gl.tick / 9.0)
        $tO.X = -2 - $d; $tO.Y = 0; $tB.X = 2 + $d; $tB.Y = 0; $tW.X = 0
        $ui.TitleO.Clip = $null; $ui.TitleB.Clip = $null
        $ui.TitleW.Text = 'THUGBOT'; $ui.Slice1.Opacity = 0; $ui.Slice2.Opacity = 0
        $ui.Sub.Text = $subs[0]
        $gl.wait--
        if ($gl.wait -le 0) { $gl.burst = $rand.Next(3, 9); $gl.wait = $rand.Next(25, 90) }
    }
})

# police lights: red flashes three times, then blue three times, with a white pop now and then
$cPolR = Brush '#FF2020'; $cPolB = Brush '#2A5CFF'; $cPolW = Brush '#FFFFFF'
$pol = @{ t = 0 }
$polTimer = New-Object Windows.Threading.DispatcherTimer
$polTimer.Interval = [TimeSpan]::FromMilliseconds(65)
$polTimer.Add_Tick({
    $p = $pol.t % 24; $pol.t++
    $r = ($p -eq 0 -or $p -eq 2 -or $p -eq 4); $b = ($p -eq 12 -or $p -eq 14 -or $p -eq 16)
    $w = ($p -eq 8 -or $p -eq 20) -and ($rand.Next(3) -eq 0)
    $ui.LightR.Opacity = $(if ($r) { 1 } else { 0.15 }); $ui.GlowR.Opacity = $(if ($r) { 1 } else { 0 })
    $ui.LightB.Opacity = $(if ($b) { 1 } else { 0.15 }); $ui.GlowB.Opacity = $(if ($b) { 1 } else { 0 })
    $ui.LightW.Opacity = $(if ($w) { 1 } else { 0.1 })
    $ui.TitleB.Foreground = $(if ($r) { $cPolR } elseif ($b) { $cPolB } else { $cPolW })
})

# status every 3 s
$stTimer = New-Object Windows.Threading.DispatcherTimer
$stTimer.Interval = [TimeSpan]::FromSeconds(3)
$stTimer.Add_Tick({ if ($script:busy) { return }; try { $before = (Cur-Srv).State; Refresh-Servers; Update-Services; if ((Cur-Srv).State -ne $before) { Fill-Rotation $ui.LstRot.SelectedIndex } } catch {} })

# the console window Thugbot-Launcher.bat opened goes away once this window is up (it stays if the launcher fails, to show why)
Add-Type -Name ConsoleWin -Namespace ThugbotLauncher -MemberDefinition '[DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow(); [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);'
$win.Add_Loaded({
    try { [void][ThugbotLauncher.ConsoleWin]::ShowWindow([ThugbotLauncher.ConsoleWin]::GetConsoleWindow(), 0) } catch {}
    Safe { Update-Services; Update-Veh; Select-Server $script:sel; $glTimer.Start(); $polTimer.Start(); $stTimer.Start(); Say 'Ready. START BACKEND, pick a server, START SERVER, then JOIN.' }
})
$win.Add_Closed({ $glTimer.Stop(); $polTimer.Stop(); $stTimer.Stop(); if ($script:nodeProc -and -not $script:nodeProc.HasExited) { try { $script:nodeProc.Kill() } catch {} } })
try { [void]$win.ShowDialog() } catch { Write-Host "Thugbot Launcher failed: $($_.Exception.Message)"; exit 1 }
