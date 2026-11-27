==============================================================================
  SONIC 3 AIRNET  -  Online co-op for Sonic 3 A.I.R.
==============================================================================

Up to four players on separate computers play one game of Sonic 3 & Knuckles
together. Every player has their own full screen, their own controller and
their own character. Windows players and browser players share the same
lobbies.

  Play in the browser:   https://trickernoxics14.github.io/Sonic-3-Airnet/
  Latest Windows build:  https://github.com/TrickerNoxics14/Sonic-3-Airnet/releases/latest

CONTENTS
   1. Getting started
   2. Playing online
   3. Playing with browser players
   4. In the lobby
   5. Choosing your character
   6. Blue Sphere special stages with several players
   7. Other ways to connect (F9 window)
   8. Updates
   9. Troubleshooting
  10. Teamwork controls and party mode (Drop-In Co-op)
  11. What has been tested
  12. License and source


------------------------------------------------------------------------------
 1. GETTING STARTED
------------------------------------------------------------------------------

Once, on every computer:

  1. Put your game ROM "Sonic_Knuckles_wSonic3.bin" into  %APPDATA%\Sonic3AIR\
     (press Win+R, type  %APPDATA%\Sonic3AIR , press Enter).
     The ROM is NOT included. Every player supplies their own legally
     obtained copy.
  2. Keep this folder together and start Sonic3AIR.exe from inside it.
  3. Everybody who plays together needs the same version of this folder.

The co-op mod is built in; there is nothing else to install. On the first
start the game copies it into  %APPDATA%\Sonic3AIR\mods  and switches it on,
and at every start it repairs that copy so that it matches this folder. If you
switch it off in the Mods menu, it stays off.

The remastered soundtrack of Sonic 3 A.I.R. is included (data\audioremaster.pak);
Options > Audio switches between it and the original music.

This build is based on the official stable Sonic 3 A.I.R. 26.03.28.0.


------------------------------------------------------------------------------
 2. PLAYING ONLINE
------------------------------------------------------------------------------

Main menu -> ONLINE. The first screen offers:

  FIND A LOBBY    QUICK JOIN at the top joins the best open lobby for your
                  version (the fullest one that is still waiting for players).
                  Below it is the list of open lobbies (name, host, players,
                  open / playing / full, version). Choose one and press A.
                  Lobbies of another game version or with other mods are
                  greyed out. REFRESH and the SHOW filter (all lobbies, or
                  only those that fit your version) are below the list; it
                  also refreshes itself.
  CREATE LOBBY    NAME, LOBBY (PUBLIC = listed, PRIVATE = joined only with its
                  code), PLAYERS (2 to 4), then OPEN LOBBY.
  JOIN WITH CODE  Numbers only: 6 digits = a lobby code, 13 digits = a long
                  code. Type the digits, paste with Ctrl+V, or use the
                  on-screen number pad (D-pad and A, B deletes a digit, Start
                  joins). Spaces and dashes in a pasted code are ignored.
  LAN GAME        Play on the same network without any server: HOST A LAN
                  GAME shows a long code, JOIN A LAN GAME takes it.
  SETTINGS        Your NAME, the LOBBY SERVER and the RELAY SERVER.

In the lobby the code is shown at the top right (OPTIONS > COPY CODE puts it
on the clipboard). The players are listed on the right with their character,
ping and READY state, the chat below them. Friends press READY, the host
presses START GAME and picks Normal Game (or Act Select) and a save as usual;
everybody joins as soon as the game starts.
Back keeps the lobby open, LEAVE LOBBY closes it. The first time you host,
Windows may ask whether the game may use the network: choose Allow.

