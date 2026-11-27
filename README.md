# Sonic 3 AirNet

**Online co-op for Sonic 3 A.I.R.** Up to four players, each on their own screen, in one game of Sonic 3 & Knuckles: on Windows, in the web browser, or both together.

[**Play in your browser**](https://trickernoxics14.github.io/Sonic-3-Airnet/) &nbsp;·&nbsp; [**Download for Windows**](https://github.com/TrickerNoxics14/Sonic-3-Airnet/releases/latest)

---

## Features

- **Up to four players online.** Every player gets the full screen, their own controller and their own character, chosen in the lobby.
- **Lobbies that just work.** Browse the public lobby list, use **Quick Join**, or share a 6-digit code with friends for a private lobby. No setup, no port forwarding.
- **Play anywhere.** The browser version runs on any modern desktop browser (WebAssembly). Windows and browser players share the same lobbies.
- **Stays in sync.** Lockstep netplay keeps every game identical, compares the game state continuously and repairs any difference on its own.
- **LAN and direct play.** Play on the same network without any server, or connect by long code through the matchmaking server.
- **Privacy built in.** Your ROM and save data never leave your computer or browser. Games in public lobbies always run through the relay, so other players never see your IP address.
- **Updates in-game.** The Windows version checks this repository for new releases (*Options → Check for updates*).

## Getting started

You need your own copy of the **Sonic 3 & Knuckles** ROM (`Sonic_Knuckles_wSonic3.bin`). It is not included and never uploaded anywhere.

### In the browser

1. Open **[trickernoxics14.github.io/Sonic-3-Airnet](https://trickernoxics14.github.io/Sonic-3-Airnet/)** and choose *Play now*.
2. Select your ROM file when asked. It is stored in that browser only.
3. Choose **Online Lobby** in the main menu: create a lobby, use Quick Join, or enter a friend's code.

### On Windows

1. Download **Sonic3Airnet.zip** from the [latest release](https://github.com/TrickerNoxics14/Sonic-3-Airnet/releases/latest) and unzip it.
2. Put your ROM in `%APPDATA%\Sonic3AIR\` (press Win+R, type `%APPDATA%\Sonic3AIR`, press Enter).
3. Run `Sonic3AIR.exe` and choose **Online** in the main menu.

Everybody in one lobby needs the same game version. To play with browser players, turn mods off on Windows.

## How online play works

Players meet through a small **relay server** that keeps the public lobby list, hands out the 6-digit codes and forwards game data. The default relay is hosted for free and kept awake by a scheduled job; if it ever falls asleep, the first player waits up to a minute while it wakes up.

In a private lobby, two browsers may connect to each other directly (WebRTC) for lower latency. This shows each player's IP address to the other, so it is never used in public lobbies, and it can be switched off by opening the page with `?p2p=off`.

The relay is open source and can be self-hosted: see [`checks/relay_ws/HOSTING.md`](checks/relay_ws/HOSTING.md).

## System requirements

| | Minimum |
| --- | --- |
| Windows | Windows 10 or 11 (64-bit) |
| Browser | A current desktop browser with WebAssembly and WebGL. Tested with Chrome; Edge uses the same engine. Firefox and Safari are not tested yet |
| Network | Internet access for online lobbies; none needed for LAN play |

## Building from source

| What | How |
| --- | --- |
| Windows game | `checks\build_online_stable.ps1` (Visual Studio 2022) |
| Web version | `checks\build_web.ps1 -EmsdkDir <emsdk folder>`, see [`WEB_BUILD.md`](checks/package_files/WEB_BUILD.md) |
| Relay server | Node.js 18 or newer: `node checks/relay_ws/server.mjs`, see its [README](checks/relay_ws/README.md) |
| Tests | `checks\run_online_core_tests.ps1`, `node checks/web_net/run_web_net_checks.mjs` |

The repository contains the game and engine source (`sonic3air_online_stable_source`), the build and test scripts (`checks`) and the Drop-In Co-op mod (`DropInCoop`).

## Credits

Sonic 3 AirNet is built on **[Sonic 3 A.I.R.](https://sonic3air.org/)** (*Angel Island Revisited*) by Eukaryot and its contributors, a fan-made remaster of Sonic 3 & Knuckles, based on its stable release 26.03.28.0.

## License and disclaimer

The source code is published under the [GNU General Public License v3](sonic3air_online_stable_source/LICENSE).

Sonic 3 AirNet is a non-profit fan project. It is not affiliated with or endorsed by SEGA, Sonic Team or the Sonic 3 A.I.R. team. Sonic the Hedgehog and all related characters, names, art and music are trademarks and copyrights of SEGA. No game ROM is included or distributed; every player must supply their own legally obtained copy.
