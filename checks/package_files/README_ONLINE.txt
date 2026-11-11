SONIC 3 A.I.R. - ONLINE CO-OP (STABLE FORK)
==========================================

Up to four players can join from separate computers. Each gets their own
full screen and controller slot. The host plays Sonic; player 2 is Tails,
player 3 is Knuckles, and player 4 is a second Sonic by default (or the
configured ESU character when an extra-character mod is installed).
Nothing else to install: the co-op mod is built in and switches itself on.


SETUP (once, on each computer)
------------------------------
1. Put your game ROM file "Sonic_Knuckles_wSonic3.bin" in  %APPDATA%\Sonic3AIR\
   (Win+R, type  %APPDATA%\Sonic3AIR , press Enter).
   The ROM is NOT included in this package.
2. Keep this folder together and run Sonic3AIR.exe from inside it.
3. Both computers must use the same version of this folder. Do not mix an older copy.
4. This fork is based on official stable Sonic 3 A.I.R. 26.03.28.0. It includes
   the engine and required engine data, but not the game ROM; each player supplies
   their own legally obtained ROM in the location above.


ONLINE LOBBY (the easy way)
---------------------------
Main menu -> ONLINE LOBBY.
HOST:   CREATE LOBBY. A code appears (like  ABCD-EFGH7 ); send it to your friends. COPY CODE puts it on the clipboard.
        Only people who have the code can join: it is also the password of your lobby. Do not post it publicly.
FRIENDS: JOIN WITH CODE, paste or type the code (Ctrl+V), press Start/Enter. Then press READY.
The lobby shows the four players and who is ready. When everybody is ready, the host presses START GAME,
picks Normal Game (or Act Select) and a save as usual, and everybody joins in as soon as the game starts.
Back keeps the lobby open; LEAVE LOBBY closes it. Windows may ask whether the game may use the network the
first time somebody hosts: choose Allow.
The F9 window below still works the same way (it is also the way to host on a VPN or the same network).

IN THE LOBBY
  Names:    NAME sets the name the others see (12 letters, plain characters only, default "Player"). It is saved in
            settings.json (section Online, PlayerName). Everybody in the lobby, and the labels below the screens
            during the game, show the names.
  Ping:     each friend's round trip time is shown next to the name (measured by the host, about once a second).
            The host gets a red warning when somebody is above 150 ms.
  Chat:     SAY sends a quick message (Left/Right picks one of 12, the last choice is WRITE A MESSAGE for a short
            text of up to 40 plain characters). One message per 1.2 seconds per player; the host drops the rest.
  Host:     KICK removes a friend (that friend cannot come back for a minute; this is a short block, not a ban).
            INPUT DELAY is chosen by the host, see below, and everybody sees it.
  Start:    if somebody is not ready when the host presses START GAME, everybody who is not ready gets a prompt and
            20 seconds. When the time is up (or the host presses START GAME again) the game starts without them.
            They can press RETRY to join the running game.
  Reconnect: a friend whose connection breaks while waiting in the lobby, or while playing, comes back to the same
            player slot by itself (the host keeps the slot for a minute).

