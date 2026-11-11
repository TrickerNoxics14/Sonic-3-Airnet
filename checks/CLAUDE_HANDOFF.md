# Claude handoff: Drop-In Co-op feature work

Updated: 2026-11-10

## User request

Implement these local co-op features: Team combo meter, Act blooper cards, Buddy launch, Boss victory pose, Ring insurance, Rescue bounce, and Teamwork awards. Keep this handoff updated with every implementation change so the user can give it to Claude. Work local-first; online is a later phase. Do not launch the game or ask the user to test. Do not publish.

## Changes so far

### Change 1 — feature toggle groundwork

- Added `option.dropin.team_features` to `DropInCoop/scripts/common.lemon`.
- Added the default-on “Team Combos, Insurance and Celebrations” gameplay setting in `DropInCoop/mod.json`.
- Added shared feature state/helpers in `teamwork.lemon`: team combo/timer, party actor lookup, total team ring tracking, act-end/boss transition state, a 90-frame boss victory pose window, and overlay text for combo/boss/act cards.
- The features still need to be connected to update/render hooks and to the rescue, launch, and hurt paths. The insurance charge bank is not yet awarded or spent.

### Change 2 — shared teamwork state and celebration logic

- Added a ring-earned insurance meter (one charge per 50 rings, maximum three) and card/stat state.
- Added transition handling for boss victory and `level.results_done`, including a temporary WIN_POSE state for living players and timed team card text.
- Added combo/card rendering helper. It is not yet called by the render path.
- These changes have not been compiled or tested in-game.

### Change 3 — rescue bounce, buddy launch, combo and overlay wiring

- Existing Down+Y rescues now bounce the rescued player upward and award a rescue stat/combo.
- Added a four-controller Up+X buddy launch when a grounded teammate is within 36 horizontal / 40 vertical pixels; cooldown is 45 frames, and launch awards combo/stat credit.
- Connected per-frame feature state and text overlay drawing to the existing game-object update hook.
- Boss pose, act card, and combo text are currently wired. Insurance accounting exists, but spending on hurt and the reset/initialization details remain to be checked.
- Static source inspection only; no game launch or runtime test.

### Change 4 — insurance spending helper

- Added a shared `DropIn.spendTeamInsurance()` helper. It spends one charge, increments the act's insurance-save count, and awards combo credit.
- The helper is not yet wired into the player hurt handlers; that is the next patch.
- Static inspection only; no game launch or runtime test.

### Change 5 — insurance on zero-ring hits

- P1/P2 now receive 10 rings before their existing hurt logic when they have zero rings, no shield/object protection, and the team has an insurance charge.
- Players 3/4 use the same charge path while preserving their separate ring values.
- Still requires script compilation and in-game confirmation of the party ring aliases and shield conditions.

### Change 6 — network setting sync, startup reset, party rescue bounce

- At this point `team_features` was temporarily forced on by the online settings block; Change 10 below removes that override to keep this pass local-first.
- New combo, insurance, launch and pose state resets in mod `Init()`.
- Party-player rescue now uses the same upward rescue bounce and combo/stat credit.
- All seven requested feature paths now have a first implementation, but source compilation and runtime verification remain outstanding. The overlay is local-only for now; online work remains deferred.

### Change 7 — run feature logic for party-only sessions

- Moved team feature ticking to the shared post-party update hook so it runs with players 3/4 even if player 2 has not joined.
- The combo/card overlay now shows for a 3–4 player party without requiring the P2 join flag.
- The controller loop now also permits the party-only session to use buddy launch, while keeping P2 rescue/regroup/Tails carry gated on P2 being joined.
- Static inspection only; no runtime or compile validation.

### Change 8 — clean initialization and split card placement

- `Init()` now clears the card ID and forces the per-act tracker to initialize for the first act.
- Split-screen act cards are moved into the upper view instead of sitting across the divider.

### Change 9 — in-game setting labels

- The settings page now calls the existing hold Down+Y action “Rescue Bounce” and documents Up+X as the Buddy Launch input.

### Change 10 — keep this pass local-first

- New combo, insurance, launch, bounce and victory-pose behavior is now gated off during online play; the shared online fork work remains deferred for a later phase.
- The online settings force block no longer overrides this new local-only toggle.

### Change 11 — insurance HUD readout

- The local co-op overlay now shows the number of banked ring-insurance charges.

### Change 12 — shared monitor power-ups

- Added a local hook to clone positive native monitor effects to each other active player, using the game’s own item-effect routines for rings, extra lives, speed shoes, shields, and invincibility.
- Robotnik/damage monitors are deliberately not shared.
- The zero/static monitor type is also excluded because the base game treats it like Robotnik.
- Third/fourth-player monitor collision now works when those players are active, even if P2 did not join.
- Runtime behavior and dynamic-object capacity still need verification.
- Corrected the share guard so an unjoined P2 does not count as a recipient; the team pickup card appears only when at least one clone effect is actually created.

### Change 13 — coordinated boss-hit phases

- Added a local team boss phase: two distinct active players must damage a boss within 300 frames; the pair earns combo and a “TEAM BOSS PHASE!” card.
- The logic observes a boss hit-count decrease during character/object collision and does not modify boss health or timing.
- This needs coverage across bosses because collision proxies and hit counters vary by boss.
- A completed boss phase now also qualifies for a “BOSS BUDDIES AWARD!” act-end card.

### Change 14 — character-specific traversal controls

- Sonic's Right+X partner move now gives a nearby grounded teammate a forward/upward dash.
- Knuckles' Down+X move gives a nearby grounded teammate a stronger upward/forward bounce.
- The main Tails slot keeps the existing Up-to-carry / X-to-release move; an extra Tails actor can use Up+X for a stronger lift bounce.
- Updated the setting label to show the character controls. These moves still use the common `team_moves` setting and local-only feature gate.
- The `team_moves` setting now gates these new Sonic/Knuckles moves as well as Tails carry.

### Change 15 — support Tails in party slots

- A Tails-based player 3/4 can use Up+X for a stronger teammate lift; P2 keeps the original carry/release controls to avoid a conflict.

### Change 16 — clarify move setting text

- Updated the settings label to distinguish Tails carry/release from Sonic and Knuckles' button combinations.

## Existing implementation context

- `DropInCoop/scripts/teamwork.lemon` already implements partner rescue (hold Down+Y), shared checkpoints, boss regroup, and Tails carry/release.
- `DropInCoop/scripts/boss2.lemon` tracks selected boss object lifetimes; `DropInCoop/scripts/split.lemon` calls `DropIn.updateBossTracking()` and `DropIn.updateTeamwork()` each game frame.
- `DropInCoop/scripts/party.lemon` owns players 3/4, separate rings/scores, a basic Down+Y rescue and shared-camera respawn.
- `DropInCoop/scripts/hud2.lemon` draws the split-screen HUD; `DropInCoop/scripts/render2.lemon` calls `DropIn.drawSplitHud()` after rendering local split views.
- Ring insurance covers P1/P2 and extra party actors by restoring 10 rings from a shared charge bank, while preserving party ring-counter swaps. New features are gated off online until a later online phase.

## Work still to do

- Local work is verified; next phase is audit and online implementation/testing. Keep online changes after the local features, as requested.
- Began online parity after the local F17 pass. Network force settings now enable team features, and deterministic shared power-up, boss phase, movement, rescue credit, ring insurance, and team HUD paths are no longer disabled just because `DropIn.net()` is true.
- Post-change D/E/N/R plus local F1–F17 passed: two-player join, no-repair sync, four player slots, reconnect, 250 ms/5% loss, and all focused local features. N8 exercised the shared-monitor clone hook in a four-peer session; all four peers created three recipient effects, with no synchronization repairs. Before package refresh, the only failed check was expected A5 (stale packaged mod).
- Added online harness checks N9/N10 to exercise the production team-launch velocity/award helper and the two-player boss-phase logic in online mode on every peer. Results pending.
- Rebuilt `Sonic3AIR_Online.zip`; package/static/engine checks passed 21/21. Rebuilt `DropInCoop_Local.zip` separately; verified 17 mod files, stable minimum 26.03.28.0, four-player feature support, no engine binary/ROM.
- Official matchmaking stage M1/M2 passed after the package refresh: the host registered a temporary session and a friend joined by code and loaded the game state. The run passed 23/23 including static/package and engine self-tests.
- Keep updating this file in the same patch as each future code change.

