# Critical checks for audit

Run the local co-op audit with one command (close Sonic 3 A.I.R. first):

    checks\run_checks.bat

Options:

- `-Build`: compile the engine first (a few minutes), then package and check.
- `-Package`: rebuild the release package (`Sonic3Airnet.zip`) from the current build and scripts, then check.
- `-Quick`: only the static checks (A, B) and the engine self test (C). No game windows.
- `-Only D`, `-Only E F` and so on: only some of the game stages.
- `-Only Z`: run the AIZ zip-line P2-view regression with a framebuffer capture.

The result is printed and saved to `checks\last_report.txt`. The logs of the last game runs are kept in `checks\last_logs`.
The run passes only if every check is PASS. The run puts your mod list, config and settings back afterwards.

Run the stable online-fork audit with one command:

    powershell -ExecutionPolicy Bypass -File checks\run_online_backend_checks.ps1

This rebuilds the stable 26.03.28.0 fork, checks the online ZIP contents, then runs two-player joining, the matchmaking code path, lag/loss handling, four-player play and reconnect. It writes `checks\online_backend_report.txt` and `checks\online_stable_runtime_report.txt`.

Every listed executable check is automated. The matchmaking test uses the public rendezvous service but joins from this same computer; it does not prove connectivity across different home networks or NATs.

The runner takes an exclusive Windows mutex before modifying game configuration or logs. A second run fails before touching either.

## A. Build and package

| ID | Check | What proves it |
|----|-------|----------------|
| A1 | Engine is built from the current C++ source | Build log says `exit=0`, and the exe is newer than every `.cpp`/`.h` file |
| A2 | Release package exists and can be opened | The zip opens and lists its files |
| A3 | Packaged engine is the same file as the build | SHA-256 of both exe files is equal |
| A4 | Packaged game scripts are the current scripts | Every script file hashes the same as the source |
| A5 | Packaged co-op mod is the current DropInCoop | Every mod file hashes the same as the source |
| A6 | No saves, script caches or save states in the package | No such entries in the zip (a stale cache caused the "Failed to load 'scripts.bin'" window) |
| A7 | No game ROM in the package | No ROM-like names, no large `.bin` files |
| A8 | Packaged config starts the normal way | `StartPhase` is 2, no test start lines are active |
| A9 | Readme explains the join code and the log file | Text check of `README_ONLINE.txt` |
| A10 | No developer test mods in the package | No harness or test folder names in the zip |

## B. Scripts (static)

| ID | Check | What proves it |
|----|-------|----------------|
| B1 | Co-op scripts have balanced brackets | Parentheses and braces match in every `.lemon` file (strings and comments ignored) |
| B2 | Co-op scripts contain no test hooks | No `test.frame`, `S3AIR_` or `Checks.report` in the shipped mod |

## C. Engine self test (join codes)

Runs the real engine code (`NetplayCodes.h`), not a copy. Checks that codes round-trip, that wrong lengths and invalid characters are refused, and that single-character typing mistakes are noticed (less than 0.5 percent may slip through).

## D. Online play, joined by the code

Two game windows on this computer, joined only by the code over the same network path. Each window writes its own log file.

| ID | Check | What proves it |
|----|-------|----------------|
| D1 | Friend joins with the code over the same network | Friend log says it found the host by the code, the game state arrives, no abort |
| D2 | Online log is written by both host and friend | `online_log.txt` has both kinds of lines |
| D3 | Game stays in sync during 15 seconds of play | No repairs were needed |
| D4 | A wrong or unknown code ends with a message | Friend log says "giving up" (nobody is hosting with this code) instead of hanging |

## M. Public matchmaking server

`-Only M` registers a temporary host session with the game's public matchmaking service, then launches a second game process using the same session code. It checks registration and whether the second process receives the game state. This uses the public matchmaking service but runs both peers on one computer; it cannot prove that two different NATs or home networks can connect.

## E. Online play with a bad connection

250 ms delay and 5 percent packet loss, both directions.

| ID | Check | What proves it |
|----|-------|----------------|
| E1 | Connection and game survive a bad connection | Friend is in the game, no abort |
| E2 | At least 30 frames per second with the bad connection | Lowest friend fps while in the game is 30 or more |
| E3 | No repairs needed with the bad connection | Repair count stays 0 |

## F. Split screen and game scripts (one game window, Angel Island 1)

