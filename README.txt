ThugBot - player bots for a Battlefield Hardline server (BF-Portable)
=====================================================================

ThugBot fills a Battlefield Hardline dedicated server with bots that play like players: they capture and
defend flags, fight with cover, crouch, vault, open doors, knife glass, throw grenades, use grenade
launchers and gadgets, camp upper-floor windows, and learn routes from you and from each other.
The bots run on the server only. Nothing is installed in the game client.


WHAT YOU NEED
-------------
- Windows, with BF-Portable installed (any folder) and its Hardline server working
  (Battlefield_BFH_Server\!StartServer.bat starts a normal server).
- Battlefield Hardline set up for BF-Portable on the PC you play on.
- Nothing else. Visual Studio is only needed to change the source code.


INSTALL (once)
--------------
1. Unzip this folder anywhere, e.g. Documents\ThugbotHardline.
2. Close the Hardline server if it is running.
3. Double-click Setup-Thugbot.bat. It looks on your drives for:
     - the BF-Portable backend (PortableBF.exe)
     - the Hardline server (bfh_server_Final.exe)
     - the Hardline Blaze emulator (BlazeServerH.exe)
     - the BF Launcher (optional) and the Hardline game (bfh.exe, optional)
   Anything it can't find is marked x: use Browse to point at it. Type your in-game name if you want the
   route hotkeys, then press "Save and install". The folders are saved in Thugbot-Paths.ini (every
   Thugbot script reads them from there - edit it or run Setup again if you move things).
   Installing backs up the server's original xinput9_1_0.dll (to Battlefield_BFH_Server\Thugbot_backup),
   puts the mod next to it, and copies Thugbot.ini and the learned map data (Data\) into the server
   folder. Files the server already has are kept. "Uninstall" in the same window puts the original back.


PLAY
----
Easiest: double-click Thugbot-Control.bat. The window lets you:
  - Start MySQL / the web server / everything the server needs (Services box)
  - Start, stop or restart the server with a number of bots per team (30 per team = 60 bots max)
  - Pick a map and mode and play it now, or edit the map rotation
  - Turn "Bots use vehicles" on or off (off by default)
  - Find LAN games: lists every BF-Portable game on your network
  - Open the "learned map" page (what the bots know about the current map)

Or double-click Start-Thugbot.bat. It starts BF-Portable, the Hardline Blaze emulator and the server
with 60 bots. Other bot counts and your in-game name for the hotkeys:
    Start-Thugbot.bat -Bots 24

The first time, Start-Thugbot makes Battlefield_BFH_Server\!StartServer-Bots.bat from your own
!StartServer.bat (same name, ports and admin password) with the bots added.

Then join the server from the game as usual. Bots play Conquest (Turf War) Large / Small and
Team Deathmatch properly; in the other modes they hunt enemies.

A map the bots have never seen is scanned when it first loads (10 - 60 seconds, bots use saved routes
meanwhile). It gets better the more it is played: bots learn doors, vaults, stuck spots and routes.


SETTINGS (Battlefield_BFH_Server\Thugbot.ini)
---------------------------------------------
  Difficulty   easy / normal / hard / veteran / elite   (aim, reaction, how far they see)
  Aggression   0 cautious, 1 normal, 2 aggressive
  DefendShare  part of each team that defends owned flags (0 - 0.8)
  Vehicles     1 = bots drive and ride vehicles, 0 = they stay on foot (default)
  Combat, Peek leave at 1
  AutoEquip   on by default in the bot core; switch with the 'autoequip 0/1' command
Changes apply when the server starts, or at once by writing "ini" into Battlefield_BFH_Server\Thugbot_cmd.txt.


TEACHING THE BOTS ROUTES (while you play)
-----------------------------------------
Every route you walk, every vault, every door you go through is learned automatically.
You can also mark routes yourself, with hotkeys (Start-Thugbot starts them; or run Thugbot-Keys.bat):
  F5  start recording (a point every 3 m)     F6  stop and save the route
  F7  add one point where you stand           F8  undo the last point
  F9  clear the points                        F10 elevator / ladder end (bottom, then top)
  F11 bots in your vehicle get out
or with chat commands: !wp  !rec  !stop  !save  !undo  !clear  !delete  !routes  !lift  !out
The hotkeys act for the player name given with -Player (Start-Thugbot / Thugbot-Keys).


FILES IN THIS FOLDER
--------------------
  out\                    the mod: xinput9_1_0.dll (loader) + ThugbotCore.dll (the bots)
  Thugbot.ini             default settings
  Data\Thugbot_nav        walkable-ground maps per level (rebuilt automatically when missing)
  Data\Thugbot_learned    doors, glass, vaults, elevators, stuck spots, player routes per level
  Data\Thugbot_routes     saved routes spawn -> flag and flag -> flag per level
  Setup-Thugbot.bat       find the folders, install / uninstall (see above)
  Thugbot-Paths.ini       the folders and your in-game name (made by Setup)
  Install-Thugbot.ps1     install without the window;  Uninstall-Thugbot.ps1 puts the original DLL back
  Start-Thugbot.bat       start everything with bots
  Thugbot-Control.bat     the control window
  Thugbot-Keys.bat        route hotkeys (F5 - F11)
  Thugbot-Chat.ps1        the chat-command bridge (started by Start-Thugbot)
  Open-Thugbot-Map.bat    the learned-map page
  Source\                 full source code, build.bat (Visual Studio 2019 Build Tools, x64), tools

In the server folder the mod writes Thugbot_25200.log (what the bots do) and reads commands
from Thugbot_cmd.txt (one per write), for example:
  bots        every bot's state into the log         routes      saved / learned route counts
  camp        window campers                         vehicles 1  bots use vehicles (0 = off)
  perf        how much server time the bots use      unblock     forget temporary blocked paths
  routes rebuild   re-make the current map's saved routes
  navrebuild       re-scan the current map
  weapons          show automatic primary-selection attempts (not proof of a loaded inventory)
  autoequip 0/1    disable/enable selecting a primary weapon on each on-foot spawn


UNINSTALL
---------
Stop the server, right-click Uninstall-Thugbot.ps1 > Run with PowerShell. The original DLL is put back.
The learned data folders (Thugbot_nav, Thugbot_learned, Thugbot_routes) stay; delete them if you like.


TROUBLESHOOTING
---------------
- "A Hardline server is already running": close it (or use Stop in the control window) and start again.
- No bots: check Thugbot_25200.log in the server folder. "core loaded" means the mod is running.
- MySQL won't start: run BF-Portable's Repair-MySQL-Privileges.bat, then Start MySQL again.
- Bots spawn with no weapon selected: check the log for 'weapon: ... requested primary selection'.
  The core requests the primary at 0.4, 1.9 and 4.5 seconds after each on-foot spawn.
  If they are still empty-handed, their loadout/inventory was not granted by the server:
  input actions cannot create a weapon. Use the 'weapons' command to see selection attempts.
- Bots stuck somewhere on a new map: give it a round; they learn the spot, and routes you walk
  there are used first. "routes rebuild" re-makes the saved routes of the current map.
- Server feels slow with 60 bots: use fewer bots per team.

Known limits: bots don't fly helicopters or drive boats; vehicle driving is basic (off by default);
the objective modes Heist / Blood Money / Hotwire / Crosshair / Rescue are played as "hunt the enemy".