INPUT DELAY (host setting: AUTO or 0 to 6 frames)
  The online game is host-driven lockstep, not rollback: the host runs the game and decides what every player's
  input is on every frame; a friend's game runs the frames it gets from the host. A friend therefore sees its own
  button press after one round trip, while the host sees its own at once. Input delay holds back the inputs of ALL
  players (the host's own too) by the same number of frames before they enter the game. It does not make anybody
  faster; it makes the host's controls feel as late as the friends' and keeps all players on the same footing.
  AUTO picks about one frame per 33 ms of the slowest friend's measured ping above 20 ms (a good connection: 0),
  re-checks every few seconds while playing, and never goes above 6. A fixed number stays as set. 0 means off.

PLAY WITH A CODE (works anywhere, free, no router setup)
--------------------------------------------------------
HOST:   Main menu -> press F9 -> "Host (works anywhere)".
        A code like  K7Q2-9XMAB  appears. Send it to your friend (Copy code button).
        Then start your game as usual (Normal Game, pick a save).

FRIENDS: Each player presses F9, pastes the same code (Ctrl+V), and chooses "Join".
         They arrive as soon as the host has started a game. Press any button
         after the level starts to enter as the next open player slot.

Press F9 again to close the window.

The game server of Sonic 3 A.I.R. is only used to introduce the two of you.
The game itself runs directly between your two computers.


PLAY WITH A VPN OR ON THE SAME NETWORK
--------------------------------------
HOST:   Press F9 -> "Host (VPN or same network)". Pick the address type shown
        (a VPN such as Hamachi, Radmin VPN, ZeroTier or Tailscale keeps your real address private).
        A join code for that address appears (longer than the lobby code, like  ABCDEF-GHJKMN-PQRSTV ).
        It contains your address and a secret, so only a friend who has the code can get in. Send it to your friend.
FRIEND: Paste the join code (Ctrl+V) and press Join. An address typed alone, without the code, does not work.
On a network you do not trust (a hotel, a school), prefer the lobby code or a VPN.
Everybody who joins has to prove that they know the code, and the host has to prove it to them too, so a stranger
on the same network cannot take a player slot, and cannot pose as your host.


IF SOMETHING GOES WRONG
-----------------------
- The lobby (and the F9 window) says in plain words why it stopped, and the lobby offers RETRY:
  the server could not be reached, the code was wrong, the lobby is full, the game version or the mods differ,
  a router or firewall blocks the connection, or the host closed the lobby.
- Both of you: send the file  %APPDATA%\Sonic3AIR\online_log.txt
  (Win+R, paste that path, Enter) and tell us what happened and where in the level.
  The log shows the ping (delay), the speed (fps) and how often the games had to be repaired.
- If the code does not connect, one of the networks probably blocks direct connections:
  use the VPN option.
- Optional relay server (a fallback for connections that cannot be made directly): if somebody runs one
  (see DEPLOY_RELAY.txt; there is no public relay) and the host AND the friends put the same address into
  settings.json (Online -> RelayServer, for example "relay.example.org:28842"), a friend whose direct connection
  has not worked after 8 seconds is connected through the relay instead. The lobby says so and warns about the
  extra lag. It is off by default (the setting is empty) and then nothing changes. It only works with lobby
  codes, not with IP join codes.


UPDATES (Options > System > Update > Check for updates)
--------------------------------------------------------
Everybody who plays together needs the same version. The game can update itself from the releases of its GitHub repository:
  1. Options > System > Update > "Check for updates". The game asks GitHub once for the latest release and shows your version, the
     latest version and the release notes. Nothing is contacted before you press that button, and nothing is downloaded before
     you press "Update now".
  2. "Update now" downloads the package, checks its SHA-256 against the manifest of the release (and against GitHub's own checksum if
     it shows one), and unpacks it into a staging folder. A package with unsafe file names (a file that would land outside the
     game folder), or with saves, settings, ROM files or test files in it, is refused as a whole. You can cancel at any time.
  3. Then the button says "Close the game and install". The game closes, update_helper.ps1 (the one that came with your current
     version, not anything from the download) waits for the game to exit, backs up every file it replaces, copies the new files over,
     installs the co-op mod into %APPDATA%\Sonic3AIR\mods\DropInCoop, and starts the new game. If anything fails it puts every file
     back and starts the old game again. The message is shown under the button ("Last update: ...") and kept in
     %APPDATA%\Sonic3AIR\update\last_result.txt; the log is update\update_log.txt and update\helper_log.txt in the same folder.
  What an update never touches: your saves, save states, settings.json, the mods you installed yourself, your ROM, and
  config.json (kept as it is). Files that are not part of the package stay. One backup (the one before the last update) is kept in
  %APPDATA%\Sonic3AIR\update\backup\ .
  What it does not do: go back to an older version (if the latest release is older than yours, it says so and stops), update
  without your click, or install a package that does not match its checksum.
  Limits worth knowing:
  - The checksum comes from the same GitHub page as the package. It protects against a broken or tampered download, not against
    somebody who controls the GitHub repository (the updates are not signed).
  - The repository is read from settings.json (section Online, UpdateRepo; default TrickerNoxics14/sonic3-coop-mod). GitHub only
    lets friends download release files if the releases are PUBLIC. While the repository is private, the check says that no release
    was found and nobody except the owner can update this way. A separate public repository for the releases only is the usual fix.
  - If the game lies in a folder you cannot write to (for example Program Files) the update says so and does not start. A copy of the
    game without version.txt (a development build) has the updater switched off.
  - GitHub allows only a limited number of anonymous requests per hour from one network. The game says so if you hit it.
  - The stock "Check for updates" of Sonic 3 A.I.R. (which asks sonic3air.org about the STOCK game and offers its download page)
    is switched off in this fork; it would have reported updates for a different program.
  For the person who publishes a release: checks\release.ps1 -Version x.y.z -Notes "..." (see the comments in that script).

NOTES
-----
- Each player has their own rings and score. Getting hit only takes away your own rings.
  Both players can open monitors.
- The game compares a fingerprint of the main game memory (the 64 KB of work RAM) every second and repairs
  the friend's game automatically if they drift apart (F9 window: "repairs so far"). Graphics memory, extra
  memory pages and the variables of the game scripts are not part of that fingerprint, so a difference there
  is only noticed if it also shows up in the main memory. One repair per ten seconds is allowed for each friend.
- The friend's view follows Tails in the regular parts of every zone. In cutscenes, bosses and
  special scrolling parts both of you see the host's view.
- Water colours on the friend's screen can be wrong when only one of you is under water.
- Other mods have to be the same on both computers. Co-op Deluxe is switched off automatically.


TEAMWORK CONTROLS
  These options are under Teamwork in the mod settings (enabled by default).
  RESCUE: hold Down + Y for 2 seconds on your controller to move to your partner.
    Your partner must be alive and standing on ordinary flat terrain. Rescue is
    disabled during bosses and scripted controls, and has a 5-second cooldown.
    Rings, score and lives are preserved. Map Y in the controller settings.
  CHECKPOINTS: either player can activate the shared starpost. A starpost reached
    on ordinary flat terrain also supports independent respawn outside bosses:
    the dead player returns there while the partner keeps playing. Sonic loses
    one life unless infinite lives is enabled; the dead player's rings reset.
    With no safe checkpoint, during bosses, on the last life, or after time over,
    the game's normal death/restart rules apply.
  BOSS REGROUP: when a boss starts, an off-screen Tails is brought to Sonic if
    Sonic is on safe terrain. The shared camera follows the existing boss rules.
  CARRY: while Tails flies, Sonic holds Up beneath him to request a grab.
    Either player presses X to release. Normal flight stamina still applies.
    Map X in the controller settings. Turning Team Moves off prevents new grabs.

ONLINE RECONNECT (custom engine)
  A friend whose transport connection drops automatically retries the existing
  host (up to 40 seconds in the lobby, 90 seconds in a running game), receives a fresh game snapshot, and resumes the
  current session. The host must keep the session open. This cannot reconnect to
  a host that closed the game or guarantee recovery after its network address
  changes. Local script-only installations do not include the reconnect engine.


THREE / FOUR PLAYER PARTY MODE (experimental, v0.2.0)
  In Mods > Drop-In Co-op > Teamwork, set Maximum Players to 3 or 4.
  Select Sonic & Tails. P1 is Sonic; P2 is Tails; P3 is Knuckles;
  P4 is a second Sonic, identified by the P4 label.
  P2, P3 and P4 press Start on their own controller to join. Extra players
  join during ordinary active gameplay; scripted controls keep them waiting.
  P3/P4 hold Down + Mode for two seconds to leave. Down + Y requests rescue.
  The extended local engine adds optional four-view split screen. Enable
  Visuals > Four-Player Quadrant Split Screen. Bosses use one shared screen;
  water, unsupported scrolling and camera locks also fall back to one view.
  Stock game builds use one shared camera for three/four players.
  Players who fall behind regroup at a grounded Sonic; extra-player deaths
  respawn near him. Extra players keep their join request across act changes.
  Each player has their own rings and score. Turning Maximum Players back to 2
  removes the extra players and makes two-player split screen available again.
  For online play, every friend uses this updated build and presses Host/Join
  as usual. Friends are assigned separate slots 2, 3 and 4. The host must set
  Maximum Players to 4 before starting a four-player game.
  Four-view mode requires the extended engine and does not work with the
  official stock executable. It remains experimental: full routes, bosses,
  scripted transports and super/shield combinations are not fully audited.
  The new network packet format requires every participant to update the engine.

LICENSE AND SOURCE
  Sonic 3 A.I.R. is free software under the GNU GPL v3 (see LICENSE.txt in this
  folder). This build is the official v26.03.28.0 stable source plus online-play
  changes. The unmodified upstream source is at
  https://github.com/Eukaryot/sonic3air . The source of the modified engine
  must be made available with this download by whoever distributes it.
  The game ROM and your saves are not included and never should be.