| ID | Check | What proves it |
|----|-------|----------------|
| F1 | Co-op scripts load in the game without errors | The game reaches the check run; no compile or load error in the log |
| F2 | Player 2 joins at the start of Angel Island before the camera scrolls | Joined, and Tails is no longer inactive (the join works during the level intro) |
| F3 | Player 2 has his own view from the start of Angel Island | The screen is split while the camera is still in the intro, and Tails is alive under normal control |
| F4 | Score is kept separately for each player | Points for player 2 change only his score |
| F5 | Getting hurt only costs the rings of the player who got hurt | Player 2 drops to 0, player 1 keeps his rings |
| F6 | The off screen indicator for player 2 is hidden while he is playing | The game reports the indicator as hidden while joined |
| F7 | Empty tile updates leave the queue unchanged | The actual row and column writers preserve the pointer and sentinel when buffer clipping yields zero tiles, and for explicit zero counts |
| F18 | Player 2 opens a monitor collision | The player-two blocking hook permits a rolling P2, the collision hook breaks the monitor, and the effect owner is P2; uses an isolated monitor fixture, not a monitor placed in a live level |

## G. Roaming coverage

`-Only G` runs 26 scenes: 23 main acts, Hidden Palace, and the separate Lava Reef and Death Egg boss arenas.
`-Only G04,G11` selects specific scenes. Roaming scenes require player 2 joined, more than 60 split frames, both players alive, and no script or VRAM error.
The navigation bot jumps over enemies and steps and charges a spindash when blocked by a breakable barrier. Damage and death are enabled.
IceCap 1 runs for 2200 frames to include snowboarding, the avalanche, and entry into indoor roaming. These opening sequences remain shared-screen.
Hydrocity 2 starts beyond the wall chase through normal level initialization, so G04 covers roaming, not completion of the wall chase.
Screenshots are captured when logs first confirm split screen, rather than at a fixed frame during a possible intro.
`GxxB` and `GxxR` exercise encounter entry and return from an already-confirmed roaming scene. World object updates are held fixed during this camera-only phase while camera/render updates continue; this avoids confusing a new story cutscene with a failed camera return.
`GxxP` requires an image saved directly from the engine framebuffer as a BMP. Desktop PNG captures may contain other windows and are secondary evidence.

## H. Boss scene coverage

`-Only H` runs isolated boss scenes. `build_boss_fixtures.py` derives approach positions from the checked-in level object tables.
The scene harness creates real boss objects and initializes the camera region from the boss's own table. It does not set `level.boss_encounter` to make the verdict pass. Special scenes use their original progression trigger; Lava Reef's separate boss act runs its real scrolling trigger, which creates the boss object.
All four players enter through the production party allocator. After the boss starts, the test fixture holds all four actors in the shared camera so the test isolates boss-scene rendering from the route and arena physics. It clears artificial hit-flash timers and captures only when all four are alive, inside the camera, and have visible render flags. A pass requires the original boss encounter to start, at least 60 active encounter frames, at least 30 consecutive frames with all four actors active, shared-screen throughout the encounter, and a saved engine framebuffer. This proves the shared boss view and visible party after arrival; it does not prove the full route or that the boss can be defeated.
Missing fixtures, bosses that never start, and script assertions are failures. These scenes do not prove the full route from act start or completion of the fight.

## V. Teleport stress diagnostic

`-Only V` retains the original mid-act IceCap teleport reproduction without the co-op mod. This deliberately skips level transition prerequisites; its crash alone does not establish a bug reachable in normal play.

## Z. Angel Island zip-line in player two's view

`-Only Z` starts Angel Island 1, joins player two, and places Tails once at the authored zip-line start while Sonic remains in the other view. The harness then allows the real rope and P2 camera to move normally. Z1 requires a live zip-line segment to be visible in P2's camera bounds and outside P1's; Z1V captures the framebuffer. This covers the reported AIZ rope case, not every multi-camera object.

## Not fully verified yet

These are coverage gaps. Please don't count them as passed.

