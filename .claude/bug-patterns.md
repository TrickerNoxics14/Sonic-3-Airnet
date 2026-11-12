# Bug patterns

Written by the exterminator skill. Each entry is a bug class found in this
project, the assumption that broke, the search that finds its siblings, and
the guard that now prevents it. Future sessions read this first and rerun the
stored searches to catch new copies early. Keep entries short; edit freely.

## 2026-11-08 · invalid-test-navigation · lemon
- Seed: checks/harness/ActHarness/scripts/main.lemon:66 - invalid camera diagnostic and a bot that walked into enemies
- Broken assumption: Holding right and teleporting x after initialization represents a valid level route
- Search: `rg -n "Teleport|TELEPORT|control.pad1|move_area.bottom" checks/harness`
- Siblings: checks/harness/ActHarness/scripts/main.lemon fixed (initialize camera before scrolling setup; jump and spindash at obstacles) · checks/harness/BossHarness/scripts/main.lemon suspected (isolated fixtures remain incomplete; require actual boss HP)
- Guard: G04/G11/G12 require joining, sustained split and both players alive; Windows mutex rejects overlapping runs
- Lesson: Scene input must initialize the level state it depends on; an arbitrary teleport is not proof of a gameplay regression.

## 2026-11-09 · extended-controller-truncation · lemon
- Seed: DropInCoop/scripts/teamwork.lemon — X/Y controls cannot use the eight-bit control.pad state.
- Broken assumption: all controller buttons fit in the legacy byte.
- Search: `rg -n 'CONTROL_(X|Y|Z)|Input.getController|control.pad' DropInCoop/scripts`
- Fix: read synchronized sixteen-bit controller inputs; derive pressed edges from their previous values.
- Guard: F8 exercises the full Down+Y hold and cancellation; F13 checks carry consent and X release.

## 2026-11-09 · invalid-checkpoint-test-region · lemon
- Seed: checks/harness/ChecksHarness/scripts/teamchecks.lemon — starpost unloaded before second-player touch.
- Broken assumption: moving a test camera also updates object-streaming bounds.
- Search: `rg -n 'despawn_position|Checkpoint.BaseUpdate|fn02d164' checks/harness`
- Fix: initialize the fixture's streaming origin alongside its camera; exercise real starpost collision rather than just the save callback.
- Guard: F10 requires second-player checkpoint activation; F11 requires both real death-boundary callbacks to respawn independently.

## 2026-11-09 · narrow-object-pointer-reconstruction · lemon
- Seed: DropInCoop/scripts/coop2.lemon — extra-player spill owner reconstructed as 0000b40c rather than ffffb40c.
- Broken assumption: adding a wide literal to a u16 read produces a wide result in every call context. The diagnostic observed truncation before passing the result to partyIndex.
- Search: `rg -n 'partyIndex.*u16|0xffff0000.*u16' DropInCoop/scripts`
- Fix: cast the stored low word to u32 before arithmetic for spill, collected-ring, and item-effect owners.
- Guard: P3/P4 damage checks require only the hurt player's rings to be lost. The original failing logs showed Sonic losing 40 rings while Knuckles retained 25; corrected logs retain Sonic's rings and zero Knuckles'.

## 2026-11-09 · duplicate-mod-override · lemon
- Seed: extra-player score/damage/spill hooks duplicated existing same-module overrides.
- Broken assumption: base.X chains earlier definitions inside one mod. It addresses the base module, and a base call from another function name is rejected.
- Search: `rg -n '^function ' DropInCoop/scripts`
- Fix: one handler per signature; extra-player branches live in the existing score/damage/spill entry points. Helpers do not make illegal base calls.
- Guard: B3 rejects duplicate same-signature implementations across all mod scripts, while allowing real overloads; F retains original two-player regressions.

## 2026-11-09 · missing-stable-address-hook · lemon
- Seed: coop2.lemon fn01a67a ring-spill override was bypassed by official stable v26.03.28.0.
- Broken assumption: every supported engine automatically inherits address hooks from base functions. That behavior arrived in the newer test engine.
- Evidence: stock P3/P4 damage tests left Knuckles at 25 rings and reduced Sonic from 40 to 12; explicit address hooks make both tests pass with the same assertions.
- Fix: explicit address-hook declarations on 34 existing mod overrides whose base functions expose raw-address entry points.
- Search: compare address-hook annotations preceding function declarations in game scripts against matching mod overrides.
- Guard: checks/run_stock_checks.ps1 runs the existing three/four-player gameplay assertions using an unmodified official stable binary and stock debug-dump reporting. Framebuffer capture is explicitly excluded because stock has no custom capture binding.

## 2026-11-09 · wrong-scene-fixture · lemon
- Seed: checks/run_checks.ps1 H20 used zone_act 0x0901 for the Lava Reef boss arena and never started the boss
- Broken assumption: the end-of-act boss remains inside the regular 0x0901 scene; source defines the separate boss scene as 0x1600
- Search: `rg -n "0x1600|LRZ_Boss|OBJECTTABLE2_9d|079786" sonic3air_source/Oxygen/sonic3air/scripts/level/10_lrz sonic3air_source/Oxygen/sonic3air/scripts/standalone/resources/level_objects`
- Siblings: checks/run_checks.ps1:849 | fixed | target actual boss scene 0x1600 · checks/harness/BossHarness/scripts/main.lemon | fixed | call original fn059c8c scrolling trigger after the boss gate is reached
- Guard: H20 requires 60 active encounter frames and 30 consecutive fully active party frames, with no split during the boss
- Lesson: Scene IDs and encounter entry must be derived from the game source, not inferred from the act label.

## 2026-11-09 · capture-before-validation · lemon
- Seed: checks/harness/ActHarness/scripts/main.lemon captured the four-view framebuffer after 30 split frames, before all four cameras were verified
- Broken assumption: the first split frame is already a valid four-way render
- Search: `rg -n "checks\.captured|quadSeen|quadValidFrames|render capture" checks/harness/ActHarness/scripts/main.lemon checks/run_checks.ps1`
- Siblings: checks/harness/ActHarness/scripts/main.lemon | fixed | capture waits until quadSeen is proven · checks/run_checks.ps1 | fixed | quadSeen scenes always require four-quadrant pixel validation
- Guard: QGxxV framebuffer sampling requires a continuous 60-frame valid four-camera interval
- Lesson: Capture evidence after the behavior under test is established, not after a related but weaker state.