## Current implementation summary

- Team combo: rescue, buddy launch, and insurance save award points; combo decays after five seconds without another event.
- Buddy launch: press Up+X near a grounded teammate; the teammate gets launched upward. Applies to the P1–P4 actor table locally.
- Rescue bounce: existing Down+Y rescue now returns a player beside a grounded partner and launches them upward; party rescue follows the same behavior.
- Ring insurance: one charge per 50 team rings collected, up to three charges; a zero-ring hit spends one charge and restores 10 rings. Charge count is displayed.
- Boss victory pose: active players enter a 90-frame win pose when the tracked boss encounter ends.
- Shared power-ups: positive monitors clone native effects to other active local players; harmful Robotnik monitors stay single-player.
- Co-op boss phases: two distinct active players damaging a boss within five seconds trigger combo credit and a boss-team card; this does not alter boss health/timing.
- Character moves: Sonic Right+X dashes a nearby teammate forward/up; Knuckles Down+X bounces one higher; Tails retains the existing carry/release move.
- Act cards: results transition shows a rescue, launch, insurance, low-rings blooper, or general teamwork card.
- Teamwork awards are selected from per-act rescue/launch/insurance stats.
- New behavior is local-only. Online compatibility and synchronization work has not started in this phase.

## Specific review points for Claude

- Shared item effects clone the initialized 0x4a-byte native effect object and replace its recipient address. Check object lifetime/allocation safety and whether every positive monitor type is correctly shared; harmful type 0 and Robotnik type 2 are excluded.
- Boss teamwork detects a decrease in `bossObject + 0x29` after `Character.OnCollisionWithDynamicObject`; boss-specific proxies may not expose hit counts there.
- Character moves are velocity-based: Sonic Right+X, Knuckles Down+X, fallback characters Up+X; Tails uses the existing carry/release path. They have not been checked against every character mod or split layout.
- The latest focused F-stage run loaded the co-op scripts successfully (F1) and passed all existing behavior checks F2–F13, including the Angel Island start camera, separate score/rings, rescue, checkpoint, boss regroup, and Tails carry. It did not directly test the new shared power-up, boss-phase, or character-move mechanics.
- Added shared helper functions used by production code for monitor eligibility and team-move input/velocity decisions; the test harness reports checks for positive versus harmful monitor eligibility, trigger edge handling, character move profiles, duplicate boss hitter rejection, and a partner-completed boss phase. The PowerShell runner now includes them as F14–F16.
- First focused F run loaded scripts, passed F1–F13, character moves (F14), and coordinated boss hits (F16). The monitor eligibility check (F15) failed because its test loop included Robotnik type 2 among positive types; the fixture now excludes type 2 and must be rerun.
- Corrected focused F rerun passed all gameplay checks F1–F16; only A1/A5 failed because the engine and package had not yet been rebuilt.
- Latest P/Q rerun passed all 3-player checks (P3L, P3.1–P3.11), 4-player checks (P4L, P4.1–P4.11), and quadrant checks Q1–Q7. Captured screenshot: `checks/last_logs/Q4-party-quad.bmp`.
- Rebuilt the engine and local release package with `checks/run_checks.ps1 -Build -Package -Quick`. Static/package and engine self-test checks passed 21/21; package contains 825 files and the current mod (17 files, 0 differences), with no ROM or test mods.
- Rebuilt the stock-game local-only mod download separately with `checks/make_local_package.ps1`. `DropInCoop_Local.zip` contains 17 mod files, declares minimum stable game version 26.03.28.0 and four-player support, and includes no engine binary or ROM.
- The full focused run reported 32/34 checks: A1 (engine build newer than C++ source) and A5 (release package matches current mod) failed because the build/package are stale; all other reported checks passed. Rebuild before treating the package as current.
- A subsequent P/Q run passed the 3-player suite (P3L plus P3.1–P3.11), 4-player suite (P4L plus P4.1–P4.11), and four-quadrant suite (Q1–Q7). The captured quadrant frame has visible level pixels in every view. Overall was 50/52; only the same stale-build A1 and stale-package A5 checks failed.
- Replaced an unverified local-pointer `.u16` field access in the monitor clone hook with an explicit `u16(...)` cast during static review.

## Verification status

- Automated game launch was performed by the focused F-stage runner to compile/load scripts and run existing regression checks; no manual test was requested.
- Script loading and existing F-stage co-op regressions passed (F1–F13).
- Focused gameplay checks F1–F17 passed, including monitor cloning and P2-only ring application. The complete focused F run passed 38/38.
- 3-player and 4-player gameplay checks passed; the quadrant split passed all seven automated checks, including independent cameras and sampled visible pixels in each quadrant.
- Engine build and local release package were rebuilt; static/package and engine self-tests passed 21/21.
- Local-only mod ZIP was regenerated and verified independently of the fork package.
- Online parity changes passed D/E/N/R plus focused F tests; N8 confirmed four synchronized copies of the shared monitor clone path.
- Matchmaking registration/join by session code passed M1/M2; combined run passed 23/23.
- Adjusted the four-player screenshot fixture to place P1–P4 farther apart and added a spacing assertion, so the capture is readable instead of showing overlapping characters.
- Reran the quadrant stage after spacing the actors: Q1–Q8 passed, including independent cameras, readable player spacing, all players alive, and visible level pixels in every quadrant (29/29 total for the focused run). Updated capture remains at `checks/last_logs/Q4-party-quad.bmp`.
- Exterminator review: root cause was test-fixture placement (`+0x10`, `+0x20`), not the production camera layout. Sibling search found no other harness with the same offsets; bug pattern recorded in `.claude/bug-patterns.md`. Automated audit tool could not inspect changes because this workspace is not a Git repository; did not initialize Git, per the project's existing constraint.
- Added F17, a deterministic synthetic rings-monitor pickup fixture that invokes the production `ItemEffect.Init` hook, verifies the P2 clone, executes the native effect on that clone, and asserts P2 gains ten rings while P1 and lives stay unchanged. F17 passed; the complete focused F run passed 38/38.
- Local work is verified; next phase is audit and online implementation/testing. Keep online changes after the local features, as requested.

## Current audit update — 2026-11-10

Online parity implementation is now present and tested. Shared monitor effects, teammate launch velocity/award, and coordinated boss-phase logic ran in all four deterministic peers (N8–N10: 4/4 peers each); no sync repairs were reported. The online transport, reconnect, matchmaking code join, and 250 ms / 5% loss suites passed. This was simulated on one computer; different-home NAT and real friend connectivity remain unverified.

Current focused automated runs:
- `-Only A,B,C,F,D,E,N,R,M`: 58/58 passed. Includes current online package contents (A5: 17/17 mod files match), two-player Angel Island intro split, local co-op regressions, code join, matchmaking registration/join, four online slots, reconnect, sync, and lag/loss.
- `-Only G`: 119/119 passed across 24 roaming-act scenes and two boss arenas. Each roaming scene passed split, shared-screen encounter transition, split return, and framebuffer capture. The two boss arena tests passed shared view with P2 active.
- `-Only QG`: 62/62 passed across 24 act checkpoints. Actual four-view cameras and all four framebuffer quadrants were verified in 17 sampled scenes: Marble Garden 1–2, Carnival Night 1, Flying Battery 1–2, IceCap 1, Launch Base 1, Mushroom Hill 1–2, Sandopolis 1–2, Lava Reef 1–2, Sky Sanctuary, Death Egg 1–2, and Hidden Palace. The other seven sampled checkpoints correctly used shared-camera fallback: Angel Island 1–2, Hydrocity 1–2, Carnival Night 2, IceCap 2, and Launch Base 2. Four-view behavior is scene-state dependent; this is not a claim that every segment of every act is four-way split.
- `-Only H`: 67/67 passed for 23 source-fixture boss scenes. The real boss encounter stayed active at least 60 frames, all four slots were active for the required window, the display used one shared view during the encounter, and a framebuffer was captured. These are isolated boss scenes; full act routes and defeating every boss are still unverified. Some scripted actors died later in the 900-frame harness, so the pass proves arrival/party activity and camera behavior during the required window, not indefinite survival.
- `checks/last_logs/QG24-roaming.bmp` is a readable four-view sample; `checks/last_logs/Q4-party-quad.bmp` is the earlier spaced four-player fixture capture.

