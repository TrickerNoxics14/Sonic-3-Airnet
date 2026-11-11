Drop-In Co-op (v0.2.0, experimental party mode)

Local co-op for Sonic 3 A.I.R. This is a script mod; it does not include or require the online fork.
Two-player mode works with the stock game. Three/four-player party mode targets the latest stable stock Sonic 3 A.I.R. v26.03.28.0. Four-view split screen needs an extended engine build that is NOT part of any current download, so 3/4 players share one screen for now.
Online play requires the separate modified engine build; this mod-only package is for local play.

INSTALL
  Copy the "DropInCoop" folder into  %APPDATA%\Sonic3AIR\mods\  and enable it in the game's Mods menu.
  Use the latest compatible stock Sonic 3 A.I.R. release for three/four players.
  Works together with Co-op Deluxe (for separate rings per player, shields and more).

HOW TO PLAY
  Start a game as Sonic & Tails (or Knuckles & Tails).
  JOIN:   press Start on controller 2 during a normal stage. Tails flies in (if off-screen) and is yours.
          The screen splits: player 1 on top, player 2 below, each with their own camera.
  LEAVE:  hold Back/Select on controller 2 (1 second by default). Tails goes back to the CPU and the
          screen returns to full size. The level keeps running.
  Controller 1 Start still pauses as usual; controller 2 Start never pauses.
  HITS:   once player 2 has joined, Tails can get hurt like Sonic (setting "Player 2 Can Be Hurt", on by default):
          he drops the shared rings, and dies when hit with no rings and no shield (no life is lost; he flies back in).
          In the original game, Tails can never lose rings or die from hits. With Co-op Deluxe, that mod handles this
          itself, with separate rings for each player.

SETTINGS (Mods menu)
  Player 2 Must Join, Drop-Out hold time, Drop-Out on idle controller, Player 2 Can Be Hurt, Split Screen, Split Screen Mode, Split Screen View Size, On-Screen Hints.

SPLIT SCREEN
  - Split Screen Mode (setting): by default ("Only when the players are far apart"), both players share one
    full-size screen while they are close together, and the screen splits when they move apart; it goes back
    to one screen after they stayed close together for two seconds. This makes the picture much clearer
    than two small views. "Always split" keeps the two views the whole time.
  - Split Screen View Size (setting, Visuals): how much of the level each view shows. "Small" is the original
    112 lines per view, "Medium" (default) 144 lines, "Large" 176 lines. A bigger view makes the whole picture
    smaller on your monitor (it is a zoom out): the picture gets taller, so on a wide monitor you may see
    bars at the sides. Only while the screen is split, in local two-player mode; online
    play and the intro of Angel Island 1 keep the original size. If something looks wrong, pick "Small".
    The Extended Widescreen mod works together with this mod and makes both views wider.
  - Player 2 gets the "Extended Camera" look-ahead (see Options > Gameplay) in the split screen, like player 1.
  - Each view has its own camera, background, objects, rings (player 2 collects the rings in their own
    view), and its own level bottom border (player 2 can go to lower areas than player 1's camera allows).
  - The screen never switches between one and two views in quick succession: it splits half a second
    after everything is ready, and short interruptions (like a camera lock) have to last a second
    before it goes back to one view.
  - Compact HUD: player 1's view shows time, rings, score and lives in one row at the top; player 2's
    view shows player 2's rings. Separate ring counts per player need the Co-op Deluxe mod (it is
    compatible); without it, both players share the rings as in the original game.
  - If Co-op Deluxe's shared camera is on, it is ignored while the screen is split (each view follows
    its own player).
  - When player 2 dies, the lower view stays where it is, then jumps to Tails once he flies back in.
  - Water: the game can only switch to the underwater colors at one screen line. When both views show
    a water surface at the same time, part of one view may be tinted wrong. The mod picks the option
    with the fewest wrong lines, and does not switch between options back and forth.