## 2026-11-10 · crowded-visual-test-fixture · lemon
- Seed: checks/harness/PartyHarness/scripts/main.lemon — P3/P4 were forced to `anchorX + 0x10` and `anchorX + 0x20`, so the four-player framebuffer showed overlapping sprites.
- Broken assumption: Any nearby player placement is adequate for a human-readable visual regression capture.
- Search: `rg -n "anchorX \\+ 0x(10|20)" checks/harness`
- Fix: Space P1–P4 by 0x50 pixels in the capture fixture and assert each adjacent pair is at least 0x40 pixels apart.
- Siblings: No other harness used those offsets; the four-player shared-camera fixture is for movement tests, not quadrant capture.
- Guard: Q5 checks character spacing; Q6 captures the framebuffer; Q8 checks visible level pixels in all quadrants.
- Lesson: A framebuffer can pass structural checks and still be hard to inspect; test fixtures should make the behavior readable.

## 2026-11-10 · frozen-state-no-cleanup · lemon
- Seed: DropInCoop/scripts/teamwork.lemon - boss win pose set on players but never released
- Broken assumption: the game clears WIN_POSE and DISABLE_* control flags by itself
- Search: `grep -n 'WIN_POSE\|DISABLE_UPDATE' DropInCoop/scripts/*.lemon`
- Siblings: DropInCoop/scripts/teamwork.lemon fixed (team_pose_mask release) · boss restart edge suspected
- Guard: B1-B3 static checks only; no runtime test
- Lesson: Every state the mod sets on a player needs a matching release path.

## 2026-11-10 · stale-cache-on-rejoin · lemon
- Seed: DropInCoop/scripts/party.lemon joinParty - camera3/4 valid flags kept from before leave
- Broken assumption: a rejoining player starts with fresh camera/view state
- Search: `grep -n 'camera[34].valid\|view[34].state_valid' DropInCoop/scripts/*.lemon`
- Siblings: party.lemon joinParty fixed
- Guard: none runtime
- Lesson: Reset cached validity flags whenever the owner (re)joins.

## 2026-11-10 · claim-exceeds-evidence · docs
- Seed: checks/CLAUDE_HANDOFF.md and run_checks.ps1 - checks ran against the wrong tree; M passed via LAN fallback
- Broken assumption: a PASS from a fixture or fallback proves the shipped product
- Search: `grep -n 'sonic3air_source' checks/*.ps1`
- Siblings: run_checks.ps1 A1/A3/A4 fixed (pkgroot) · M1/M2 LAN fallback needs ok
- Guard: A-stage now reads the package's own tree
- Lesson: Verify the checked artifact is the shipped artifact.

## 2026-11-10 · build-artifact-leaks-environment · powershell
- Seed: Sonic3AIR.exe embeds C:\Users\<name> (PDB path); no GPL LICENSE in zip
- Broken assumption: release builds carry no builder-specific data and license text
- Search: `grep -c 'Users' Sonic3AIR.exe`
- Siblings: make_package.ps1 fixed (license copy, leak warning) · exe rebuild with /PDBALTPATH needs ok
- Guard: make_package.ps1 warning
- Lesson: Scan release binaries for builder paths before publishing.

## 2026-11-10 · works-only-in-dev-tree · powershell
- Seed: checks/make_package.ps1:32 - packaged data/ from the source tree without engine data; zip failed at start
- Broken assumption: the game's data folder is self-contained, because it runs from the build tree
- Search: `grep -n 'Join-Path $game "data"' checks/*.ps1`
- Siblings: checks/make_package.ps1 fixed · checks/make_local_package.ps1 not a bug (mod-only zip, no engine)
- Guard: A11 content check and U stage (unzipped run); negative-tested
- Lesson: Test the package, not the folder it was built from.

## 2026-11-11 · hard-coded-dimension · lemon
- Seed: sonic3air scripts level_aiz.lemon:2049 and :965 - zone code written for a 224 line screen; with a taller screen a u16 count went negative and a u8 loop never ended, overwriting RAM
- Broken assumption: the screen is always exactly 224 lines tall, so lines = 0xde - x stays positive and u8 counters reach the bound
- Search: `grep -rn 'u8 [a-z]* = 0; [a-z]* < getScreenHeight()' sonic3air_online_stable_source/Oxygen/sonic3air/scripts`
- Siblings: level_aiz.lemon:965 u8 loop fixed (dropincoop screensize.lemon override) · level_ssz.lemon:653 u8 loop fixed (override) · level_aiz.lemon:2049/2160/2200 negative D1 into fn239e74 fixed (guard) · rendering.lemon:161/176 not a bug (native in the game app) · bluesphere, gumball bonus, menus u8 loops not a bug (not reachable in split screen)
- Guard: Z stage with larger views; G stage across 24 acts
- Lesson: A value a game never changed is not a constant; test the extreme before relying on it.

## 2026-11-11 · vram-region-overlap · lemon
- Seed: DropInCoop planes2.lemon - taller views put plane B rows where the game DMAs its line scroll table (0xf000)
- Broken assumption: VRAM above the name tables is free
- Search: `grep -rn 'copyToVRAMbyDMA' sonic3air/scripts`
- Siblings: planes2.lemon plane B base fixed (moved below 0xf000 while enlarged)
- Guard: G stage screenshot of the lower view
- Lesson: Map what lives at each address before growing a buffer.

## 2026-11-11 · ram-overlap-behind-table · lemon
- Seed: line scroll table at 0xffffe000 ends at 0xffffe380 where the dynamic object list starts; a 288 line screen overwrote it (crash in CheckCollisionsWithDynamicObjects_partial)
- Broken assumption: nothing important sits right behind a table sized for 224 lines
- Search: `grep -rn 'getScreenHeight()' sonic3air/scripts`
- Siblings: split.lemon UpdateLevelDisplay fixed (copy of 0x280 bytes saved and restored)
- Guard: G stage, all 24 acts
- Lesson: A buffer sized exactly for today's maximum has a neighbour; guard it before raising the maximum.

## 2026-11-11 · peer-trusted-by-reachability · cpp
- Seed: NetplayHost::createNetConnection - anyone who could reach UDP 28840 got a player slot; the LAN broadcast carried the session ID in clear, a fake LAN host was believed
- Broken assumption: being able to send a packet to the port (or to answer a broadcast) proves being a friend
- Search: `grep -rn "createNetConnection\|onReceivedPacket" Oxygen/oxygenengine/source/oxygen/network`
- Fix: secret in the join/session code, challenge-response in both directions (NetplayAuth.h); nothing but the handshake is processed before it; broadcast carries a one-way tag
- Guard: core tests (auth, replay, codes); runtime stage S (wrong secret refused, right secret in)
- Lesson: authenticate the peer before giving it any state, and let the peer authenticate the host too.

## 2026-11-11 · test-switch-ships-in-release · cpp
- Seed: getenv("S3AIR_...") in the release exe (hosting at startup, memory dumps, packet dropping)
- Broken assumption: a developer environment variable never exists on a player's computer
- Search: `grep -rn 'getenv("S3AIR\|getEnv("S3AIR' Oxygen --include=*.cpp` (everything must go through TestHooks::getEnv)
- Guard: testhooks.enabled marker (made by run_checks.ps1 only), make_package.ps1 and A12 refuse it, U4/U5 run the unzipped exe with the variables, with and without marker
- Lesson: a debug path needs a switch the shipped product cannot have.