Both packages were rebuilt after the production-script changes: `Sonic3AIR_Online.zip` and `DropInCoop_Local.zip`. The local ZIP contains only the 17 mod files, advertises four-player support and minimum game version `26.03.28.0`, and contains no engine binary or ROM. The online ZIP passed static package checks, excludes saves/caches/test mods/ROM, and matches the current mod exactly. No manual user testing or second computer was used.

Remaining material gaps: a friend joining from a different home network/NAT, rope/zip-line rendering, player-two-opened monitor behavior, full routes into every boss, and exhaustive visual review of every camera position/transition. The current report file is from the most recent H run (67/67); the independent run counts above are preserved here.

Boss-scene audit completed after the update above: `-Only H` passed 67/67 for all 23 fixtures. Each scene ran the boss encounter for at least 60 frames; the party was fully active for at least 30 encounter frames; split was off during the measured encounter; and a framebuffer capture was saved. This verifies isolated arena behavior, not full act traversal or winning the fight. A few scripted extra actors died later in the harness, so the active-window assertion is the scope of the pass.

## Follow-up diagnostic — four-view water fallbacks

Added two developer-harness diagnostic lines to the QG act test for zone/scroll/boss/water and renderer/camera state. Reran the seven fallback checkpoints (`QG01,QG02,QG03,QG04,QG08,QG12,QG14`): 28/28 passed. Every one reported `zone 1 scroll 1 boss 0 water 1`, with four-view engine support available; they fall back solely because the active scene has water. This confirms an intentional renderer limitation rather than a broken camera branch. The diagnostics are in `checks/harness/ActHarness/scripts/main.lemon` and are excluded from both release packages.

## Follow-up regression — player-two monitor collision

Added F18 to exercise the production `MonitorIntact.CheckBlocking.player2` hook and `Character.OnCollisionWithDynamicObject`: a rolling P2 is allowed through the blocking test, breaks the monitor fixture, and is recorded as its effect owner; the temporary Competition-mode bridge is restored. F18 passes and the full focused F run is 39/39. The first harness draft used a nonexistent Lemon `player2.state` alias and failed compilation; changed it to the documented A0/`char.state` access, searched the harness/mod for sibling aliases (none found), then reran successfully. `checks/CHECKLIST.md` now states the scope precisely: this isolates real hooks but is not yet a monitor placed in ordinary level terrain.

The seven four-view fallbacks are all water scenes, by design. No production camera change was made because the global waterline/palette effect cannot safely provide four independent waterlines yet.

## Follow-up diagnostic — Angel Island zip-line view (2026-11-10)

Added an automated Z stage to `checks/run_checks.ps1` and a focused ActHarness reproduction. It starts AIZ1, joins P2, holds Sonic at x=0x27c8 while Tails is centered at the authored zip line around x=0x2200, then checks the real `AIZZipLine.BaseUpdate` object slots for segments marked visible within P2's camera bounds and outside P1's. It also requests an engine framebuffer capture. Run with `checks\run_checks.bat -Only Z`. This is a test-only change; no production DropInCoop or engine behavior has been modified yet. The check has not run, so the rope disappearance remains unconfirmed and unfixed pending the diagnostic result.
Harness wiring follow-up: the generic ActHarness completion assertion is bypassed only in ZIPLINE_TEST mode, so the Z-specific object/render assertion is reached instead of being pre-empted by the regular act check. No production code changed.

Zip-line reproduction setup correction: the first focused run showed that moving Sonic out of the intro at runtime left the AIZ scripted camera transition active (`camera.update_routine=04`, `scroll=08`), which prevented split activation and caused the test to inspect an invalid scene. The Z runner now initializes Sonic at the intended roaming position during level setup, before camera and object streaming initialization; the P2-only object assertion remains unchanged. The first failure is retained in `last_report.txt` / `last_logs/Z1-logfile.txt` as fixture evidence.

Zip-line harness correction 2: direct start-position teleport caused an engine memory-range error before P2 joined, so that setup is removed. The runner now uses the same in-level navigation controls as the act audit, lets Sonic naturally reach the far-side checkpoint, then relocates only P2 into the authored vine area while ordinary AIZ split/object streaming is active. The original production assertions are unchanged. This is still a diagnostic harness change; rerun pending.

Zip-line harness correction 3: natural AIZ navigation was still at x=0x2175 near the old x=0x27c8 stop point by frame 2970. The stop/camera separation target is now x=0x2500, just beyond the vine's P1 view, and the check endpoint is 4000 frames. This keeps the player on a natural route while giving the scripted checkpoint time to reach the scene. The interrupted run did not produce a verdict; rerun pending.

Zip-line harness correction 4: the natural route reaches x≈0x228c near frame 4000 with split active. The Z fixture now freezes Sonic immediately after the rope at x≥0x2200, moves only P1's camera to 0x2400, and centers P2's camera on the authored zip line (0x2200). This makes the rope outside P1's view while keeping ordinary two-view streaming active. No production behavior changed; focused Z run pending.

Zip-line harness correction 5: the previous framebuffer had a manually forced P1 camera and was visually corrupted; it was not valid evidence. The forced P1 camera is removed. The runner now lets both normal AIZ camera tracks follow the actual actors, continues the harness route until Sonic naturally reaches x≥0x2500 (past the vine), then places only P2 at the vine and captures the real split view. Endpoint increased to 5000 frames. The prior test failure was a fixture artifact, not evidence of a production defect.

Zip-line harness correction 6: the prior natural-run route had not reached x=0x2500 by frames 4000–5000, so no valid result was produced. The fixture now waits until Sonic naturally passes x=0x2200 (the rope root), positions only Tails on the rope, and advances P1's camera gradually by four pixels per update to x=0x2280. This avoids the abrupt camera jump that corrupted the earlier capture. The endpoint is 4300 frames. The production assertion remains the same and still requires a real loaded/visible segment specifically in P2's view.

Zip-line harness correction 7: the normal route reaches the checkpoint around frame 4200, leaving only 70 frames for P2's object window to settle before capture. Stage Z now captures at frame 4500, allowing roughly 270 frames of ordinary second-window updates after the rope view is established. A valid verdict still requires active split and a visible segment within P2 but outside P1; this change only lengthens setup time.

Zip-line harness correction 8: with P1 near x=0x2200, jumping P2/camera directly back to the rope did not load a rope object in 270 frames. The harness now freezes P1 near x=0x1e00, then moves only P2 and P2's camera gradually toward the vine in four-pixel steps. This gives the second object window a normal streaming path from nearby coordinates and keeps P1's camera untouched. This corrects the fixture's camera/object-manager transition; no production code changed.

Zip-line harness correction 9: the latest framebuffer had two loaded AIZ vine segments, but the test camera y=0x0150 put the vine below the 112-pixel P2 viewport. The generated image showed scenery but no rope, confirming a vertical fixture misalignment. P2 camera y is now 0x01c0, which places the vine at y=0x01e0 and Tails at y=0x0220 inside the lower view. Next run checks actual P2 visibility.

Zip-line harness diagnostic update: the last natural-camera capture shows Tails inside the lower view and two vine segment objects in memory, but neither qualifies as visible in P2. Added a one-object sample to log its world coordinates, render flags, and horizontal offsets from each camera. The route trigger is lowered to x=0x1d00 because the harness stalls there reliably; this is well before the zip line and keeps Sonic's camera away. The next run distinguishes camera-bound failure from object render-flag/update failure.

Zip-line diagnostic follow-up: the object sample from the last run was at x=0x1e68/y=0x0375, a different AIZ vine object, not the zip-line root at x=0x2200/y=0x01e0. The harness currently counts every shared `AIZZipLine.BaseUpdate` use, so its segment count was a false positive. Added final P1/P2 coordinates and both camera coordinates to distinguish a second-camera positioning failure from missing target objects. No production change; target-object filtering will be applied once these values are confirmed.

