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
- **Direct connections (WebRTC, peer to peer).** Once two browsers are in a lobby, the friend offers the host a WebRTC data channel
  (unordered, no retransmissions: it behaves like UDP, which the game's protocol was written for). The offer, the answer and the ICE
  candidates travel through the relay as new message types (SIGNAL_*, the relay needs no change in its data path and stays the lobby and
  the signalling server). When the channel works, the game's datagrams go over it and the relay carries (almost) nothing; the lobby shows
  "Direct connection to the host (peer to peer)". If no channel opens within 8 s, or it dies or goes silent later, the game uses the relay
  again at once (both paths stay open for receiving; the game's protocol already handles lost, repeated and reordered datagrams). A friend
  whose attempt never worked asks again after 12 s, 24 s, ... (at most every 96 s); one that worked and died asks again after 12 s.
  **Privacy:** a direct connection shows each player's IP address to the other player (WebRTC), which a relay-only game does not.
  Switch it off with `?p2p=off` in the page link, `"Online": { "DirectConnection": false }` in the browser's settings.json, or
  `window.S3AIR_P2P = false` in `relay_config.js`. STUN servers (they tell a browser its public address; they never see game data):
  default two public Google STUN servers; change with `?stun=stun:host:port,...` (`?stun=none` = none, then only players in the same
  network connect directly), `"Online": { "StunServers": "..." }`, or `window.S3AIR_STUN_SERVERS` in `relay_config.js`. **No TURN
  server is used**: players behind NATs that STUN cannot cross (symmetric NAT, some mobile and company networks) simply stay on the relay.
  The Windows game has no WebRTC (see the handoff): a lobby with a Windows player keeps using the relay for that player, and a browser
  friend's offer to a Windows host is simply never answered.
- A tab in the background: a browser stops the animation frames of a hidden tab (the game's main loop) and slows its timers to about one per second, which used to stall an online game for everybody. The game now keeps
  an online tab running in the background with a Web Worker that ticks the main loop (in headless Chrome 157, the only browser tried, worker timers are not slowed down; Firefox, Safari and phones were not tried; `?bgtick=off` switches it off), tells the other players at once when its tab
  is hidden ("Waiting for NAME: their tab is in the background"), asks the player to return in the tab title, plays on without a player that is gone for 8 s, drops a friend after 25 s (the slot is kept for a minute)
  and ends a friend's game when the host's tab stays away for 60 s. An online game is never paused by losing the focus. Browsers that stop a hidden page entirely (a tab frozen or discarded by the browser, phones) still stall it;
  keep the tab visible there.
- Tested: see the handoff (headless Chrome, relay on 127.0.0.1, direct connections between two Chrome instances on this PC with STUN off). Not tested:
  a real internet relay, real NATs / STUN / TURN-needing networks, a real GPU, Firefox, Safari, phones.
- Automated check: `node checks/web_net/run_web_net_checks.mjs` (browser sessions A, B, C and P = direct connections), `--native` adds the Windows game sessions.
- Netplay, relay and Blue Sphere multi-runner code are the same as in the Windows version; the Blue Sphere multi-runner was not tried in the browser.

## Differences to the Windows version
- Update check: shows "Updates are not available" (reload the page for a new version). Bundled-mod installer unused.
- Save data and settings live in the browser (IndexedDB, mounted at /sonic3air/savedata); clearing site data deletes them.
  The in-game file manager (Options) can export/import files.
- Remastered music: not included.
- Sound starts after the first click/key on the page (browser autoplay rules).
- No mods, and the local split-screen mod is not part of this build. Online play needs a relay (above).