- Two different home networks over the internet (the code path only checked on one computer).
- A real friend joining from a different home network through the game's public matchmaking service.
- Other objects that stream or render differently across two camera views; the AIZ zip line is covered by Z.
- Camera and graphics in every zone with both players, and the split screen in water.
- A monitor placed in ordinary level terrain and opened through normal movement by player 2; F18 exercises the real hooks with an isolated fixture.
- The split-return VRAM regression has a concrete reproduction in G01. Empty clipped row/column updates encoded `0xffff`; F7 covers that case. AIZ intro and Hidden Palace second-view scrolling also queued first-view tile writes; those calls were removed. The original player's exact route is unknown, so this establishes a fixed class rather than proving every possible VRAM crash is gone.
- Complete routes from act start into every boss, including scripted airship and wall-chase sequences. Boss scenes are isolated; they prove arena behavior after arrival, not the route to it.
- Rendering correctness across all camera positions and transitions; screenshots of a few scenes provide only sampled evidence.


## Teamwork regressions and reconnect

F8 tests the 120-frame rescue hold, cancellation, cooldown, both players, and preserved ring counters. F9 rejects airborne/platform anchors, scripted controls, and rescue input during bosses. F10 saves the second player's checkpoint camera/layer and verifies restoration of the primary camera. F11 exercises the real death-boundary hook for both players and rejects a checkpoint from another zone. F12 tests regrouping an off-screen partner at encounter entry without repeatedly teleporting them. F13 exercises the original carry collision with opt-in input and release.

These are isolated state and interaction checks; they do not prove collision safety at every point in every zone.

`-Only R` disconnects the friend's real transport after the initial snapshot. R1 requires a second snapshot and confirmation that the existing session resumed. R2 requires advancing frames and zero synchronization repairs. This runs on one computer; different internet networks and changed NAT mappings remain unverified.


## P / T. Three and four player regressions

`-Only P` runs both modes. `-Only P3` (also `-Only T`) runs three; `-Only P4` runs four. Each scene uses original terrain/character physics and independently scripted controller inputs. It requires the correct actors, isolated movement, jumping, shared camera, framebuffer capture, all players alive, separate platform standing pointers, score ownership, ring loss limited to the hurt player, and death/respawn without restarting Sonic's game.

The previous zero-Knuckles capability report is historical; party mode now allocates real Knuckles and fourth-player objects. A picture does not establish correctness in every level.

## Q / QG. Four-view split coverage

`-Only Q` checks all four quadrants from the engine framebuffer, readable character spacing, visible level pixels in each quadrant, and distinct line-scroll data for every view. `-Only QG` checks the party across all 24 roaming acts. Supported scenes must show all four active players with valid cameras for at least 60 consecutive frames; unsupported or water scenes must keep the party on one shared screen. `-Only H` checks real bosses separately and requires one shared screen with all four players alive. `QGxxV` inspects the captured image only when the run proves that all four views were active.

Four-view rendering requires the extended engine build. The official stock executable does not expose the extra scroll-offset sets. Water scenes fall back to one shared view because one global palette interrupt cannot provide four independent waterlines. A camera lock or scripted sequence can also temporarily switch back to one screen.

## N. Four real netplay peers

`-Only N` starts one host and three clients. Each client sends a different input through the real network packets and must receive player slot 2, 3, or 4. The host must observe all four slots in the running simulation. Player three is disconnected and reconnects while player four keeps slot four. Script errors and sync repairs fail the check. This is a local-network test on one computer, not an internet/NAT guarantee.

## Party mode coverage limits

Three/four-player mode remains experimental. The extended engine supports four-way views in validated scrolling scenes; stock builds and unsupported scene segments use a shared camera with grounded-leader regrouping. Ordinary terrain movement, jumping, representative solid-platform contacts, counters, damage, respawn and sampled four-quadrant images are checked. Full routes, every boss/special sequence with all four players, every interactable, and all advanced ability/shield/super combinations remain unverified.


## Netplay hardening (2026-11-11)

Offline core tests (checksun_online_core_tests.ps1, part of run_online_backend_checks.ps1): join code with secret (CRC-12, every single typo noticed), session code with check character, challenge-response (right and wrong secret, replay, role swap, LAN tag), failure reason messages, absurd blob size, zip bomb, two connection managers over loopback (spoofed end-of-connection ignored, junk LAN packets, control case), test hooks off without marker.

| Stage | What it proves |
| --- | --- |
| S1-S3 | Host with a secret: a friend with a wrong secret is refused and told WRONG_CODE, gets no slot or game; the right secret gets in and both sides log the proof |
| X1-X10 | Reasons: server unreachable (friend and host), wrong code, blocked/timeout, version, mods, lobby full, Retry gets in after a slot became free, host closed (graceful and killed) |
| U4/U5 | The unzipped package ignores S3AIR_* variables; with the marker file next to it the same variables work |
| A12 | No testhooks.enabled in the zip |

