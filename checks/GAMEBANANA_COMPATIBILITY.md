# Third-party mod compatibility review

Reviewed 2026-11-09. No third-party mod files were installed, copied into the project, or added to either player package.

## Character frameworks

| Candidate | What it provides | Fit with our extra players | Decision |
| --- | --- | --- | --- |
| [MISC Framework](https://gamebanana.com/tools/21589) | Extra selectable/save character identities; the public framework describes characters as integrated per-character mods. | Not a drop-in multiplayer API. Our extra players are separate runtime actors; MISC characters are additional character identities built on the game's existing character bases. | Best framework to target for a reusable adapter, after one character proves the adapter. |
| [Extra Character Framework + JUWL Characters](https://gamebanana.com/mods/650382) | Current extra-slot framework page with JUWL characters, including Mighty, Ray, and Amy. | Same adapter gap: extra save slots do not automatically become independent party actors. | Best first external compatibility candidate after the adapter exists. Do not redistribute its files. |
| [ESU Framework](https://sonic3air.boards.net/thread/28/esu-framework) and [Reto the Rabbit](https://gamebanana.com/mods/571981) | Legacy extra-slot framework and a character implemented for it. | Reto's ESU slot does not map to party players 3 or 4 by itself. It also adds an older framework path to maintain. | Defer; test only if a player specifically needs Reto. |

The adapter needs to take a framework character's selected base, update/movement hooks, animation selection, sprite and palette identity, and ability state, then bind those to one party actor without changing the other actors. Framework slot numbers alone cannot satisfy this.

## Zone and menu mods

| Candidate | Compatibility assessment |
| --- | --- |
| [Custom Zone Select + Useott's Extra Zones](https://gamebanana.com/mods/511333), including [Cold Peaks](https://gamebanana.com/mods/523499) | The mod routes extra zones through alternate stock zone slots and custom raw data. Our existing per-zone camera and split rules assume vanilla zone-act IDs, so loading is plausible but camera, split, scrolling, and boss behavior are unverified. Start with a two-player camera smoke check, then test four views. |
| [Harmony Styled Level Select](https://gamebanana.com/mods/648071) | Its page explicitly says it is not ESU-compatible and documents separate compatibility hooks for MISC character icons and CZS zone entries. It changes level selection, so treat it as a menu integration test, not a gameplay dependency. |
| [An Act Select Mod](https://gamebanana.com/mods/652423) | Its page advertises support for MISC, ECF, ESU, and Custom Zone Select, with a prescribed load order. It still needs conflict checks against our act-start/menu hooks. |
| [Separate Act 2 Backgrounds](https://gamebanana.com/mods/676717) | A rendering/background override only. It may render in a single view, but four independent cameras and their background offsets need visual comparison. |

## Multiplayer integration consequence

The online fork compares game build and the active mod list before starting. Its signature now includes each active mod's files and settings, so matching IDs and version strings alone cannot silently admit different local copies. The online fork has been ported onto official stable v26.03.28.0 and the Release x64 solution builds successfully.

Run `powershell -ExecutionPolicy Bypass -File checks/run_online_backend_checks.ps1` for the offline code/fingerprint tests, stable Release build, package guards, two-player code join, lag/loss handling, and four-player online/reconnect checks. Runtime results are saved to `checks/online_stable_runtime_report.txt`; the consolidated result is `checks/online_backend_report.txt`. These checks use multiple game instances on this machine and do not verify a second home network, public matchmaking, or real friend connectivity.

## Recommended order

1. Done: port the online fork to official stable v26.03.28.0 and retain the upstream sender-address validation fix.
2. Done in local multi-instance runtime checks: same-mod handshake, four online player slots, reconnecting player three, synchronized teamwork effects, and 250 ms / 5% packet-loss performance. Still needed: verify play between separate home networks and through the public matchmaking service.
3. Build a small adapter for one MISC/ECF character. Use JUWL Mighty as the first candidate; do not include its files in our release.
4. Add a CZS extra-zone camera fixture only after the vanilla actor and camera hooks are stable.