Zip-line harness correction 10: confirmed `BaseUpdate` is shared with an unrelated AIZ vine at x=0x1e68, which caused a false segment count. The scanner now filters to the authored target around x=0x2200/y=0x01e0–0x0240. For a deterministic second-camera-region setup, the harness places P2 at the rope once and invokes the production `DropIn.resetView2()` helper so the real camera and object-window initialization recompute from that position. P1 stays at natural x=0x1d00. This exercises the target object loader/render path; it does not prove normal travel to the rope yet.

Zip-line harness correction 11: after the one-time reset, Tails' normal physics moved him past the vine (`P2 x=0x256c`, camera x=0x24a1), leaving the test pointed beyond the target. The fixture now reapplies the stationary P2 test coordinates and clears both velocities each update, while keeping the camera/object-window reset one-time. This makes the sample truly stay centered on the vine for the full rendering interval.

Zip-line diagnostic correction 12: after resetting the secondary object window, P2 and camera coordinates are correct but the target zip-line still has zero matching segments. Added a scan for *any* active object at the authored rope coordinates, plus the second object window's table pointer and address range. This distinguishes failure to spawn the level object from a renderer visibility problem. Still no production fix until that root cause is observed.

Zip-line diagnostic correction 13: the target-region scan finds only Tails, and the vine is absent. Added a lookup for the exact authored AIZ1 table record (x=0x2200, y=0x01e0, object 0x06, subtype 0x3b) and whether the second dynamic-object window's current table range includes that record. This will determine if the gap is in object-table range advancement or later object initialization.

Zip-line diagnostic correction 14: the exact root record is inside P2's object-table range, but no matching object is active. Added its respawn byte (`0xffffeb00 + object index`) to determine whether the level loader marked the vine as spawned without leaving the object alive. This is the last diagnostic before deciding whether the shared respawn bookkeeping is the production cause.

Zip-line harness correction 15: confirmed why the window had the wrong range. The harness reset `om2` during the object-update hook, but the next object-loading pass runs before the camera-update hook; it initialized the window from P2's stale prior camera coordinate (table end at x≈0x2500), then the camera moved to 0x2138. Setup now seeds P2's actor and camera at the target before calling `DropIn.resetView2()`, so the next real object-loading pass initializes the correct range. No production code changed.

Zip-line diagnostic correction 16: the secondary window includes the exact vine record, but the respawn flag is still zero. Added the count of free slots in the engine's standard dynamic-object allocation range. This separates a full object pool from the window failing to iterate/spawn the record. All changes remain in the test harness only.

Zip-line diagnostic correction 17: the standard allocator had 59 free slots, so pool exhaustion is excluded. The target record's respawn byte remains zero, indicating its spawn attempt was skipped before allocation, most likely by the loader's vertical camera bounds. Added P2 camera and character Y values to the diagnostic so the final run can confirm whether the rope's authored y=0x01e0 fell outside that window.

Zip-line run result (2026-11-10): Z1 remains FAIL, but the scene is confirmed split and the framebuffer capture is valid. At capture P1/P2 x are 0x1d4e/0x2200; P2 camera is 0x2140/y=0x01f0 and character y=0x0220. The authored rope table entry exists inside the second window's range, but there are zero active objects at rope coordinates, its respawn byte remains 0, and the standard allocation range has 59 free slots. Thus wrong target coordinates, vertical viewport bounds, and a full object pool are ruled out. The second-window iterator still appears to be skipping the in-range rope entry; root cause not yet proven. No production fix or package rebuild has been made. The Z1 framebuffer is at `checks/last_logs/Z1-zipline.bmp`; log is `checks/last_logs/Z1-logfile.txt`.

Zip-line fixture correction 18: removed direct P2 actor/camera teleport and the forced Tails control override. The Z harness now stops Sonic before the rope and steers Tails there with normal pad input, then waits 60 frames before checking the second object window and framebuffer. This makes the regression exercise actual camera tracking and object streaming. Production mod remains unchanged; rerun checks/run_checks.ps1 -Only Z before drawing conclusions.

Zip-line fixture run 18 (2026-11-10): Z stage timed out before P2 reached the target; Z1 and Z1V failed because the fixture never completed, so this run says nothing conclusive about production zip-line behavior. The prior screenshot was produced by an invalid teleported setup; removed. Current valid evidence: regular split checks remain green, but the dedicated AIZ rope regression has not yet been made reliable. No production code or release package changed.

Zip-line fixture correction 19: bounded the natural-movement attempt at the configured act endpoint. If Tails does not reach the target, the harness now emits a diagnostic failure with his position/AI state, captures the final frame, and exits instead of leaving the game running until the external timeout. Test-only change; production remains untouched.

Zip-line fixture correction 20: automated P2 movement reached x=0x1bf5 but fell into the AIZ water before the target; the fail-fast check now reports this instead of timing out. Added periodic jump pulses to the dev harness to clear terrain and reach the authored rope without teleporting. Re-run Z before changing production code.

Zip-line fixture correction 21: P2 reached x=0x22a3 but remained at y=0x0496, below the rope and outside its vertical spawn band. The harness now continues steering upward (and gives jump pulses) until Tails enters the rope's vertical region, and only then starts the render check. No production changes.

Zip-line fixture run 21 (2026-11-10): Tails advanced to x=0x22ae but stayed at y=0x0445 despite up/jump input; the target root at y=0x01e0 is outside P2's current camera window, so the object loader correctly never attempts it. Z1 fails because the harness cannot reach the upper route; this is not evidence of a production loader defect. The direct-placement experiment remains the only way to exercise this exact table entry so far, but its camera/scene setup needs a controlled valid-state fixture before using it as proof. No production fix or package rebuild.

Zip-line harness revision 22: replaced the unreliable natural route with a bounded, controlled second-view setup at the authored vine coordinates. Added a dev-only wrapper around the engine's conditional object spawn routine, recording whether the exact AIZ1 table entry is reached, its vertical bounds, Y-check/wrap flags, result, and respawn byte. No production mod files changed; this directly tests the root cause rather than inferring from the final object list.

Zip-line engine trace addition 23: the spawn wrapper confirmed the exact vine record is accepted and sets respawn flag 0x80, yet the final object scan is empty and the respawn flag later returns to 0. Added a test-only wrapper for AIZ's fn021f38 cleanup path to record the vine's x/y, despawn distance, active update address, and respawn byte at cleanup. This should distinguish actual culling from a later object-slot overwrite.

Zip-line cleanup trace result 23: the rope spawn succeeds (respawn eb65 becomes 0x80); its first AIZ cleanup call sees x=0x2200, despawn x=0x2080, distance 0x0180 (within the 0x0280 lifetime), and leaves it active at update 0x021d28. This rules out initial Y filtering and an immediate distance cull. Adjusted the test trace to catch only a later out-of-range cleanup, which would explain the subsequent missing object and cleared flag.

Zip-line root cause in test fixture 24: the rope's own cleanup trace showed it remained active at x=0x2200 (distance 0x0180 from its P2 despawn origin), then carried Tails/segments to x=0x2380 where distance became 0x0300 and correctly unloaded. The harness caused this by forcing Tails back to x=0x2200 every frame, preventing P2 camera tracking. Removed the repeated actor/camera-control override; only seed Tails once, then let normal movement and camera logic run. This is a test-fixture correction, not a production fix.

Zip-line fixture correction 25: with the repeated P2 teleport removed, diagnostics show the root remains spawned (respawn flag 0x80) while Tails and the camera follow its movement from x=0x2200 to x=0x23a9. The old assertion only searched within ±0x80 of the authored spawn coordinate, so it missed the moving vine. Updated the scan to count the live BaseUpdate segment and test its actual P1/P2 viewport positions and visible flag. Production code unchanged.

Zip-line regression fixed 26 (2026-11-10): the controlled AIZ1 test now seeds Tails at the rope only once, then lets normal rope movement and P2 camera tracking continue. The earlier failure was a harness artifact: per-frame position forcing kept the camera behind a moving rope, which triggered the game's normal unload rule. Updated Z documentation accordingly. checks/run_checks.ps1 -Only Z passes 23/23 total checks (Z1 and Z1V pass). The captured checks/last_logs/Z1-zipline.bmp visually shows the vine in P2's lower view while P1 remains in the upper view. No production code or release package changed; no online fork work has started in this turn.