KNOWN LIMITATIONS
  - Mods that change the screen size while playing (like "Screen Zoom-in") cannot be combined with the split screen:
    the screen stays single while such a mod is active. Mods that set a fixed width (like Extended Widescreen) are fine.
  - Mod order: works with Co-op Deluxe in either order in the mod list.
  - The split screen works in the regular parts of Angel Island, Hydrocity, Marble Garden, Carnival Night,
    Flying Battery, Ice Cap, Launch Base, Mushroom Hill, Sandopolis, Lava Reef, Sky Sanctuary and Death Egg.
    It uses a single screen in other parts: intros, cutscenes, the snowboard run in Ice Cap, the Marble
    Garden earthquake areas, bosses with their own scrolling, the Doomsday Zone and the endings.
    While an area is not supported, the game plays like normal with one screen.
  - Zones where the background changes between areas (Ice Cap 2 indoors/outdoors, Lava Reef, Mushroom Hill 2,
    Marble Garden 2, Sky Sanctuary): each view shows the background of its own player's area in Ice Cap 2;
    in the other zones the screen goes back to one view while the two players are in areas with different
    backgrounds, and splits again once they are in the same kind of area.
  - Ice Cap 2: while the water surface is visible in player 2's view, that view uses player 1's colors for the
    background (the shapes are right, the colors may be off) -- the game has only one color switch per screen.
  - Angel Island 1 starts with the burnt intro area, which uses different level graphics: the split
    starts once player 1 has left that area (a hint is shown until then).
  - Single screen during the fire at the end of Angel Island 1, the airship sequence and the boss at
    the end of Angel Island 2, cutscenes, camera locks, and while player 1 is dying.
  - The hollow tree in Angel Island 1 does not show its opening animation while the screen is split.
  - Frame Interpolation (video settings) is not supported in split screen: the views may look jittery.
    Use V-Sync instead.
  - Does nothing in Sonic Alone (there is no Tails object), special/bonus stages, Competition.
  - A controller disconnect cannot be detected by scripts; the optional "idle" setting is used instead.


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
  On the official stock game, three/four-player mode uses one shared camera.
  An extended engine build (not currently distributed) would add optional four-view split screen. Enable
  Visuals > Four-Player Quadrant Split Screen when using that engine. The game falls back to a
  shared view during bosses, water scenes, unsupported scrolling and camera
  locks. Boss fights use one shared screen for the whole party.
  Players who fall behind regroup at a grounded Sonic; extra-player deaths
  respawn near him. Extra players keep their join request across act changes.
  Each player has their own rings and score. Turning Maximum Players back to 2
  removes the extra players and makes two-player split screen available again.
  For online play, every friend uses this updated build and presses Host/Join
  as usual. Friends are assigned separate slots 2, 3 and 4. The host must set
  Maximum Players to 4 before starting a four-player game.
  Four-view mode requires an extended engine build that is not currently distributed, and does not work with the
  official stock executable. It remains experimental: full routes, bosses,
  scripted transports and super/shield combinations are not fully audited.
  The new network packet format requires every participant to update the engine.

OPTIONAL ESU EXTRA-SLOT CHARACTERS (experimental adapter)
  P3 Extra Character Slot and P4 Extra Character Slot are under Teamwork.
  Leave the value at 0 for built-in Knuckles/Sonic. Slots 35 and 90 select
  characters only when the matching extra-character mod and Extra Slots
  Unlimited (ESU) are installed and enabled. The adapter uses the character
  mod's own startup and sprite callbacks; it does not include ESU, character
  scripts, or artwork. Each extra slot must be unique in your mod setup.
  This bridge is currently an integration preview: player movement and custom
  abilities can still depend on how a character mod hooks ESU, so not every
  extra-slot character or ability is verified for independent P3/P4 play.
  Reto's mod is third-party content with its own license; install it separately
  from its original author and keep it out of the co-op ZIP.