## 2026-11-11 · budget-counts-handled-packets · cpp
- Seed: ConnectionManager::updateReceivePacketsInternal - a LAN discovery packet handled inside the socket returned an empty buffer, which ended the loop; 12 junk packets starved a real one
- Broken assumption: an empty read means the socket is drained
- Guard: core test "junk LAN discovery packets do not hold back a real packet" (failed before the fix with 12 packets)
- Lesson: distinguish "nothing there" from "consumed internally".

## 2026-11-11 · wire-size-used-for-allocation · cpp
- Seed: VectorBinarySerializer blobs, zlib inflate, Lemon runtime save-state counts (call stack, locals, value stack) sized by numbers read from the network
- Search: `grep -rn "serializeData(\|resize(.*read<" Oxygen`
- Fix: per-field limits in the packets, bounded inflate (ZlibDeflate::decode maxOutputSize), bounds in Runtime::serializeState, loadState fails on serializer error
- Guard: core tests (absurd blob size refused, zip bomb stopped)
- Lesson: every length that came over the wire needs a ceiling before it is used.

## 2026-11-11 · failure-without-reason · cpp
- Seed: client ignored refusals (CONNECTION_INVALID) and a vanished host; host waited forever for the server; messages were free text
- Fix: NetplayFailure reasons with plain messages, JoinReply refusal packet, heartbeat, timeouts on both sides, Retry in the lobby
- Guard: runtime stage X (server unreachable, wrong code, blocked, version, mods, lobby full + retry, host closed, host killed, host cannot reach server)
- Lesson: every wait needs a timeout and every timeout a reason the player can act on.

## 2026-11-11 · identity-by-address · cpp
- Seed: NetplayHost kick block and slot reservation keyed by the friend's IP address - the L12 check failed: a kicked friend came straight back because its retry arrived from 127.0.0.1 instead of the LAN address of the same computer (two addresses, one player); on the other side, two friends in one household share one address and would have blocked each other
- Broken assumption: one player = one IP address
- Search: `grep -n "getRemoteAddress().getIP()" Oxygen/oxygenengine/source/oxygen/network/netplay/*.cpp`
- Fix: the join message carries a random per-run token (JoinAuthPacket::mClientToken); NetplayHost::identityOf uses it (address only if there is none) for kick block, slot reservation and "same friend coming back". Siblings left on purpose: the failed-code counter per address (it limits guessing from one place) and the known-peer list for server hosting (it is about where packets may come from)
- Guard: L12 (a retry is refused), L11 (same slot back), core tests for the reservation and block lists
- Lesson: decide what identifies a player before using it as a key; an address is where packets come from, not who sent them.

## 2026-11-11 · test-pins-internal-path · powershell
- Seed: L11 first demanded the log line "taking over its old player slot" from the host; the reconnect really worked, through another path (the dropped friend reused its UDP port, so the host had already replaced the old connection)
- Broken assumption: a behaviour has exactly one internal route, and the log line of that route proves the behaviour
- Fix: the host logs one line for the outcome ("friend came back to its player slot N", whichever route) and the check looks for that, plus the friend's own "back in player slot 3"
- Guard: L11
- Lesson: assert the outcome the player sees; log the outcome at one place that every route passes.

## 2026-11-11 · evidence-picture-of-the-wrong-window · powershell
- Seed: run_checks.ps1 L2 pictures: "host" and "friend 2" screenshots both showed friend 2's window (several game windows on one screen, SetForegroundWindow is not allowed to steal focus)
- Broken assumption: the window that is asked to come to the front is the one that gets photographed
- Fix: Save-WindowPicture minimizes the other game windows, restores the wanted one, then takes the picture
- Guard: pictures L2-lobby-host.png / L2-lobby-friend2.png / W4-lobby-host.png were looked at by eye
- Lesson: a picture is evidence only if it shows what its file name says.

## 2026-11-11 · test-measures-stale-counter · powershell
- Seed: run_checks.ps1 Y4f read the relay's counter file 0.5 s after the garbage datagrams; the file is rewritten once per second, so the count was one short and a correct relay failed the check
- Broken assumption: a status file written periodically shows the state of a moment ago
- Search: `grep -n "stats\|Read-KeyValueFile\|Read-ProxyStats" checks/run_checks.ps1` (sleep at least one write interval, compare against a value read before the action)
- Siblings: Y4d reads after a 3 s pause (fine); W stage reads proxy stats after stopping nothing but with 40 s margin (fine)
- Guard: Y4f now waits 2.5 s and compares with the earlier counter
- Lesson: when a check reads a periodically written file, wait one period and compare with a baseline.

## 2026-11-11 · log-only-in-one-branch · cpp
- Seed: NetplayHost::createNetConnection logged "a friend connects through the relay server" only inside the `mRestrictToKnownPeers` branch; a lobby opened without the matchmaking server never reached it, so Y2b could not see the line although the relay worked
- Broken assumption: a log line placed in the code path that is guarded by a security option is seen on every route
- Fix: the line is logged at the top of the function, before the optional restriction
- Guard: Y2b
- Lesson: log the outcome where every route passes (same as test-pins-internal-path).

## 2026-11-11 · mod-load-order-assumed · powershell
- Seed: checks/run_checks.ps1 stages G, QG, Z, H, F, P, Q listed the harness mod BEFORE DropInCoop (`Set-ActiveMods @("ActHarness", "DropInCoop")`); on the stable fork the game stopped at start with "Script compile error: Unknown function name 'DropIn.partyPlayer' ... module 's3air-drop-in-act-harness'"
- Root cause: a script module can only see the functions and globals of the modules compiled before it, and modules are compiled in the order of active-mods.json. The harness calls DropIn.* and reads dropin.* globals, so it has to come after DropInCoop (the N stage and run_stock_checks.ps1 already did that). The test engine (v26.08.15) accepted either order, so every earlier run hid it.
- Search: `grep -n "Set-ActiveMods" checks/run_checks.ps1` (a harness must come after the mod it imports from)
- Siblings: all seven harness stages fixed (dependency first); TeleportHarness imports nothing
- Guard: G01 and QG05 run on the stable engine (stable runs are the ones that count)
- Lesson: when a test mod imports another mod, list the other mod first; do not rely on an engine version that forgives the wrong order.