Limits: all on one computer (loopback/LAN fallback); the Retry button press itself is simulated by a test switch; a stranger who skips the handshake is covered by unit tests of the proof, not by a raw-packet runtime test.

## Lobby features and bad internet (2026-11-11, stage B)

Offline core tests (23 new lines starting with "lobby:"): names cleaned (default Player, markup, control characters, non-ASCII and 12 letters), chat cleaning and 40 character limit, the 12 quick messages, chat rate limit, ping smoothing and the 150 ms warning, Auto input delay curve and cycling, the input delay line (N frames exactly, all four players alike, changing the delay only skips or repeats one frame), slot reservation, kick block, lobby packets (round trip, over-long names and chat refused).

| Stage | What it proves |
| --- | --- |
| L6-L14 | Host and two friends (one computer): names sent and cleaned (a name full of forbidden characters becomes BadNameXY 12), ping measured per friend and known to the friend, chat (a quick message and a free text with markup reach everybody cleaned up; a flood of 6 lines from one player is dropped except one), input delay set by the host in the lobby seen by everybody, a friend whose connection drops silently comes back to the same slot with its name, the host kicks a player (told, no Retry, a retry is refused), a ready check that ends without the unready friend (prompt shown, left out, can press Retry and is back) |
| W1-W3 | Lag proxy (checks\udp_lag_proxy.ps1: 200 ms round trip, 30 ms jitter each way, 3 percent loss, datagrams may overtake each other) between friend and host: joins, the traffic went through the proxy with the set delay and loss, no repairs and the sync check keeps passing, the friend keeps at least 30 fps (average 45 or more), the host measures about 200 ms and warns, the input delay in use is the one chosen (off, Auto, fixed 4), the default name is Player, and each change of the friend's button reaches the game exactly N frames after it reached the host (measured from logs) |
| W4 | The same proxy with both lobby screens open: the host sees the friend's ping (red) and the warning, the friend sees its own ping and the input delay that Auto chose (4 to 6 frames) |

Limits: everything on one computer. The proxy imitates delay, jitter, loss and reordering on loopback; it does not imitate NAT, bandwidth limits, a congested line or a path that changes. The ping is the round trip including both games' frame timing (about 20 ms even on one computer). The frames-per-second figure is the friend's simulated frames per second, which says the game keeps running, not how the controls feel. How the input delay feels to a person was not tested (nobody played). The player name is a setting in settings.json edited from the lobby; there is no entry for it in the Options menu. Kick is a short block by the player's random per-run identity, not a ban.

## Relay fallback (2026-11-11, stage C, code only, never deployed)

Offline core tests (about 30 new lines starting with "relay"): the wire format and the made-up peer addresses; the relay logic with forged datagrams and a fake clock (forwarding both ways, two friends kept apart, repeated hello keeps the token, wrong keys get no answer, wrong-key registration cannot take a lobby, forged tokens / forged host address / missing friend / garbage / oversized / truncated datagrams dropped, 8 wrong keys block an address for a minute, floods capped by packets and by bytes while others still get through, hello flood answered only a few times, caps on lobbies / friends / remembered addresses, no answer larger than its request, expiry of silent hosts and friends); and the game's own UDP socket through a real relay on the loopback (host and friend let in, two connection managers connect and exchange packets through it, a forged "relay" datagram from another address is not trusted, a wrong key never gets in). A mutation (turning the key check off) makes three of these fail.

| Stage | What it proves |
| --- | --- |
| Y1 | No relay set (the default): the friend's blocked direct connection (a proxy that drops everything) fails with the usual timeout message and no relay is mentioned |
| Y2 | Same setup with the same relay set on both sides: the friend tries directly, switches to the relay after 8 seconds, is let in, both sides prove the code, the friend gets into the running game, the proxy forwarded 0 datagrams (all traffic went through the relay), the relay forwarded the game, no repairs, the sync keeps advancing, about 60 fps, the online logs say relay=1 / relayed=1, the relay program printed no addresses or codes |
| Y3 | The lobby through the relay, with pictures (Y3-lobby-host.png, Y3-lobby-friend.png): the friend sees "Playing through a relay server", the host sees the warning |
| Y4 | The real relay program (s3air_relay.exe) attacked over the loopback: register and join, data both ways, wrong keys get no answer and 8 block an address, a flood of 40000 datagrams is rate limited, it keeps serving afterwards, garbage / oversized / truncated datagrams do not crash it |