Four-view screenshot finding 27: the Q4 visual harness overwrote P2/P3/P4 positions after every game update to keep them in a tight 80px chain. That made the screenshot look crowded and forced movement deltas to zero; it was not a faithful four-player gameplay capture. Removed the per-frame position rewrite so the existing controller inputs and independent camera updates determine the picture. Next run Q4 to reveal any real movement/tether issue. No production code changed.

Quad capture refinement 28: after removing the forced-position fixture, the remaining visual crowding was P1 and P2 genuinely sharing their initial spawn location; the Q harness had never moved them. Added brief opposing P1/P2 inputs in the quad-only capture run. This is test-only stimulus for a readable four-view visual regression; production inputs remain untouched.

Quad graphics fix 29: Q4 capture reproduced tile/scroll corruption in the top row. Root cause: prepareView2() writes to shared RAM at 0xffffe000; calls for P3/P4 overwrote P1/P2 line-scroll data before setupFourViewPlanes() consumed it. Added dedicated P1/P2 scroll snapshots and Q9, an automated per-view buffer-distinctness check. This is the first production mod change for the four-view visual bug. Rebuild/re-run Q before considering it fixed.

Quad capture refinement 30: the first post-fix Q capture still placed P3/P4 only 20px apart, making their sprites overlap. Extended the quad-only input window so P3 moves right and P4 left for longer before capture; normal 3/4-player behavior tests retain their original input intervals. This remains harness-only. Re-run Q to verify spacing and inspect the corrected framebuffer.

Quad rendering verification 31 (2026-11-10): Q1-Q9 pass after the per-view scroll snapshots and capture-input adjustments. Q reports 29/30 overall because A5 compares the old bundled online package against the changed source; Q itself has all required quad checks passing. The online fork/package was intentionally left untouched for the user's local-first, online-last order. Rebuilt DropInCoop_Local.zip with checks/make_local_package.ps1; verified 17 files, stable minimum version 26.03.28.0, and no engine binary or ROM. Updated CHECKLIST.md with the Q9 scroll-data contract. Latest engine capture: checks/last_logs/Q4-party-quad.bmp.

Boss audit correction 32 (2026-11-10): Found that H could report PASS without requiring four live actors at capture, and that the fixture's continuous run controls let actors leave the arena. H now requires a real boss encounter, at least 60 encounter frames, 30 consecutive frames with all four active, no split during the encounter, and a capture where four players are alive, inside the shared camera, and have visible render flags. Removed test-only 120-frame P1/P2 invulnerability that made the base renderer blink Sonic/Tails off during captures. The boss fixture holds the four players at spaced camera-relative positions after the boss starts; this is scene-render validation, not movement/combat or route validation. The Lava Reef standalone boss scene required the same camera-relative staging while its vertical-scrolling scene initializes. Re-ran `checks/run_checks.ps1 -Only H`: all 23 boss scene checks and 23 framebuffer checks passed (46/46). Visually inspected current H08, H10, H14, H18, H19, H20, and H23 captures; all four characters are visible on one shared view. Overall report is 66/67 because A5 still flags the intentionally stale online bundle; online remains deferred. Updated CHECKLIST.md with the exact boss-test scope. Latest captures are in checks/last_logs/Hxx-boss.bmp.

## Stable local and online completion — 2026-11-10

Local co-op verification completed before the online pass. The all-local audit ran the three/four-player controls and split-screen stages, all 24 act roaming/shared-boss camera cycles, 23 shared-screen boss scenes with framebuffer checks, and the AIZ P2 zip-line view. Functional stages P, Q, F, H, G, QG, and Z passed; the earlier aggregate was 258/259 because the online ZIP was stale at that point. The fresh local-only ZIP was rebuilt again after online work: 17 files, stable minimum 26.03.28.0, no engine executable or ROM (`DropInCoop_Local.zip`). The four-view capture is `checks/last_logs/Q4-party-quad.bmp`.

The stable online fork now builds from official stable 26.03.28.0. Fixed per-process main-log naming (`S3AIR_LOGNAME`) so simultaneous instances do not overwrite each other's logs. Ported the outdated compiled-script fallback so a stale `saves/scripts.bin` is logged and source scripts load instead of opening the blocking error dialog. Added opt-in `Checks.report` and `Checks.capture` developer bindings to the stable engine, with no test harness included in the release.

Fixed the online test harness to access mod-private team globals through the named-global API, use the correct option variable names, and enable teamwork options for the fixture. The main runner now sets only DropInCoop active during ordinary D/E net tests and loads DropInCoop before NetPartyHarness.

`checks/run_online_backend_checks.ps1` is now the single online audit command. It runs code/fingerprint/LAN tests, builds the stable fork, builds and guards the online package, then runs D/E/M/N runtime checks. Final result: ONLINE BACKEND CHECKS PASS; runtime checks 18/18. This covers same-machine code join, public matchmaking registration and code join, 250 ms / 5% loss at 60 fps without repairs, four peers, player-three reconnect, and synchronized teamwork monitor/move/boss-phase behavior. Both peers in M ran on this computer; different home networks/NATs remain unverified.

The online package is `Sonic3AIR_Online.zip` (stable fork, ROM excluded, save/cache/test-mod guards pass). The local package remains the separate mod-only ZIP. Nothing was published.

## Detailed continuation handoff — 2026-11-10 (latest authoritative supplement)

This supplement records the full recent local-audit and stable-online work. Keep the earlier history above; its intermediate FAIL/pending entries are superseded only where a later result explicitly resolves them. Paths below are relative to `<workspace>`.

### User requirements to preserve

- Local multiplayer is a mod-only download; online uses the separate fork. Target the chosen official stable base 26.03.28.0, not the newer test build.
- Up to four local players; split for roaming, shared view at bosses/special scripted scenes. Keep each player's controls, score and rings independent.
- Use the exterminator skill for bug work: reproduce, identify the cause, search related cases, and retain meaningful regressions. It was used for the visual and audit failures in this work.
- Automate verification; do not ask the user to play through acts or use another laptop. Do not claim that local multi-instance tests prove separate-home connectivity.
- Update this handoff after changes. Do not publish to GameBanana yet. Do not initialize git without asking. Do not add the ROM to releases. Keep third-party character files out of our release. Do not run a permanent public server on the user's PC. No subagents were used in this work.

### Local production change and visual harness changes

`DropInCoop/scripts/split.lemon`: the real four-view corruption came from shared line-scroll memory at 0xffffe000. P3/P4 preparation overwrote P1/P2 data before the four-view planes consumed it. Dedicated snapshots preserve each top-row view's scroll state. Q9 checks the separate buffers. This production fix was already in the local package before starting the stable-online phase; no further production DropInCoop script changes were made during that phase.

The PartyHarness quad fixture previously rewrote actor positions every frame, making movement deltas zero and crowding the picture. That rewrite was removed. Brief opposing P1/P2 inputs and longer opposite P3/P4 inputs make the capture exercise ordinary movement/camera tracking. These input adjustments are test-only.

The AIZ zip-line investigation ended as a fixture correction, not a confirmed new production loader fix. Repeatedly pinning Tails to the spawn coordinate kept his camera behind the moving rope, causing normal distance cleanup. The final Z fixture seeds Tails once, then lets the rope and camera move naturally. It tests the live moving segments in P2's view while outside P1's view. Z1/Z1V passed, with evidence in `checks/last_logs/Z1-logfile.txt` and `Z1-zipline.bmp`.

### Boss audit correction and actual scope

The old H fixture could accumulate 30 party frames across interruptions and report success without four live actors at capture. It also continuously drove extra players right out of the arena. The corrected fixture requires a real encounter, at least 60 encounter frames, 30 consecutive four-player-active frames, no split during the encounter, and all four actors alive/in view/render-flag-visible at capture. Test-only P1/P2 invulnerability was removed because the base hit-flash renderer hid them in captures. The boss fixture stabilizes actors at spaced positions in the shared camera, including the Lava Reef vertical boss scene.

All 23 boss-scene checks and 23 framebuffer checks passed. H08/H10/H14/H18/H19/H20/H23 captures were visually inspected. This verifies isolated scene/camera/render setup. It does NOT verify traversal from the start of each act, full combat, defeating each boss, or flawless artwork in every frame. The camera-relative actor staging is a fixture, not normal production movement. Framebuffer pixel-count checks establish rendered regions; they are not a complete image-quality oracle.