THE LOBBY SERVER
  Public lobbies, Quick Join and 6-digit codes come from a lobby server: a
  small relay program that keeps the list, hands out the codes and forwards
  game data between players who cannot reach each other directly.
  This build uses a free public one by default
  (wss://sonic3-airnet-relay.onrender.com), so online play works without any
  settings. It runs on a free plan that would go to sleep after 15 minutes
  without players; a scheduled job keeps it awake. If it ever does sleep,
  the first player sees "Waking the server up" and may wait up to a minute.
  You can use another server under SETTINGS > LOBBY SERVER (or in
  settings.json, section Online, LobbyServer). An address that starts with
  wss:// or ws:// is reached by a WebSocket, like the browser version does;
  any other address (name or name:port) by UDP. The lobby server is also used
  as the relay unless a separate RELAY SERVER is set. How to run your own:
  checks\relay_ws\HOSTING.md in the source repository.

THE CODES
  6-digit lobby code   From the lobby server, for public and private lobbies.
                       Wrong guesses are limited, so a lobby cannot be found
                       by guessing.
  13-digit long code   12 digits and a check digit (for example
                       4827-1693-50728): a typing mistake or two swapped
                       digits is noticed at once. It works through the
                       matchmaking server and on the same network.
  Codes of older versions (letters and digits) can no longer be typed in;
  ask the host to update.


------------------------------------------------------------------------------
 3. PLAYING WITH BROWSER PLAYERS
------------------------------------------------------------------------------

The web version (link at the top) runs in a desktop browser and joins the same
public lobbies through the same server. A browser player can join your lobby
and you can join theirs: Quick Join, the lobby list and 6-digit codes all work
across both.

  - Turn mods off on Windows: the browser version has none, and only games
    with the same version and mods can play together.
  - Games between Windows and browser players always run through the relay
    server, which adds a little delay compared with a direct connection.
  - The browser keeps its ROM and saves in that browser only; nothing is
    uploaded.


------------------------------------------------------------------------------
 4. IN THE LOBBY
------------------------------------------------------------------------------

  CHARACTER   Left/Right picks your character. ANY means the character is
              chosen in the game's own menu.
  OPTIONS     Input delay (host), KICK (host, only when a friend is there),
              your NAME and COPY CODE (host).
  NAME        The name the others see (12 plain characters, default
              "Player"), saved in settings.json (Online, PlayerName). It is
              shown in the lobby and on the labels below the screens.
  PING        Each friend's round-trip time is shown next to their name
              (measured by the host about once a second). The host gets a red
              warning when somebody is above 150 ms.
  CHAT        Sends a quick message: Left/Right picks one of 12, the last
              choice, "Write a message...", takes a short text (up to 40 plain
              characters). One message per 1.2 seconds per player.
  KICK        (Host, in OPTIONS) Removes a friend, who cannot come back for
              a minute. This is a short block, not a ban.
  START       If somebody is not ready when the host presses START GAME,
              they get a prompt and 20 seconds. Then the game starts without
              them (or at once if the host presses START GAME again); they can
              press RETRY to join the running game.
  RECONNECT   A friend whose connection breaks, in the lobby or in the game,
              returns to the same player slot by itself. The host keeps the
              slot for a minute.

INPUT DELAY (host setting: AUTO, or 0 to 6 frames)
  Online play is host-driven lockstep, not rollback: the host runs the game
  and decides every player's input for every frame, and the friends' games
  run the frames they receive. A friend therefore sees their own button press
  one round trip later, while the host sees theirs at once.
  Input delay holds back the inputs of ALL players, the host included, by the
  same number of frames. It does not make anybody faster; it makes the host's
  controls feel like the friends' and puts everybody on the same footing.
  AUTO picks about one frame per 33 ms of the slowest friend's ping above
  20 ms (a good connection gets 0), checks again every few seconds while
  playing, and never goes above 6. A fixed number stays as set; 0 means off.


------------------------------------------------------------------------------
 5. CHOOSING YOUR CHARACTER
------------------------------------------------------------------------------

Every player, the host too, has a CHARACTER entry in the lobby: Left/Right (or
the accept button) steps through the characters you can have, and everybody
sees "<number> <name> - <character>" for every player. You can change it as
often as you like while the lobby is open. The host checks every choice: a
choice that is not possible is refused and you are told what you play instead.
When the game starts, the choices are fixed for that whole game (a friend who
reconnects keeps their slot and character; somebody who joins a running game
gets the character of that slot). If nobody touches the entry, the classic
roles apply: the host plays what the game menu says (Sonic with Tails), player
2 is Tails, player 3 is Knuckles and player 4 is a second Sonic (or the
extra-slot character set in the mod's "Player 3/4 Extra Character Slot").

  Host (player 1)   GAME MENU (the default: what the save slot / game menu
                    says), SONIC or KNUCKLES, each with Tails as the game's
                    second character. Tails alone is not offered for the host,
                    because the game would then have no body for player 2. For
                    an extra-slot character, pick it in the game's own menu
                    (if that mod supports it) and choose GAME MENU.
  Player 2          TAILS (default), SONIC, KNUCKLES or an extra character.
                    For anything but Tails, the Tails body becomes the picked
                    character once player 2 has joined and normal gameplay is
                    running (not during the Angel Island 1 intro or a
                    cutscene); until then player 2 plays Tails.
  Players 3 and 4   KNUCKLES or SONIC (the classic roles) or an extra
                    character. Tails exists once in the game, so only player 2
                    can be Tails. Two players may pick the same character.

EXTRA CHARACTERS
  With the extra-slot character mods installed (Extra Slots Unlimited and the
  characters made for it), their characters appear for players 2, 3 and 4 as
  "EXTRA <slot>" (the well-known slots 35 and 90 by name). Everybody must have
  the same mods; the join check refuses friends with different ones. The list
  follows mods that are switched on or off while the lobby is open. If a
  picked extra character is missing on the host when the game starts, that
  player gets the default of the slot and the lobby says so. A friend whose
  game cannot find the host's extra character refuses the start with the
  "different mods" message (and can press Retry) rather than play a different
  character than everybody else.

SAVING
  If the host picks a different character than the one the save slot was made
  for, that slot is not written to (a Sonic save never gets Knuckles'
  progress). Play without a save slot, or with a slot of that character.

LIMITS
  Tested by automated checks with a small test mod as the extra character. The
  real extra-slot character mods were not tested: not every one works for
  players 2 to 4 (see DropInCoop\README.txt). With three or four players the
  game uses one shared camera; with two players each keeps a full screen.
  Not tested: player 2 changing character during the Angel Island 1 intro or
  in boss fights.


------------------------------------------------------------------------------
 6. BLUE SPHERE SPECIAL STAGES WITH SEVERAL PLAYERS
------------------------------------------------------------------------------

When an online game with at least two players enters a Blue Sphere special
stage, EVERYBODY runs it AT THE SAME TIME, each on their own copy of the same
layout: a fair race. A small list in the top-left corner shows every player
(name, RUN / DONE / OUT / TIME, blue spheres left, rings collected).

  - Touch a red sphere and you are OUT: you watch the players who are still
    running (Left/Right switch between them). Players who complete the stage
    also watch until everybody is done.
  - The stage ends when nobody is running. It counts as CLEARED (you get the
    Chaos Emerald, once) if at least one player completed it. The host's
    normal results, continues (50 rings) and extra lives (100 / 200 rings)
    apply; friends get the ring bonus (10 points per ring, 5000 for a perfect)
    as score.
  - Who takes part: the host, and every friend who had joined the level
    (player 2 with any button, players 3 and 4 with Start). A connected friend
    who never joined just watches.
  - A runner who presses no button for 30 seconds is timed out (TIME). The
    others may go on for 30 seconds after the first player completes the
    stage, and every stage ends after 7 minutes at the latest.

  Limits: every runner is drawn as the host's main character; only the host
  touches the giant ring and can pause; the other bonus stages (Slot Machine,
  Glowing Spheres, Gumball) and the Hidden Palace emerald cave are unchanged;
  a friend who joins during a stage watches. The timings are constants at the
  top of level\special\defines_bluespheres.lemon.


------------------------------------------------------------------------------
 7. OTHER WAYS TO CONNECT (F9 WINDOW)
------------------------------------------------------------------------------

The ONLINE menu covers everything most players need. The F9 window (press F9
in the main menu) offers two more ways:

PLAY WITH A CODE (works anywhere, no router setup)
  Host:     F9 -> "Host (works anywhere)". A long code such as 4827-1693-50728
            appears; send it to your friends (Copy code). Then start your game
            as usual.
  Friends:  F9, paste the code (Ctrl+V), choose "Join". They arrive as soon as
            the host has started a game; press any button in the level to take
            the next open player slot.
  The Sonic 3 A.I.R. game server only introduces you to each other; the game
  then runs directly between your computers. Press F9 again to close the window.

PLAY WITH A VPN OR ON THE SAME NETWORK
  Host:     F9 -> "Host (VPN or same network)" and pick the address type shown.
            A VPN such as Hamachi, Radmin VPN, ZeroTier or Tailscale keeps your
            real address private. A join code appears (letters and digits,
            like ABCDEF-GHJKMN-PQRSTV; it is only for the F9 window, not for
            the ONLINE menu). It contains your address and a secret.
  Friend:   paste the join code (Ctrl+V) and press Join.
  Everybody who joins must prove that they know the code, and the host must
  prove it to them, so a stranger on the same network can neither take a slot
  nor pose as your host. On a network you do not trust (hotel, school), prefer
  the lobby code or a VPN.


------------------------------------------------------------------------------
 8. UPDATES  (Options > System > Update > Check for updates)
------------------------------------------------------------------------------

Everybody who plays together needs the same version. The game can update
itself from the releases of its GitHub repository:

  1. "Check for updates" asks GitHub once for the latest release and shows
     your version, the latest version and its release notes. Nothing is
     contacted before you press the button, and nothing is downloaded before
     you press "Update now".
  2. "Update now" downloads the package, checks its SHA-256 checksum, and
     unpacks it into a staging folder. A package with unsafe file names, or
     with saves, settings, ROM files or test files in it, is refused. You can
     cancel at any time.
  3. "Close the game and install" closes the game; the update helper that came
     with your CURRENT version backs up every file it replaces, installs the
     new files and the co-op mod, and starts the new game. If anything fails,
     it restores every file and starts the old version. The result is shown
     under the button and kept in  %APPDATA%\Sonic3AIR\update\ .

An update never touches your saves, save states, settings.json, your own mods,
your ROM or config.json. One backup (from before the last update) is kept in
%APPDATA%\Sonic3AIR\update\backup\ . It never installs an older version, never
updates without your click, and never installs a package whose checksum does
not match.

Good to know:
  - The checksum comes from the same release page as the package. It protects
    against broken or tampered downloads, not against somebody who controls
    the repository (updates are not signed).
  - The repository is set in settings.json (Online, UpdateRepo; default
    TrickerNoxics14/Sonic-3-Airnet).
  - A game installed in a folder you cannot write to (such as Program Files)
    cannot update itself; move it to a folder of your own.
  - GitHub limits anonymous requests per hour per network; the game says so
    if you hit the limit.
  - The stock Sonic 3 A.I.R. update check (sonic3air.org) is switched off in
    this build, because it would report updates for a different program.


------------------------------------------------------------------------------
 9. TROUBLESHOOTING
------------------------------------------------------------------------------

  - The lobby and the F9 window say in plain words why something stopped, and
    the lobby offers RETRY: the server could not be reached, the code was
    wrong, the lobby is full, the version or mods differ, a router or firewall
    blocks the connection, or the host closed the lobby.
  - "Waking the server up": the free server was asleep. Wait up to a minute,
    or press the button again.
  - A long code that does not connect: one of the networks probably blocks
    direct connections. Use a lobby (it falls back to the relay server) or
    the VPN option.
  - To report a problem, every player sends the file
        %APPDATA%\Sonic3AIR\online_log.txt
    (Win+R, paste the path, Enter) and describes what happened and where in
    the level. The log shows the ping, the frame rate and how often the games
    had to be repaired.


------------------------------------------------------------------------------
 10. TEAMWORK CONTROLS AND PARTY MODE (DROP-IN CO-OP)
------------------------------------------------------------------------------

These are features of the built-in Drop-In Co-op mod; the options are under
Mods > Drop-In Co-op > Teamwork (on by default).

  RESCUE       Hold Down + Y for 2 seconds to move to your partner. Your
               partner must be alive and on ordinary flat ground. Not during
               bosses or scripted controls; 5-second cooldown. Rings, score
               and lives are kept. Map Y in the controller settings.
  CHECKPOINTS  Either player can activate the shared starpost. A starpost on
               ordinary flat ground also lets a dead player respawn there
               while the partner keeps playing (outside bosses). Sonic loses
               one life unless infinite lives is on; the dead player's rings
               reset. Without a safe checkpoint, in bosses, on the last life or
               after time over, the game's normal rules apply.
  BOSS REGROUP When a boss starts, an off-screen Tails is brought to Sonic if
               Sonic is on safe ground.
  CARRY        While Tails flies, Sonic holds Up beneath him to be grabbed.
               Either player presses X to let go. Flight stamina still
               applies. Map X in the controller settings.

  Every player has their own rings and score; getting hit only costs your own
  rings. Everybody can open monitors.

THREE / FOUR PLAYER PARTY MODE (experimental)
  Set Teamwork > Maximum Players to 3 or 4 and select Sonic & Tails. Player 1
  is Sonic, 2 is Tails, 3 is Knuckles, 4 is a second Sonic (with a P4 label).
  Players 2, 3 and 4 press Start on their controller to join during normal
  gameplay; players 3 and 4 hold Down + Mode for two seconds to leave.
  Players who fall behind regroup at Sonic; extra players respawn near him.
  For online play, the host sets Maximum Players to 4 before starting a
  four-player game; friends get slots 2, 3 and 4. This online build uses one
  shared camera for three or four players (the four-view split screen of the
  local mod is not part of it). Full routes, bosses, scripted transports and
  super/shield combinations are not fully checked yet.

ONLINE RECONNECT
  A friend whose connection drops retries the same host automatically (up to
  40 seconds in the lobby, 90 seconds in a running game), receives a fresh
  copy of the game and carries on. The host must keep the session open; a host
  who closed the game cannot be reconnected to.

NOTES
  - The friend's view follows Tails in the regular parts of every zone; in
    cutscenes, bosses and special scrolling sections everybody sees the host's
    view.
  - Water colours on a friend's screen can be wrong when only one of you is
    under water.
  - Other mods must be the same on every computer. Co-op Deluxe is switched
    off automatically.
  - Every second the game compares a fingerprint of the main game memory (the
    64 KB work RAM) and repairs a friend's game automatically if they drift
    apart (at most once per ten seconds per friend). Graphics memory, extra
    memory pages and script variables are not part of that fingerprint.


------------------------------------------------------------------------------
 11. WHAT HAS BEEN TESTED
------------------------------------------------------------------------------

Online play is checked by automated tests: two to four game windows and
browsers on one computer, a lag simulator (200 ms round trip, jitter and 3%
packet loss), Windows and browser players in one lobby, and the free public
server used by this build (a Windows game hosting and a browser player joining
and playing in sync). Not yet proven: real games between two different home
networks through every kind of router and firewall, the browsers Firefox and
Safari, and how the input delay settings feel to a human player. The sync
check only watches the 64 KB work RAM.


------------------------------------------------------------------------------
 12. LICENSE AND SOURCE
------------------------------------------------------------------------------

Sonic 3 A.I.R. is free software under the GNU GPL v3 (see LICENSE.txt in this
folder). This build is the official v26.03.28.0 stable source plus the online
play changes; its full source is at
https://github.com/TrickerNoxics14/Sonic-3-Airnet , and the unmodified
upstream source at https://github.com/Eukaryot/sonic3air . Whoever distributes
this build must make its source available with it.

Sonic 3 AirNet is a non-profit fan project, not affiliated with or endorsed by
SEGA, Sonic Team or the Sonic 3 A.I.R. team. Sonic the Hedgehog is a trademark
of SEGA. The game ROM and your saves are not included, and never should be.
