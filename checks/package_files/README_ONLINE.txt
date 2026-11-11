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
  host for up to 90 seconds, receives a fresh game snapshot, and resumes the
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
