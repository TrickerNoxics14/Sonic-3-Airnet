# Sonic 3 AirNet in the browser (preview, online play through a relay server)

A WebAssembly build of this fork, made with the official Emscripten SDK. Based on the web target of the official
Sonic 3 A.I.R. source (`sonic3air_online_stable_source/Oxygen/sonic3air/build/_emscripten`).

## What the player needs
- The player supplies their own `Sonic_Knuckles_wSonic3.bin` (4 MB). The page asks for it on the first visit, checks it
  and stores it in the browser only (IndexedDB). It is never uploaded and is not part of the build.
- A browser with WebAssembly and WebGL (Chrome, Edge, Firefox, Safari released after 2017). No special server headers
  are needed (the build uses no threads and no SharedArrayBuffer).
- Download size: about 12.6 MB raw, about 6.2 MB gzip (wasm 4.96 MB, data package 7.30 MB, js 0.23 MB). The remastered
  soundtrack (130 MB) is not included, the emulated soundtrack is used.

## Build (Windows)
1. Install the official SDK once: `git clone https://github.com/emscripten-core/emsdk`, then `py -3 emsdk.py install latest`
   and `py -3 emsdk.py activate latest` (tested with Emscripten 7.0.0). CMake and Ninja must be on the PATH.
2. Build the Windows game once (the web packaging uses its `-pack` option and one short start to create the data packages
   and the compiled scripts).
3. `powershell -File checks\build_web.ps1 -EmsdkDir <path of emsdk>` builds from scratch (about 3 minutes) into
   `web_build\` in the workspace root. That folder is untracked and contains no ROM (the script refuses to finish if a
   4 MB file is in it). Parameters: `-OutDir`, `-PackExe`, `-Jobs`.
   The SDK's clang/wasm-opt on Windows sometimes crashes at random; the script re-runs ninja up to 4 times.

## Run locally
`py -3 -m http.server --bind 127.0.0.1 8000 --directory web_build`, then open http://127.0.0.1:8000/sonic3air_web.html
(`index.html` is the info page). Stop the server afterwards.

## Online play in the browser
A browser cannot send UDP, so the browser version never talks to other players directly and never contacts the game server.
It talks WebSocket to a **relay server** (`checks/relay_ws`, a small Node program that somebody hosts; see its README), which
also speaks UDP to Windows games, so browser and Windows players can be in the same lobby.
- Nothing is deployed for you: with no relay address set, ONLINE LOBBY opens but says that a relay is needed.
- Set the relay (first one that is set wins): the page link `sonic3air_web.html?relay=wss://relay.example.org`; the entry
  RELAY SERVER in the lobby (saved in the browser); `window.S3AIR_RELAY_DEFAULT` in `relay_config.js` next to the page (the
  page owner's default). "host:port" without a scheme means `wss://` (`ws://` for localhost and 127.x). A page served over https
  can only use `wss://` relays (browser rule), except `ws://127.0.0.1`.
- Create a lobby, give the code to friends; friends choose JOIN WITH CODE (lobby codes only: join codes with an address need UDP).
  Everything after that is the normal lobby (names, characters, ready, input delay, chat, host starts the game).
- Everybody in a lobby needs the same game version (the netplay revision text, not the compile time) and the same mods. The browser
  version has no mods, so Windows players have to switch their mods off to play with it.
- Input delay: the host's "Auto" delay plans for the unsteadiness of a relayed line (at least 1 frame, ping plus twice the
  measured deviation). A lost WebSocket packet holds back everything behind it (TCP), which shows as a short stutter, not as a desync.
- A lost relay connection is made again by itself (every 0.5 to 8 s); the friend's game connection then restarts and the host puts
  it back into its slot (the friend tries for up to 90 s, then is told that the connection was lost).
- Keep the tab visible while playing: browsers slow hidden tabs down to about one frame per second, which stalls the game for everybody.
- Tested: see the handoff (headless Chrome, relay on 127.0.0.1). Not tested: a real internet relay, a real GPU, Firefox, Safari, phones.
- Automated check: `node checks/web_net/run_web_net_checks.mjs` (browser sessions), `--native` adds the Windows game sessions.
- Netplay, relay and Blue Sphere multi-runner code are the same as in the Windows version; the Blue Sphere multi-runner was not tried in the browser.

## Differences to the Windows version
- Update check: shows "Updates are not available" (reload the page for a new version). Bundled-mod installer unused.
- Save data and settings live in the browser (IndexedDB, mounted at /sonic3air/savedata); clearing site data deletes them.
  The in-game file manager (Options) can export/import files.
- Remastered music: not included.
- Sound starts after the first click/key on the page (browser autoplay rules).
- No mods, and the local split-screen mod is not part of this build. Online play needs a relay (above).