## 2026-11-11 · cache-validated-by-version-only · cpp
- Seed: LemonScriptProgram::loadBaseScriptFromBinary - `saves/scripts.bin` (written after every compile from source) was reused although a game script had changed, because its only checks are the engine's native binding hash and the game version number (unchanged: 26.03.28.0 in the official game and in this fork). The new Engine.markFourViewSplitSupported function in scripts/main_gameapp.lemon was invisible (four views "req 0").
- Broken assumption: a compiled cache that loads without error is the compile of the scripts that lie next to it
- Fix: the cache in the saves folder is ignored (with a log line) if any .lemon file of the script folder is newer than it; a shipped data/scripts.bin is not touched by this
- Search: `grep -rn "mCompiledScriptSavePath" Oxygen`
- Guard: U6 (unzipped package with a stale cache is rebuilt) and the four-view stages, which need the marker function
- Lesson: a cache needs a key that changes whenever its input changes; a version number is not one if it is not bumped.

## 2026-11-11 · fixed-width-view-constants · lemon
- Seed: DropInCoop four-view code (split.lemon, planes2.lemon) was written for a 320 pixel screen and never looked at: horizontal band offset 0x60 (= 256 - 160), view width 160 (PARTY_VIEW_WIDTH), camera offset 72; the game window is 400 pixels wide, so each view is 200 wide
- Seed 2: the plane bands were two columns of 16 tiles in a 512 pixel wide plane, but the plane wraps every 512 pixels: a view whose camera x passed a multiple of 256 showed the neighbour band (torn pictures in Marble Garden, Sandopolis ...). The pixel counts of QGxxV could not tell.
- Seed 3: plane B was given the scroll set of plane A and the other way round (the game's line table has plane A in the upper word)
- Fix: four bands below each other in one 32 patterns wide plane (so a view wraps like a normal screen), sizes derived from the screen, scroll sets matched to planes; QGxxR compares each view with the game's own picture of the same camera
- Search: `grep -n "0x60\|PARTY_VIEW\|- 72\|- 44\|>> 1) \* 8" DropInCoop/scripts/*.lemon` and every `160`/`320`/`224` in the mod
- Guard: QGxxR (the four-view picture against the game's own full screen picture of each of the four cameras)
- Lesson: a visual feature needs a check that compares pictures, not one that counts coloured pixels.

## 2026-11-11 · rom-band-table-overrun · lemon
- Seed: fn04f0de (zone line scroll code) with a screen taller than 224 lines reads past the end of a band table in the ROM: "Invalid memory access at 0x400000 of 0x2 bytes" in IceCap 1 and Mushroom Hill 1 (the table is the last data of the ROM there; in other zones it read other data without any message)
- Broken assumption: the number of lines (getScreenHeight() - 1 in D1) always fits the table; true only for the original 224 lines
- Siblings: fn04f0ce, soz 608, ssz_knuckles 489, dez_boss 535, ddz 232, slotmachine 287 (all set D1 = getScreenHeight() - 1, all reach fn04f0de); AIZ water/scroll fixed earlier (screensize.lemon)
- Fix: screensize.lemon overrides fn04f0de and asks for at most 224 lines while the screen is enlarged
- Guard: QG11/QG15 (the two zones that crashed), G stage with view size Large
- Lesson: the extreme case of a changed constant must be run through every zone, not through the zones that happened to be looked at.

## 2026-11-11 · detached-child-never-ran · cpp
- Seed: SelfUpdater::installNow started powershell.exe with CREATE_NO_WINDOW | DETACHED_PROCESS; the game closed and nothing was installed, the helper did not even write its first log line (run_update_checks T1b: empty result)
- Broken assumption: a console program that must outlive the game should be detached from every console
- Root cause: PowerShell needs a console (a hidden one is enough); with DETACHED_PROCESS it exits silently
- Search: `grep -rn "CreateProcess" sonic3air oxygenengine --include=*.cpp` (only this call starts a console tool)
- Fix: CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP; the game also logs the exact command line
- Guard: T1b-T1e (the helper must write its result and the new game must start)
- Lesson: after launching a helper, check for a sign of life it writes itself; "started" only means the process was created.

## 2026-11-11 · rollback-restores-what-never-changed · powershell
- Seed: update_helper.ps1 added every file to the "changed" list BEFORE overwriting it; when the overwrite failed (file open elsewhere) the rollback tried to copy the backup over that same locked file, failed, and reported "rollback incomplete" although nothing needed restoring (run_update_checks H2a: exit 2 instead of 1)
- Broken assumption: everything on the rollback list was really modified
- Fix: Restore-Everything skips a file that already equals its backup (Test-SameFile); a file that cannot be read at all still counts as a problem
- Siblings: the added-files list is safe (removing a file that was never created is a no-op); the mods folder restore replaces the whole folder
- Guard: H2a (real lock, not simulated), H3/H4 (failure injected after the files / after the mods)
- Lesson: a journal written before the action must be undone only as far as the action got, or the undo must be idempotent and tolerant.

## 2026-11-11 · helper-needs-module-autoload · powershell
- Seed: update_helper.ps1 used Get-FileHash; started from a PowerShell 7 session (PSModulePath of 7 inherited by Windows PowerShell 5.1) it died with "Get-FileHash is not recognized", before changing anything
- Broken assumption: the cmdlets of the utility module are always loadable in the helper's environment
- Fix: SHA-256 through [System.Security.Cryptography.SHA256] (Get-Sha256Hex); no Get-FileHash left
- Search: `grep -n "Get-FileHash" checks/package_files/update_helper.ps1` (none); the other scripts of checks\ run under the developer's own shell and are not shipped
- Guard: the helper tests H1-H10 now pass from either shell
- Lesson: a script shipped to players should depend on the language and the .NET base classes, not on modules that autoload.

## 2026-11-11 · evil-test-name-collides-with-real-file · powershell
- Seed: run_update_checks T3f used the file name "settings.json" for the forbidden entry and then checked that no such file appeared in the data folder; the player's real settings.json is there, so a correct refusal failed the check
- Broken assumption: the marker name of a "nothing escaped" check does not exist anywhere yet
- Fix: unique marker names (evil_*.txt, settings_evil.json), which still match the forbidden-name rule
- Guard: T3a-T3f
- Lesson: a "did not appear" check needs a name that cannot exist beforehand.

## 2026-11-11 · std-name-clash-by-adl · cpp
- Seed: a helper function named `quoted(const std::wstring&)` in an anonymous namespace was shadowed by std::quoted through argument-dependent lookup, giving an unreadable operator<< error
- Fix: renamed to quotePath
- Lesson: do not name local helpers after standard library functions when the arguments are std types.

## 2026-11-11 · stock-feature-answers-for-another-program · cpp
- Seed: the stock "Check for updates" of Sonic 3 A.I.R. asks sonic3air.org whether a newer STOCK build exists (build number 26.03.28.0) and offers the stock download page. In this fork it would show a wrong "Update available" and send friends to a program that has no online co-op
- Fix: UpdateCheck::startUpdateCheck() is inert (compile time constant ENABLE_STOCK_UPDATE_CHECK = false), the options entry and its release channel selector now belong to SelfUpdater; nothing ever installed the stock game (the stock check only ever opened a web page)
- Siblings: the remastered soundtrack download (RemasteredMusicDownload) uses the same server but only downloads audio on the player's click; the game server (gameserver.sonic3air.org) is still used for matchmaking and ghost sync, not for updates
- Lesson: when forking, list every feature that talks to the upstream project and decide for each what it should say about the fork.

## 2026-11-11 · wait-window-equals-the-timeout · powershell
- Seed: run_checks.ps1 X3 waited 60 s for the friend to give up (BLOCKED_OR_TIMEOUT); the game needs about 50 s for that plus 10 s of start-up, so on a slower moment of the machine the check failed twice in a row although the game behaved as designed (measured: 63 s from process start)
- Broken assumption: the check's wait window is longer than the thing it waits for
- Fix: the window is 100 s
- Siblings: the other X checks wait 60-80 s for events that take about 15-25 s; W/Y waits have large margins
- Lesson: a wait window must be a multiple of the measured time, not the design value; a check that sits on the edge fails only on busy days.
## 2026-11-11 · low-entropy-secret-hashed-once · cpp
- Seed: NetplayAuth.h derived the LAN broadcast tag, the relay lobby name/key and the join secret as ONE SHA-1 of a 40 bit session ID (30 bit direct secret, one SHA-1 for the proof). Anything an observer sees (tag, proof, relay lobby name) was an offline brute-force oracle: 2^40 single hashes is minutes of GPU time.
- Broken assumption: "only a hash goes over the network" protects a secret that has few bits
- Fix: key stretching (400000 chained SHA-1 per derived value, about 150 ms, computed once per hosting/joining and cached, `NetplayAuth::STRETCH_ITERATIONS`), full 64 bit values derived from the stretched master (the secret used to be truncated to 32 bits), new domain strings (...2). Honest limit written into the header: a cost multiplier, not impossibility; the 30 bit direct join code stays weak against a GPU farm (about a day of one GPU), the 40 bit session code needs years of one GPU
- Search: `rg -n "buildSHA1|hashToUint64" sonic3air_online_stable_source` for any new value derived from a code or from a session ID
- Guard: core tests "auth: values derived from a code go through key stretching" (iteration floor, cached second use, the old single hash no longer matches)
- Lesson: a one-way hash of a small secret is not a secret; either add entropy or add cost, and say which.

## 2026-11-11 · shared-key-is-not-ownership · cpp
- Seed: RelayCore.h let any caller of HOST_REGISTER with the lobby key become the host of an existing lobby. The key derives from the session code that every friend holds.
- Broken assumption: "knows the key" means "is the host"
- Fix: HOST_REGISTER also carries a random 64 bit host secret that only the registering game process knows (RelayProtocol version 2); a later registration without the same secret is dropped silently and counted as a wrong key
- Siblings: DATA_H2R already needs the random host token; NetplayHost identity is a client-sent token (see the handoff: an address block on kick was rejected because it would lock out friends behind one NAT)
- Guard: core tests "a friend who knows the lobby key but not the host secret cannot take the lobby over" (mutation: removing the secret comparison turned two tests red), "the real host can re-register from a new address"
- Lesson: when every participant shares a secret, ownership needs a second secret that only the owner has.

## 2026-11-11 · per-source-state-keyed-by-address-and-port · cpp
- Seed: RelayCore kept rate limits and wrong-key counters per IP:port with a cap of 8192. One machine rotating the source port got a fresh allowance for every datagram, bypassed the block, and could fill the table so that real senders were dropped.
- Fix: state keyed by IP address only (allowance per IP raised to 8/s, burst 24, so a family behind one router still works); when the table is full, entries quiet for 120 s and then 10 s are dropped before newcomers are refused
- Guard: core tests (port rotation, flood fills the table then a newcomer is served), run_checks Y4c (another source port of a blocked IP stays blocked; the "other address" is now 127.0.0.2)
- Lesson: an attacker chooses the source port for free; key abuse state by something that costs them.

## 2026-11-11 · counter-never-decays · cpp
- Seed: NetplayHost::mFailedAuthByIP counted wrong proofs per IP forever (never reset, never pruned): eight typos, or a guesser behind the same NAT, locked a friend out for the whole session and the map grew without bound. Same shape: UDPSocket::mLanRequesterIPs stopped accepting new addresses at 16 entries for the life of the process.
- Fix: `NetplayLobby::AuthFailureTracker` (60 s window, a proven code resets, 256 entries at most, oldest dropped), `UDPSocket::rememberRecentAddress` (most recent 64)
- Search: `rg -n "std::map<std::string, int>|size\(\) < [0-9]+\)" sonic3air_online_stable_source/Oxygen/oxygenengine/source`
- Guard: core tests "an address that guessed wrong codes is ignored only for a minute", "the LAN requester list makes room"
- Lesson: every limiter needs a way back; a block that never ends is an outage for the wrong person.

## 2026-11-11 · unproven-connections-fill-the-table · cpp
- Seed: NetplayHost::createNetConnection refused every newcomer once 8 connections existed, and a stranger needs no secret to open one, so eight idle connections kept real friends out for as long as they were reopened.
- Fix: at most two unproven connections per address; a full table gives way (the oldest unproven connection older than 3 s is replaced); proven connections are never replaced (`NetplayLobby::chooseConnectionToReplace`). The fallback identity of a friend without token is now address and port (two friends behind one NAT were one identity, so one could take over the other's slot).
- Guard: core tests for the policy. Not tested end to end with eight real UDP clients.
- Lesson: a cap on a resource that anonymous peers can take must prefer the peers that proved something.

## 2026-11-11 · transient-hint-hides-status · cpp
- Seed: LobbyMenu::pressStart set mMessage ("Asked everybody to get ready...", "Nobody has joined yet.") and nothing cleared it while hosting, so the ready check countdown, the relay warning, the high ping warning and the "everybody is ready" line (all in the else branch of `if (!mMessage.empty())`) were never drawn again. W4 and Y3 passed because they read a log line written by update(), not what render() drew.
- Fix: hosting hints expire after 4 s (mHostingMessageSeconds); render() logs "Lobby: on screen: <what the bottom line shows>" when it changes; W4 and Y3 now require that line
- Search: `rg -n "mMessage" sonic3air_online_stable_source/Oxygen/sonic3air/source/sonic3air/menu`
- Lesson: a check for something that is drawn must observe what is drawn; a decision logged before the drawing proves only the decision.

## 2026-11-11 · promised-by-readme-nobody-implements · cpp
- Seed: README_ONLINE.txt and make_package.ps1 said the game installs bundled_mods by itself; no code did, so a fresh unzipped package ran without the co-op mod. Check A9 only grepped the README, so nothing could notice.
- Fix: ModManager::installBundledMods (called from EngineMain::initFileSystem, before the ROM is needed): mirrors bundled_mods/<name> into the mods folder (copies changed files, removes leftovers) and switches a newly installed mod on once
- Guard: run_checks U8a/U8b start a freshly unzipped package with an empty local savedata folder (the game then uses it instead of %APPDATA%)
- Lesson: a feature promised in a README needs a check that performs it, not one that greps the promise.

## 2026-11-11 · release-from-stale-build · powershell
- Seed: release.ps1 packaged whatever Sonic3AIR.exe was in the build folder unless -Build was given (the clean-tree check cannot see ignored build output), and it compared the new version only with local tags.
- Fix: a real release always builds; the exe must be newer than every tracked source file (a dry run warns); tags are fetched from origin first (failure refuses a real release)
- Lesson: a release must be derived from the commit it names, not from what happens to be on disk.

## 2026-11-11 · helper-and-game-disagree-about-a-file-name · cpp
- Seed: the update package contains Sonic3AIR.exe, update_helper.ps1 looks for the RUNNING exe's name; a renamed program passed the game's package check and failed in the helper after the whole download.
- Fix: the updater is switched off with an explanation when the exe is not named Sonic3AIR.exe (check T12b)
- Lesson: validate the preconditions of the last step before doing the expensive first steps.

## 2026-11-11 · unsigned-parameter-for-screen-position · lemon
- Seed: DropIn.drawPlayerLabel(u8 slot, u16 x, u16 y, ...) received s16 positions (py - 28, px partly off-screen); a negative value became 65xxx in the stock-engine fallback Renderer.drawText (s32).
- Fix: s16 parameters and s16 shared-memory stores; check A15 on the package
- Search: `rg -n "u16 (x|y)\b" DropInCoop/scripts`

## 2026-11-12 · checks-run-against-stale-installed-copy · powershell
- Seed: checks/run_checks.ps1 copied the co-op mod to `<exe folder>\bundled_mods`, but the engine installs bundled mods from `bundled_mods` in its WORKING directory (the source tree's game folder when the runner starts it), so every game stage ran against whatever copy was last put into %APPDATA%\Sonic3AIR\mods by hand. Stage K died at start with "Unknown function name 'DropIn.pick'" (the new script file did not exist in the installed copy).
- Broken assumption: the comment "the game installs the copy from its own folder at each start" holds for a check that starts the game from the build folder (the unzipped package stage U does); it does not hold when the working directory is somewhere else.
- Fix: the runner mirrors the source mod into the mods folder itself (robocopy /MIR; fails the run if that does not work).
- Search: `grep -n "bundled_mods" checks/*.ps1` (every place that relies on the automatic installation must start the game with the right working directory, or install the copy itself)
- Siblings: stage U and L4/L5 start the exe from the unzipped package folder (right directory, fine); the local stages F/G/P/Q ran on the hand-copied mod before this too.
- Guard: stage K needs a function that only the new script defines; it cannot pass on an old copy.
- Lesson: when a check installs something "for the game to pick up", check that the game really looks where the check put it.

## 2026-11-12 · lemon-stringformat-argument-limit · lemon
- Seed: checks/harness/CharPickHarness: a `stringformat` with 16 arguments failed to compile ("No appropriate function overload found calling 'stringformat'"); the game stops with a modal error dialog at start, so every runner wait ran out its full time before the cause was visible.
- Broken assumption: stringformat takes any number of arguments (the standard library has stringformat1 to stringformat8: at most 8).
- Fix: build long lines from several short stringformat calls (at most 8 arguments each).
- Search: `grep -n "stringformat(" -r DropInCoop checks/harness` and count the arguments of each call (more than 8 does not compile)
- Lesson: after an edit to a script, start the game once and look at the window before running a long stage (a script compile error is a dialog, not a log line).

## 2026-11-12 · init-hook-runs-after-every-state-load · lemon
- Seed: DropInCoop/scripts/main.lemon `Init()` reset all mod variables (`dropin.joined = false`, team state, party wants...). Found with stage K: after a friend reconnected in the game, its restored game said `dropin.joined 0` while the host and the other friends had 1 (player 2 was playing); the player-2 rules (hurt, rescue, team moves) then differ on that one computer, and the 64 KB memory comparison cannot see script variables.
- Root cause: the engine calls the script function `Init` at the end of EVERY `CodeExec::reinitRuntime`, which includes loading a game state (a friend who joins a running game, comes back, or is repaired). The state's script variables are loaded first and `Init` then overwrote them with defaults on that computer only.
- Fix: online (`DropIn.net()`) and in a running level (`level.framecounter != 0`) `Init` keeps what the state loaded; on a fresh start (counter 0) and offline it resets as before.
- Search: `grep -n "function void Init()" -r DropInCoop checks/harness sonic3air_online_stable_source/Oxygen/sonic3air/scripts` and read what each one resets; also anything in C++ that assumes script variables are zero after a load.
- Siblings: the stock `Init` in standalone/enginecallbacks.lemon only re-derives values from settings (safe to repeat, it says so itself); `InitializeGameCharacters` runs at level start on every computer at the same frame (fine); the test harness mods keep their counters in globals that the state restores (fine).
- Guard: K1f (a friend whose game connection breaks while playing is restored and reports the same bodies, with `joined` true, 1500 frames later). Not covered: other mod variables that are set once and never again; they are restored from the host now.
- Lesson: a reset hook that runs "on load" must know whether the load brought state with it.

## 2026-11-12 · host-decided-value-replaced-on-one-computer · cpp
- Seed: NetplayManager::setSessionCharacters (used by the friend for the host's start message) re-checked each pick against the friend's own list of installed extra characters and silently put the slot default in its place. That list is cached for 1 s and is empty while the script program reloads, so a host pick (extra character 90) could become Knuckles on one computer only: different actors, and the sync hash (64 KB work RAM) does not cover the picks.
- Root cause: a value that the host decided and that every computer must run identically was "sanitized" on the receiving side with local state. Sanitizing belongs to the authority; a receiver may only accept or refuse.
- Fix: friends use the host's picks exactly as sent or refuse the start with the "different mods" failure and a reason (NetplayLobby::findUnusableSessionPick, strict setSessionCharacters; the list is re-read right then; the same picks again during a repair are not re-checked). The host adopts what its own manager made valid (no divergence between what it sends and what it runs).
- Search: `grep -n "sanitize" -r sonic3air_online_stable_source/Oxygen/oxygenengine/source/oxygen/network/netplay` and ask for each call: who is the authority, and is this the authority?
- Guard: core tests "a friend finds out whether it can use the host's picks exactly as sent ..." (mutating the helper turns them red). Not tested at runtime (a friend without the mod is refused earlier by the mod fingerprint; the stale-list window cannot be provoked reliably).
- Lesson: never replace an authoritative value with a default on one peer; refuse instead.

## 2026-11-12 · stale-state-undoes-local-choice · cpp
- Seed: NetplayClient lobby state handler took over the host's pick whenever the "changed by the host" bit was set and the pick differed from the friend's request. The bit stays set until the host accepts a changed request, and lobby states are rebroadcast about once a second, so a state sent before the host read a new request reverted the friend's choice (and the next heartbeat then asked the host for the old pick).
- Fix: NetplayLobby::CharacterRequestTracker: a fresh request is kept for 3 s or until the host's state shows it; only then a different pick with the bit set is taken over. Picks fixed at the start and host-side changes of a pick that the friend did not just ask for still apply at once.
- Search: `grep -n "mCharacterNotice\|mLobbyCharacters" -r sonic3air_online_stable_source/Oxygen`; any "take over what the server says" code needs to know whether the server has seen our newest request.
- Guard: core tests "an old lobby state ... does not undo a request that was just made" (settle time set to 0 turns them red).
- Lesson: optimistic local state needs a pending marker, or every old broadcast undoes it.

## 2026-11-12 · short-grace-timeout-discards-a-choice · cpp
- Seed: NetplayHost::friendsHaveAnnounced let the game start after 3 s without a friend's first lobby message, which fixed the friend's character at the slot default, with nothing written down.
- Fix: 10 s (NetplayLobby::ANNOUNCE_WAIT_SECONDS, messages are reliable so a friend silent for that long is about to time out), the host logs every friend that had not announced at the lock and every request that comes after the lock (once per friend), the friend logs when the fixed pick differs from what it asked for.
- Guard: core test for the wait rule. Not tested at runtime (would need a delayed first message).
- Lesson: when a fallback discards user input, say so in the log of both sides.

## 2026-11-12 · test-fixture-shares-the-fallback-value · lemon
- Seed: the only extra character of the checks was Sonic based and the mod's fallback for "base character unknown" is Sonic, so a broken base-character lookup (resolveExtraBaseCharacter) passed every check. Confirmed by mutation: with the lookup disabled the old K scenarios stay green; the new Knuckles-based slot 202 turns K3a red.
- Fix: checks\harness\ExtraSlotTestMod got slot 202 (CHARS_KNUCKLES_ALONE), used for player 4 in K1 and player 2 in K3; the expected body is a Knuckles.
- Lesson: a test double must differ from the default it is meant to be distinguished from.

## 2026-11-12 · clear-memory-slot-without-owner-check · lemon
- Seed: DropIn.convertPlayer2 cleared the object slot 0xffffcc9e (the dust cloud of the second character) unconditionally, the tails object at 0xffffcc0a only after an owner check.
- Fix: DropIn.isPlayer2DustObject (owner field +0x42 = 0xb04a, or the unrun stock dust object); harness checks the guard on the live memory (K1i) and the dust is still there for a Tails player 2 (p2dust 1 in K2).
- Search: `grep -n "zeroMemory" -r DropInCoop/scripts` (the other zeroMemory clears an actor slot that this mod itself created).

## 2026-11-12 · private-memory-areas-without-declared-size · lemon
- Seed: private scratch areas of players 2, 3 and 4 were bare numbers in three places; nothing said how large they are or that they must not overlap.
- Measured: the legacy helpers use the dust object at A6 (0x4a bytes) and one word at A5 = A6 + 0x70, so the areas did not overlap (player 4: up to 0x8fe1f1, player 2 from 0x8fe200). Not a live defect, but unguarded.
- Fix: constants SPEED_SIZE, SCRATCH_SIZE, PARTY_SCRATCH3/4 and check B4 (all six areas declared, no overlap). Third-party extra-character code may use A5/A6 differently: unknown.

## 2026-11-12 · check-script-overwrites-the-players-copy · powershell
- Seed: run_checks.ps1 mirrors the repository's DropInCoop over %APPDATA%\Sonic3AIR\mods\DropInCoop at every run (needed: the engine only installs bundled mods from its working directory), without keeping what was there.
- Fix: if the installed copy differs from the source (SHA-256 of every file), it is copied to %APPDATA%\Sonic3AIR\mods_backup\DropInCoop_<time> first (newest 3 kept; outside the mods folder so the game does not load it).
- Lesson: a check that "installs the thing under test" into the user's real folders must back up what it replaces.

## 2026-11-12 · lemon-oneline-if · lemon
- Seed: checks/harness/BSMultiHarness/scripts/main.lemon:196 and :329 - 'if (cond) statement' on one line stops the game with a Script compilation error
- Broken assumption: Lemon accepts the statement of an if on the same line, like C does
- Search: `powershell -NoProfile -File checks/lint_lemon_oneline_if.ps1`
- Siblings: BSMultiHarness main.lemon:196-198 fixed (three if (...) pad | = ... lines) · BSMultiHarness main.lemon:329 fixed (if (...) Checks.report(...)) · all 3034 other .lemon files in the repo not a bug (scan finds no other same-line if)
- Guard: checks/lint_lemon_oneline_if.ps1 with -SelfTest (red on the old lines, green on the fix)

## 2026-11-12 · stale-compiled-script-cache · powershell/engine
- Seed: sonic3air_online_stable_source/Oxygen/sonic3air/saves/scripts.bin (the engine's compiled script cache) was older than the edited game scripts; the engine loaded it because only its core-module hash is compared, so a check "against the changed scripts" ran against the OLD ones with no message (first symptom: the harness reported "Unknown function name 'BSMulti.readRosterMask'" although the function was in the source; the offline "unchanged" comparison had passed trivially before that)
- Broken assumption: edited game scripts are used by the next game start
- Search: `rg -n "scripts.bin|loadBaseScriptFromBinary" sonic3air_online_stable_source/Oxygen/oxygenengine/source/oxygen/simulation/LemonScriptProgram.cpp checks/*.ps1`
- Fix: run_checks.ps1 removes saves\scripts.bin when a game script is newer than it (the engine writes it again after compiling from source).
- Guard: the removal message in the run output; J9 compares the offline run with the golden recording AFTER the cache was rebuilt from the new scripts (16 lines identical).
- Lesson: when a check proves "no change", first prove that the changed code was actually the code that ran.

## 2026-11-12 · lemon-ternary-mixed-constant-address · lemon
- Seed: checks/harness/BSMultiHarness/scripts/main.lemon BSH.stateBase: `return BSH.active() ? (BSH.BLOCKS + runner * BSH.STRIDE + 0x500) : 0xffffe420` returned the wrong address (reads and writes both went to the block address although the condition was false), so layout edits never reached the running stage
- Broken assumption: a ternary with a constant expression and a literal gives the chosen operand with the type of the other
- Search: `rg -n "\? \(?[A-Za-z.]+ \+ .* : 0x" --glob "*.lemon"` and every ternary that returns an address
- Fix: plain if/else with a typed local (production bluespheres_multi.lemon uses no ternary for addresses or u8 values)
- Guard: the golden comparison J9 and the multi-runner checks read the layout back from the same RAM that the game uses.

## 2026-11-12 · stock-memory-used-as-state-flag · lemon
- Seed: first design of BSMulti read "active" from the work RAM header at 0xffffe5c0, which in the stock stage holds the Tails follower's position history (nonzero) so the stock path would have looked "active"
- Broken assumption: a RAM area that is unused in the new mode is zero in the old mode
- Search: `rg -n "0xffffe5[0-9a-f][0-9a-f]" --glob "*.lemon"`
- Fix: the "active" flag lives in the shared memory (zero in the stock game), the RAM header is read only while that flag is 1; the harness reads the same flag.
- Guard: J9 (offline run state equals the recording) and the unit check for the memory layout.

## 2026-11-12 · powershell-wrapped-array-in-pipeline · powershell
- Seed: checks/run_checks.ps1 Get-JReports returns `,@(...)`; `Get-JReports ... | ForEach-Object` then saw ONE object (the whole array), so a regex matched only the first report line and the spectator check reported "views: 0" although the log had views 0 and 1
- Search: `rg -n "Get-(K|J)Reports .*\|" checks/run_checks.ps1`
- Fix: iterate with foreach ($line in (Get-JReports ...)) .

## 2026-11-12 · lemon-string-256-limit · lemon
- Seed: checks/harness/BSMultiHarness/scripts/main.lemon BSH.stateLine()/"bs end" report: a state of four runners built as one stringformat result stopped the game with "Too long string" (lemon/utility/FastStringStream.h: every stringformat result is limited to 0x100 = 256 characters)
- Broken assumption: a script string can be as long as needed (here: prefix plus 4 runner entries of about 55 characters each joined with " | ", plus a tail)
- Search: `rg -n "stringformat\(\"%s" --glob "*.lemon" checks DropInCoop` (every stringformat that feeds a string back into itself in a loop or joins variable-length parts)
- Fix: BSH.reportState emits one report line per runner (every part starts with "f<frame> ") and run_checks.ps1 puts the parts of a frame together; with one runner the line is unchanged (golden format). BSH.hexState (unbounded) removed; the probe list of differing bytes is capped at 12 entries.
- Guard: check J*l requires every report text to be at most 250 characters, and "Too long string" is in the script error pattern of stage J.

## 2026-11-12 · render-phase-ram-write-not-undone · lemon
- Seed: sonic3air_online_stable_source/Oxygen/sonic3air/scripts/level/special/bluespheres_multi.lemon BSMulti.undoAddress - the render of a runner that shows the emerald art switched it back with Nemesis.loadDataToVRAM, which writes its decompression buffer 0xffffaa00 and stack leftovers 0xfffffdf6 into the hashed work RAM, only on the computer watching that runner: false desync repairs (3 in a 4 player run)
- Broken assumption: everything the render phase writes into work RAM is in the undo set; the stage start probe (one frame, ring art only) proved that
- Search: `rg -n "Nemesis\.|Kosinski\.|updateSpritePattern|Enigma\." sonic3air_online_stable_source/Oxygen/sonic3air/scripts/level/special/bluespheres_multi.lemon`
- Siblings: bluespheres_multi.lemon BSMulti.updateEmeraldArt Nemesis path fixed (undo ranges 0xffffaa00 len 0x200 and the stack 0xfffffd00 len 0x100) · first fix used 0xffffaa00 len 0x600, which also covered the sprite lists at 0xffffac00: the runner vanished from the picture (seen in the screenshot) fixed, range cut to 0x200 · Kosinski.decompress in updateEmeraldArt not a bug (writes only the shared scratch area, probe at call 560/640 shows 0 bytes)
- Guard: harness probe now runs at calls 100, 470, 560, 640 and also renders back to runner 0 (art switch both ways); unit check 'bs unit undo' (no undone range overlaps the sprite lists); check J3m counts runner pixels in the picture
- Lesson: a probe must be run in every phase of the thing that is rendered (here the emerald sequence), not only at the start; and an undo range must be as small as the measured write

## 2026-11-12 · render-phase-ram-write-not-undone · lemon
- Seed: bluespheres_multi.lemon BSMulti.cleanup: clears restore_pending without restoring the undo set; the last render's RAM stayed in hashed work RAM when the stage ended
- Broken assumption: the stage loops always do a frame wait after the last render, which restores the RAM
- Search: `rg -n 'restore_pending' sonic3air_online_stable_source --glob *.lemon`
- Siblings: bluespheres_multi.lemon BSMulti.cleanup fixed (calls onbeforeyield first) · BSMulti.mainLoop level.restart return path fixed (same cleanup) · game_pause.lemon pause loop: waitForNextFrame without a render fixed (bsmulti.onpauseframe) · 0xfffff62c sprite count written by RenderSprites not in the undo set fixed (undo index 6)
- Guard: harness override of BSMulti.cleanup reports pending/diff; J*s (red on old code: diff 9); pause probe J*r; J9f source check

## 2026-11-12 · ghost-participant-never-leaves · lemon
- Seed: bluespheres_multi.lemon roster fixed at stage start; a player who left keeps a zero-input runner that counts as running (up to 7 minutes)
- Broken assumption: a roster member keeps playing or fails by himself
- Search: `rg -n 'STATUS_RUNNING|isRunning' sonic3air_online_stable_source --glob *bluespheres_multi.lemon`
- Siblings: evaluateEnd end rule fixed through bsmulti.updateidle (30 s without any button = timed out)
- Guard: unit checks idle1-3, scenario J4 (end reason 1 at frame ~1992, not the 7 minute cap)

## 2026-11-12 · checksum-misses-fields · lemon
- Seed: bluespheres_multi.lemon BSMulti.updateChecksums: object hashed 0x48 of 0x4a bytes, plane B word of runners 1-3 not hashed
- Broken assumption: word loops over a block cover every field that matters
- Search: `rg -n 'sumWords' sonic3air_online_stable_source --glob *.lemon`
- Siblings: grid checksum 16 bit sampled once per 8 frames not a bug (design trade-off, state checksum changes with every collection)
- Guard: harness checksum probe J*q (changes plane B, tail bytes, idle counter; red on the old sum)

## 2026-11-12 · lost-session-role-byte · lemon
- Seed: bluespheres_multi.lemon pickView/ownRunner read u8[0x8fff00] which the engine sets to 0 when the session ends mid-stage
- Broken assumption: the online role byte stays valid while the stage runs
- Search: `rg -n '0x8fff00' sonic3air_online_stable_source/Oxygen/sonic3air/scripts --glob *.lemon`
- Siblings: BSMulti.decide reads it only at the stage start not a bug
- Guard: unit check bs unit role (effectiveRole)
