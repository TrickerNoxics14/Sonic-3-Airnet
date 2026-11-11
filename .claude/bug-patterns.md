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