### Local verification evidence

Full command executed before the online package migration:

    .\checks\run_checks.ps1 -Only P,Q,F,H,G,QG,Z

Result at that time: 258/259. The only FAIL was A5 comparing the then-stale online bundle against the changed source mod. All selected functional stages passed: P (3/4 actors and mechanics), Q (four views including Q9), F (co-op mechanics and intro), H (23 boss fixtures), G (act/camera transitions), QG (four-player act scenes), Z (P2 rope view). Do not silently rewrite that historical report as 259/259; it remains in `checks/last_report.txt`.

Important engine distinction: the default `run_checks.ps1` source is still `sonic3air_source`. The stable ONLINE runtime checks explicitly passed `-EngineRoot .\sonic3air_online_stable_source`. Do not describe the entire historical local sweep as a clean official-stock-stable sweep on the basis of its minimum-version metadata alone. Earlier stable tests are documented above, but this particular broad command used the default source tree.

### Stable fork fixes, with files

Engine root: `sonic3air_online_stable_source`.

- `Oxygen/oxygenengine/source/oxygen/application/EngineMain.cpp`: supports `S3AIR_LOGNAME` suffixes for the main logfile. The port previously left every instance writing logfile.txt while the audit waited for logfile_host/friend/partyN. This caused missing/stale evidence and false failures. Separate main logs now match the already-separated online logs.
- `Oxygen/oxygenengine/source/oxygen/simulation/bindings/LemonScriptBindings.cpp`: implemented and registered `Checks.report(name, passed, detail)` and `Checks.capture(label)`. The missing report binding caused the user's `Unknown function name 'Checks.report'` dialog. Report lines use `CHECK|name|PASS/FAIL|detail`. Capture is enabled only by `S3AIR_CHECK_CAPTURE_DIR` and `S3AIR_CHECK_CAPTURE_PREFIX`; labels are restricted to letters, digits, underscore and hyphen. It saves the engine framebuffer as BMP, avoiding desktop overlay interference.
- `Oxygen/sonic3air/scripts/_reference/cpp_core_functions.lemon`: declared those two native functions.
- `Oxygen/oxygenengine/source/oxygen/simulation/LemonScriptProgram.cpp`: an incompatible compiled binary now logs `Compiled script cache is outdated, loading the scripts from source instead` and returns failure to the existing source fallback, instead of opening the blocking `Failed to load 'scripts.bin'` modal. Adding native bindings invalidated the old saves/scripts.bin dependency hash and reproduced that problem during testing.

The stable base still does not contain the old `S3AIR_SELFTEST` startup hook. RuntimeOnly mode skips that hook; real code-level tests are run separately by the online wrapper. It was not ported in this work.

### Online harness and runner fixes

`checks/harness/NetPartyHarness/scripts/main.lemon`:

- Direct references to private teamwork globals caused `Unable to resolve identifier: dropin.team_card`. Added typed get8/get16/set8/set16 wrappers around System's named-global API.
- Lookup names must exactly match the declared identifier: `dropin.team_card`, `option.dropin.team_features`, etc. The initially attempted `global.dropin.*` / `global.option.*` names were wrong and silently returned zero. Those attempts were corrected.
- Frame-one fixture setup enables team_features and team_moves. Without those settings, the option-dependent assertions failed despite four connected peers.
- Tests save and restore the production state around monitor cloning, teammate launch/award, and coordinated boss-hit probes.

`checks/run_checks.ps1`:

- Added `-EngineRoot` to select a source tree and `-RuntimeOnly` to skip the legacy static/package checks and old self-test startup hook.
- With both switches it writes `checks/online_stable_runtime_report.txt`.
- Invoke-OnlineJoin sets only DropInCoop active, preventing a prior N harness from contaminating later E/R-style tests.
- N activates DropInCoop before NetPartyHarness. The named-global access fix is also required; load order alone did not resolve private variables.
- Existing mutex/config backup/active-mod restore remain in use. Interrupted runs left `.run-recovery.json`; subsequent runs recovered the saved configuration. Final completed audit restored configuration and left no recovery file or Sonic3AIR process.

`checks/run_online_backend_checks.ps1` now orchestrates code tests, stable Release x64 build, stable package construction/content guards, and D/E/M/N runtime stages in a separate PowerShell process. It consolidates PASS/FAIL lines into `checks/online_backend_report.txt`.

### Final online verification and its limits

Run:

    powershell -NoProfile -ExecutionPolicy Bypass -File checks\run_online_backend_checks.ps1

Final execution succeeded with `ONLINE BACKEND CHECKS PASS`. Runtime: 18/18. Core tests cover code encoding/invalid inputs/typing mistakes, mod-file fingerprint sensitivity and enumeration ordering, unreadable-vs-empty files, and LAN session discovery/mismatch. The stable Release x64 build succeeded.

D: code discovery, loaded snapshot, 15 seconds with zero repairs, wrong-code termination all passed.
N: players 2/3/4 assigned distinct slots and snapshots; all four real input slots routed correctly; player three reconnected while player four kept its input slot; no observed sync repair/script error. Shared-monitor clones, launch velocity/award, and coordinated boss phase each reported PASS in 4/4 peers.
E: simulated 250 ms delay plus 5% packet loss; joined, maintained minimum steady-state 60 fps in this run, and needed zero repairs.
M: public server accepted the temporary host registration and the friend loaded the game using the session code. Qualification: the friend log also says `found the host in the same network`, so LAN fallback was active. M2 passing does NOT establish that the client obtained the host exclusively through the public rendezvous response. Separate-home NAT traversal, relay availability, and real friend connectivity remain unverified. Earlier wording implying an exclusively public matchmaking path should be read with this qualification.

Intermediate N failures were useful reproductions of missing logger/bindings, private-global compile errors, invalid dynamic names, and unset fixture options. They were followed by successful reruns; do not treat the early FAILs as unresolved production four-player transport failures.

### Packaging, documentation, and exact artifacts

`checks/make_package.ps1` now takes the engine/data/scripts from `sonic3air_online_stable_source/Oxygen/sonic3air`, rather than the older `sonic3air_source` tree. It sanitizes StartPhase/LoadSaveState/LoadLevel and bundles only DropInCoop. Added staged-file and ZIP-entry guards for saves/states, named harnesses, ROM-like filenames/extensions and large .bin files. These are heuristic content guards; they are not an asset-license audit.

`checks/package_files/README_ONLINE.txt` now describes the stable 26.03.28.0 fork, up to four computer/player slots, default Sonic/Tails/Knuckles/second-Sonic roles, optional configured ESU character, and joining via the shared code.

`checks/CHECKLIST.md` documents the boss fixture's scope, Q9, and the single online audit command. `checks/GAMEBANANA_COMPATIBILITY.md` records stable runtime progress and separate-network limitations. Third-party page review does not prove every character/zone mod compatible; no new third-party character integration or redistribution was done during this final phase.

Artifacts at final check:

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| DropInCoop_Local.zip | 55,907 | 0E6B34FB9E6D387648D40BC4EA39DDB152F4974929522343AAE68289736AC0BD |
| Sonic3AIR_Online.zip | 140,299,763 | 84E01B8141A39393FD292D512038F17D52B50D733F5F20965B5D6F14018E3989 |

The local ZIP was independently rebuilt/verified again at the end: 17 mod files, minimum 26.03.28.0, FourPlayers declared, no engine/ROM. The online ZIP includes the stable fork and engine data, excludes the ROM, saves and development test mods. No files were published or sent to friends.

Evidence: `checks/online_backend_report.txt`, `checks/online_stable_runtime_report.txt`, historical `checks/last_report.txt`, and `checks/last_logs/` (D1/D4/E1/M logs; N4-player1.txt through N4-player4.txt; N4-online-party.bmp; Q4-party-quad.bmp; Hxx-boss.bmp and logs; Z1-zipline.bmp and log). The four-view capture was displayed again in the final reply; that display is not a new gameplay run.

### Known continuation issues and safe next steps