Limits: everything on one computer. "Direct path blocked" is a proxy that drops all datagrams, not a real strict NAT; the relay ran on 127.0.0.1, so the extra delay of a real detour through a server was not measured. The fallback after "the game server did not answer" and "the server does not know the code" is in the code but has no runtime test (the test switch that drops all of one game's UDP traffic would also drop the relay's). Direct IP join codes never use the relay. Nobody hosts a relay: no server was deployed, and the public matchmaking server protocol was not touched.

## In-game updater and releases (2026-11-11, stage E)

Run with `checks\run_update_checks.ps1` (part of `run_online_backend_checks.ps1`, runs last). Offline core tests: about 55 lines starting with "update" (versions, SHA-256 test vectors, zip-slip names, forbidden content, addresses, release JSON, manifest and digest rules).

| Stage | What it proves |
| --- | --- |
| H1-H11 | The install helper alone on throw-away folders: installs, deletes stale files, keeps saves/settings/user mods/config.json, backup; rollback after a really locked file, after the game files and after the mods; locked program file; tampered staging; unsafe path; protected files skipped; waits for the game process; no deletions without a list |
| T4-T5 | Same version and older release: nothing is downloaded (no downgrade) |
| T6-T7 | No network, rate limit, 404 (private repository), server error, garbage answer: plain messages, game folder untouched |
| T2 | Wrong SHA-256, manifest and digest that disagree, no manifest and no digest: nothing unpacked or installed |
| T3 | Zip-slip (.., absolute, drive letter, backslashes), saves/settings in the package, no program file, wrong version inside: refused, nothing written outside the staging folder |
| T8-T9 | Partial download and cancel: leftovers deleted, nothing installed |
| T12 | Build without version.txt: updater off, server not asked |
| T1 | Happy path with the real game: check, download through a 302 redirect, SHA-256, unpack, helper, new files in, stale file gone, saves/config/mods untouched, backup kept, new game starts and says "up to date" |
| R1 | `release.ps1 -DryRun`: refuses a dirty tree and a bad version, builds, writes zip + .sha256 + manifest, creates no tag, no save/ROM/settings/test marker in the zip |
| R2 | The real release zip (about 134 MB) installed by the real updater over an older copy: every file equal byte for byte |

Limits: the server is a loopback program that imitates GitHub (JSON shape, `digest` field, 302 redirect), not GitHub itself; HTTPS, certificate checks and the real CDN redirect were not exercised (the code only follows https redirects to github.com / githubusercontent.com in normal mode, but that path was only unit tested). Nothing was published. The Options menu entry was built but only exercised through the test switch (see the handoff).

## Character choice in the online lobby (2026-11-12)

Every online player (host and friends) picks the character they play in the lobby; the host checks every pick, the picks are fixed when the game starts, every computer's scripts read the same four numbers.