1. Preserve the explicit stable EngineRoot when running online checks. The legacy general runner still defaults to sonic3air_source, but make_package.ps1 now builds the stable online tree. Consequently its default A1/A3/A4 comparisons and `-Package` flow need alignment before claiming the legacy all-in-one command validates the new stable online artifact. This mismatch was identified during final handoff review and has NOT been changed or rerun here.
2. `online_stable_runtime_report.txt` is the shared output name for any RuntimeOnly/EngineRoot run; archive it before repurposing that mode for a local sweep. The consolidated online report contains the final runtime results too.
3. Improve M so an explicit public-service-only test cannot pass via LAN fallback, then rerun it. Existing M1 registration evidence is valid; exclusive public discovery is not established.
4. Current scene tests do not prove a complete playthrough, every boss defeat, all third-party mods, perfect visual composition, or connectivity between two different home networks. Keep those claims separate from the passing fixtures.
5. This user request only expanded the handoff. No gameplay, engine, package, or test implementation changed in this documentation-only update, and no further game tests were needed or run.

## Claude review addendum (2026-11-10)

Claude read this handoff and the Codex transcripts and re-audited the work. Findings, fixes and open items:

**Corrections to claims above**
- "Online has not started" is stale: the online parity checks later passed.
- M1/M2 (matchmaking) passed only via LAN fallback; the public-service path alone is not proven.
- "No subagents were used" is wrong for part of the work; treat it as unverified.
- The legacy runner defaulted to `sonic3air_source` while the zip is built from `sonic3air_online_stable_source`; this caused false A3/A4 failures.
- Four-view split needs an extended engine that is not in the stable fork or any package. 3/4 players currently share one screen in the shipped build.

**Fixed by Claude**
- `teamwork.lemon`: boss win-pose could leave a player frozen (state/control flags never cleared). Now tracked by `dropin.team_pose_mask` and released.
- `party.lemon`: stale 3rd/4th camera after leave+rejoin; `camera3/4.valid` and `view3/4.state_valid` now reset in joinParty.
- `run_checks.ps1`: A1/A3/A4 now compare against the stable tree the package is built from (`$pkgRoot`).
- `make_package.ps1`: ships the engine LICENSE as LICENSE.txt (GPL), and warns if the exe embeds a `C:\Users\<name>` path.
- README wording about the "extended engine"; GPL paragraph added to README_ONLINE.txt.
- Zips rebuilt; static run `-Quick`: 21 of 21 pass. Game stages (D-R) were NOT rerun after the mod edits.

**Open, needs the owner's decision**
- The exe embeds the builder's user folder (PDB path). Rebuild with `/PDBALTPATH` and `/pathmap` before public release.
- GPL: the modified engine source needs a public location (not invented here).
- Port four-view into the stable fork, or keep documenting it as unavailable.
- Netplay C++ review (read-only) found: host accepts any UDP peer with no tie to the session code (F2); unbounded queue growth from a huge packet ID (F1); spoofable LAN reply (F5); uncapped snapshot allocation on the joiner (F4); repair requests never retried (F8) and unthrottled (F9); host starts only if slot 0 is connected (F11); debug env switches ship in the release exe (F16); sync hash covers only 64 KB RAM, not the script globals (F18). Fine for friends-on-trust use; fix before any public hosting.
- Unconfirmed edge: pose release after a boss restart.
- Lives sharing in monitor clones; unused `tools/SheetEdit` download; 185 MB `checks/sonic3air-stable-source.zip` and other orphan folders.

**Path-leak fix (2026-11-10, later)**
- `build_online_stable.ps1` now sets `CL=/d1trimfile:<workspace>` and `_LINK_=/PDBALTPATH:%_PDB%`, accepts `-Rebuild`, and runs `scrub_exe_paths.ps1` at the end. The scrub overwrites the workspace path in prebuilt SDL/ImGui assert strings (those libraries are not rebuilt by the project build).
- Result: `Sonic3AIR.exe` contains no `C:\Users\...` and no user name (ASCII or UTF-16); the packager warning no longer fires.
- Verified after the rebuild and repackage: `-Quick` 21/21, `-Only F` 39/39, `-Only D` (online join by code, 15 s sync) pass. Other online stages (E, M, N, R) and G/H/Q were not rerun.
- Always build with `build_online_stable.ps1` (not a bare MSBuild call) or the path comes back. Previous exe is not kept in the repo.

**Netplay hardening (2026-11-10, Claude)** — engine fork changes, rebuilt through `build_online_stable.ps1`:
- F1 `ReceivedPacketCache.cpp`: a packet whose ID is more than 1024 ahead is refused (was an unbounded loop of queue allocations).
- F4 `NetplayClient.cpp`: a joiner ignores game-state chunks with an implausible size (compressed over 32 MB, or uncompressed over 128 MB).
- F11 `NetplayHost.cpp`: the host starts the game when any player is connected, not only slot 0.
- F8 `NetplayClient.cpp/.h`: a repair request lost on the network is sent again after 600 frames.
- After the rebuild: `-Quick` 21/21, `-Only D` 25/25, `E` 24/24, `N` 30/30, `R` 23/23, `M` 23/23 (M still runs both peers on one PC). E2 failed once with a 0 fps sample on the first run after a rebuild, then passed twice at 60 fps; treat it as a flaky first sample.
- Still open from the review: F2 (host accepts any peer), F3, F5, F6, F7, F9, F10, F12-F20. None of these are fixed.

**F2 partial fix (2026-11-10, Claude)** — `NetplayHost.cpp/.h`: when hosting through the matchmaking server, the host now accepts connections only from IP addresses the server introduced (or that connected before, so reconnects work). Anyone else is refused and logged ("refused a connection from an address the matchmaking server did not introduce").
- NOT covered: hosting by direct IP code or LAN. The join code there holds only IP and port, so the host has no secret to check; anyone who can reach UDP 28840 can still connect. Fixing it needs a secret in the join code (protocol change).
- Not tested: a refused stranger. No check simulates one; only the allowed paths (M, D, E, N, R, all passing after the change). A friend whose NAT changes IP during a session would be refused on reconnect.

**Packaging bug found by launching the zip (2026-11-10, Claude)**
- `Sonic3AIR_Online` failed at start with "Could not find engine data": the package had no `data/oxygen_icon.png`, `data/shader` and the other engine files (in `oxygenengine/data`). The README claimed they were included. No earlier check ran the package exe itself; the checks ran the build tree, so all passed.
- Fixed: `make_package.ps1` merges `oxygenengine/data` into `data/` and refuses to package without it; new check A11 verifies the zip. The packaged game now starts to the menu.
- Lesson: at least one check must run the exe from an extracted copy of the zip. Not done yet; only a manual launch confirmed the fix.

**Check U added (2026-11-10, Claude)** — `run_checks.ps1 -Only U` (also part of the full run, not -Quick): unzips `Sonic3AIR_Online.zip` into a fresh temp folder, starts that exe with no test variables, and requires "Ready to go" in the game log with no "Could not find engine data" and no Error window (U1), the log's executable path to be the unzipped copy (U2), and the game still running 5 seconds later (U3). It then stops the game and deletes the temp folder.
- Proven to catch the original bug: with `data/oxygen_icon.png` removed from a copy of the zip, U1 and A11 failed (window title "... - Error"); with the real zip all pass.
- Limit: it proves the package starts to the menu, not that the co-op mod or online play work from the unzipped copy.

**Root cause of the missing engine data (exterminator, 2026-11-10)**
- In a source tree, Oxygen mounts `../oxygenengine/data` as `data/` when started next to the engine source (`EngineMain.cpp` `initFileSystem`). So the game found `oxygen_icon.png`, shaders and fonts only because that sibling folder existed.
- `make_package.ps1` copied the game's `data/` alone and assumed it was complete. Every check ran inside the source tree, where the sibling folder hid the gap, so 258 of 259 passed on a package that could not start.
- Siblings checked: `make_local_package.ps1` is a mod-only zip (no engine), not affected; no other script copies engine data. Guards: A11 (content) and U (unzipped run), both tested by removing the file from a copy of the zip. Pattern logged as `works-only-in-dev-tree`; exterminator skill gained that class.

## Camera and lobby work (Claude, 2026-11-10 and 11)

**Larger split-screen views (local two-player)** — new setting "Split Screen View Size" (Small 112 / Medium 144 default / Large 176 lines per view).
- While the screen is split, `DropIn.applyViewSize` (split.lemon) makes the screen taller (`Renderer.setScreenSize`) and the plane memory bigger (`VDP.Config.setPlayfieldSizeInPatterns(64, 64)`); plane B's name table moves below 0xf000 (`DropIn.planeBaseB`) because the game DMAs its line scroll table to 0xf000. Online play and the AIZ1 intro keep the original size. The game camera's vertical offset is scaled to the view height (`DropIn.scaleCameraOffset`).
- Found and fixed with the exterminator workflow (all logged in `.claude/bug-patterns.md`): (1) ROM band tables of the zone scroll code ran past their end with a 288 line screen (`fn04f0ce` override asks for 224 lines); (2) the line scroll table ends at 0xffffe380, where the dynamic object list starts, so taller screens overwrote it (the memory behind the table is saved and restored around the level display update); (3) game scripts hard-code 224 lines: AIZ1 water scroll computes `0xde - x` (negative with a taller screen) and has a `u8` loop that never ends (`DropInCoop/scripts/screensize.lemon`, only active while the screen is enlarged).
- Verified on the old test engine (sonic3air_source, v26.08.15): `-Only F` 39/39, `-Only G` 120/120 earlier except A5, `-Only Z` pass. Verified on the stable engine (26.03.28): see the last run below. Not verified: every water crossing in every zone; other game scripts written for exactly 224 lines may exist. If a crash appears in split screen, pick "Small" in the settings first.
- Harness options: ActHarness and ChecksHarness set `option.dropin.view_size = 1` (the default). Z passed with 0 and 1.

**Online lobby (stable fork)** — main menu entry "ONLINE LOBBY" (`LobbyMenu.cpp/.h`, wired in MainMenu/MenuBackground).
- Host creates a lobby (session code through the matchmaking server), friends join with the code and press Ready; the host starts the game when everybody is ready, picks the game as usual, and friends follow (existing `StartGame` flow). Protocol: `LobbyStatePacket` (host to clients) and `LobbyReadyPacket` (client to host) in NetplayPackets.h; logic in NetplayHost (`getLobbySlots`, `areAllClientsReady`, `updateLobby`) and NetplayClient (`sendLobbyReady`).
- Test hooks (environment): `S3AIR_LOBBY_AUTOREADY`, `S3AIR_LOBBY_OPEN=host|hostlan|join` with `S3AIR_LOBBY_CODE`. Stage L (only with `-Only L`, run with the stable engine, part of `run_online_backend_checks.ps1`): L1-L3 network level, L4-L5 through the real menu of the unzipped package, with pictures `last_logs/lobby-host.png` and `lobby-friend.png`.
- Limits: the menu test connects over 127.0.0.1 (same computer), not through the public server; the first time somebody hosts, Windows asks for firewall permission (the user must click Allow). Invites are by code only (there is no friends list or account system). Names are "Player n".

**F2 follow-up (2026-11-11)**: the known-peer allow-list refused friends on the same network who found a server-hosted lobby through LAN discovery (M2 failed once the LAN path won the race). The host now also accepts addresses that asked for its session code on the LAN (`UDPSocket::hasLanRequester`). Stable engine runtime stages D, E, M, N, R and L pass (25/25) after the fix. Static `-Quick` and the full run on the test engine pass except V1 (a vanilla-game crash diagnostic that does not involve the mod; it failed before any of this session's changes were relevant).

## Stage A: netplay hardening (Claude, 2026-11-11)

Interrupted earlier attempt reviewed: `TestHooks.cpp/.h` were sound (one invalid escape fixed, now also listed in `oxygen_netcore.vcxproj` and `.filters`). Everything below is built with `build_online_stable.ps1`, verified by `run_online_backend_checks.ps1`: **ONLINE BACKEND CHECKS PASS, 81 PASS lines, 0 FAIL** (core tests, build, package, runtime D, E, M, N, R, L, S, X, U); `run_checks.ps1 -Quick`: 23/23. The backend script now builds first (the core tests link the new libraries) and runs stages D,E,M,N,R,L,S,X,U.

**Join secret (F2).** `NetplayAuth.h`: both sides prove knowledge of a secret by challenge-response (SHA-1 over secret, role, host nonce, client nonce); the host ignores everything but the handshake until the friend is proven, and the friend ignores everything from the host until the host is proven (this also stops a fake LAN host, F5). Join code (direct IP) = IP + port + random 30 bit secret + CRC-12, 18 characters `XXXXXX-XXXXXX-XXXXXX`. Session/lobby code = 8 characters + 1 check character (`XXXX-XXXXX`); the secret is derived from the session ID by hash, so server-introduced and LAN-found friends work with the same code. LAN trust decision: the broadcast only carries a one-way hash tag of the session ID; a LAN device that merely listens learns the tag, not the code or secret (offline brute force of a 40 bit ID remains theoretically possible). The old known-peer/LAN-requester IP list for server hosting stays as a second layer. A typed bare IP no longer works. Wrong attempts: refused with a reply, then ignored per address after 8. Tests: core (auth, replay, role swap, codes), runtime S1-S3 (hostdirect:/joindirect: developer modes).

**Review findings.** F1/F4/F8/F11 were done before. Now: F3 (UDP packets only count from the connection's own address; connection IDs from an OS-seeded generator; core test with spoofed sender plus control), F4 (per-field blob limits, bounded inflate `ZlibDeflate::decode(..., maxOutputSize)`, test with a 64 MB bomb), F5 (above), F6 (LAN packets consumed inside the socket no longer end the receive loop; replies rate-limited; core test fails with the old logic), F7 (loadState fails on serializer error; Lemon `Runtime::serializeState` bounds call stack, locals and value stack; host settings already go to the non-persistent alternative settings; not fuzzed), F9 (one repair per 10 s per friend), F10 (friend heartbeat every 2 s, host-left detection, X8/X9), F12 (frame numbers must be plausible, 64-bit lead check; no dedicated test), F13 (host registration timeout 25 s and failure reason, X10), F14 (session code check character, ids below 0x10000000 refused, direct-IP host no longer uses the default session ID), F15 (a returning friend asks for its old slot; same-IP old connection is taken over; covered indirectly by N6/R1), F16 (see next), F17 (nonsense frame counts refused, 600 frame cap, failed state load now an explicit failure BAD_GAME_DATA), F19 (retired connections are deleted one update later; no leak measurement was done), F20 (discovery also goes to each adapter's subnet broadcast; delayed packets cleared on close; core test only checks the addresses are valid, not that a VPN friend was reached).
**F18 not fixed as such:** the sync hash still covers the 64 KB work RAM only (shared memory differs by design, ROLE_ADDRESS); README now states the limit.

**Test hooks (F16).** All `S3AIR_*` variables go through `TestHooks::getEnv`, honoured only if `testhooks.enabled` lies next to the exe. `run_checks.ps1` creates it next to the engine exe for its duration and removes it in `finally` (and at the next start if a run was interrupted); `make_package.ps1` refuses to package it (folder and zip), A12 checks the zip, U4/U5 run the unzipped exe with variables set (ignored without the marker, honoured with it). `S3AIR_LOGNAME` is filtered to safe characters. New test switches: `S3AIR_NETPLAY=hostdirect:HEX|joindirect:HEX`, `S3AIR_NETPLAY_MAXCLIENTS`, `_CLOSE_AFTER`, `_FAKE_BUILD`, `_FAKE_MODS`, `S3AIR_LOBBY_AUTORETRY`.

**Failure reasons.** `NetplayFailure.h`: SERVER_UNREACHABLE, WRONG_CODE, LOBBY_FULL, VERSION_MISMATCH, MOD_MISMATCH, BLOCKED_OR_TIMEOUT, HOST_CLOSED, CONNECTION_LOST, BAD_GAME_DATA, each with a plain message; the host sends a `JoinReplyPacket` refusal (wrong code, full). LobbyMenu shows the message and a RETRY entry. Runtime X1-X10 produce each reason locally (X7 shows Retry getting a second friend in once a slot is free; screenshots `checks/last_logs/X-*-screen.png`). Limits: the Retry key press is simulated by a test switch; everything ran on one computer; X2 needs the public matchmaking server.

**Remaining:** real cross-network test; F12/F9/F15/F19 without dedicated regression tests; F18 hash scope; four-view split still absent from the stable fork. Not pushed or released.