Offline core tests (33 new lines starting with "characters:" and "compatibility:"): the list of installed extra characters (sorted, no duplicates, bounded), what each slot may pick (Tails only for player 2, "game menu" only for the host, the old roles are the defaults, two players may be the same character, the mod's older "Player 3/4 Extra Character Slot" setting stays the default), the host's check of a request (all 65536 numbers in every slot end in a character that player can have), cycling, the rate limit, the reservation that keeps a reconnecting friend's pick, the picks of a session, the packets (ready message, lobby state, start message: round trip, older layout read as no request / unknown, truncated, forged numbers, older start message refused) and the version handshake (a join message or answer without the lobby message version reads as version 0, a version mismatch is a join result of its own).

| Stage | What it proves |
| --- | --- |
| K1a-K1h | Host and three friends through the real lobby screens with a test extra-slot mod (checks\harness\ExtraSlotTestMod, our own code, slot 201): the lobby lists the extra character; everybody picks something else than the default (host Knuckles, player 2 turned into Sonic, player 3 the extra character, player 4 Knuckles); the host accepts and everybody sees the same picks; the four computers log the same fixed picks; in the game every computer reports (CharPickHarness) the same bodies: main character Knuckles with Tails as the second character, player 2 turned into Sonic, player 3 the extra character on a Sonic base with its own mod drawing it, player 4 Knuckles, reported twice 1500 frames apart; a friend whose lobby connection broke came back to its slot and pick, a friend whose game connection broke while playing was restored and the picks did not change; the sync check keeps passing on every friend with no repair; no script errors |
| K2a-K2g | The extra-slot mod is NOT installed: only Sonic, Tails, Knuckles are listed; a friend who asks for the missing extra character and one who sends a made-up number are refused and told (the lobby screen of those friends shows the note), and play the roles of old; the game runs without errors, same state everywhere |
| K4a-K4g | The extra-slot mod goes away between the pick and the start (test switch S3AIR_CHARACTER_LIST_DROP makes the host list of extra characters empty after 75 seconds): the host gives player 3 the default of its slot and tells it (log and the lobby screen), the game starts with Knuckles for player 3 on every computer, same state, no errors |
| K3a-K3g | Only two players (host and one friend): the friend asks for the test extra-slot character as player 2, the Tails body of player 2 is turned into it on both computers (a Sonic base, drawn by the test mod), the friend keeps its own full screen view of player 2, same state, no errors |
| W2i | The lag proxy run with a host pick (Knuckles) and a friend pick (Sonic): both computers log the same fixed picks, and the usual W checks (sync, frame rate, no repairs, input delay) still pass |
| X5b | A friend that speaks the lobby messages of the version before the character choice (test switch S3AIR_NETPLAY_OLD_LOBBY_PROTOCOL sends version 0 in the join message): the host refuses it as another version and the friend is told "different version" (the same path as X4), it does not join |
| pictures | checks\last_logs\K1-lobby-host.png, K1-lobby-friend.png (the lobby with the CHARACTER entry and each player character), K1-*-charpick.bmp (the game) |

Limits: one computer, loopback/LAN, level Angel Island Act 2 only (no intro, no boss). The extra character is a test mod written for this check, not a real character mod: real extra-slot mods (Reto, Modern Sonic, ...) were not run, their abilities depend on how they hook into the framework. Player 2 as another character than Tails is turned from the Tails body after it has joined (never in the Angel Island 1 intro or a cutscene: not exercised; deaths, bosses, act changes and water not exercised either; the player 2 hurt path is the existing one). The host's own pick is Sonic or Knuckles (with Tails); Tails as the host is not offered because the game then has no second character; an extra character for the host is picked in the game's own menu (GAME MENU), which was not run with a real extra-slot mod. Save slots: if the host's pick differs from the save slot's character the game does not write to the slot (read from the script, not played through an act). The sync check compares only the 64 KB work RAM: the four picks (shared memory) are not part of it, so a pick that differed on one computer would not be noticed by it; they are compared by the runner through each computer's own report (K1d, K2c, K3a). A friend who joins a running game later (not through the lobby) gets the pick that was fixed for its slot at the start. The Init fix (main.lemon) concerns every state load online (a late joiner, a reconnect, a repair), not only this feature.

## Blue Sphere special stage with several online players (2026-11-12, stage J)
Run with `run_checks.ps1 -RuntimeOnly -EngineRoot sonic3air_online_stable_source -Only J` (or J1, J2, J3, J9); also part of `run_online_backend_checks.ps1`. The test mod is checks\harness\BSMultiHarness (own code, never packaged).

| Check | What it proves |
|---|---|
| J9a-J9e | Offline (and host-only online) the stage is unchanged: run state, grid hash and end of the stage at fixed frames equal checks\golden\bs_offline_golden.txt (recorded with the unmodified scripts), three scenarios; 20 unit checks of the multi-runner rules (next runner to watch, end rule, roster, friends' score, design constants, memory layout, undo areas) |
| J1a-J1k | Four real game windows through the lobby: all enter the stage at the same frame, every 30 frames the position/counters/status/grid hash of all four runners are identical on every computer, one runner fails (its player watches the others and can switch with left/right), one completes, one times out, emerald +1 once, back in the level with the same scores; the sync check keeps passing with NO repair while the computers show different runners (J1f); the view probe (J1p) renders every runner from the same RAM at the start, during the win sequence and back, and finds no work RAM byte that depends on the view; pictures of the running stage, the spectator view and the results in checks\last_logs |
| J2a-J2g | Three players, everybody fails: the stage ends when the last one is out, no emerald, level restored, same state everywhere, no repair |
| J3a-J3m | Two players and one connected friend who never joined (watches); one completes, one fails; the picture of every running player shows his runner |

Limits: one computer, loopback/LAN; every runner is drawn with the host's main character; players who lose the connection run on with neutral input until a red sphere or the time limit; the giant ring and pause stay host only.
