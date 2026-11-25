# Sonic 3 AirNet in the browser (preview, online play through a relay server)

A WebAssembly build of this fork, made with the official Emscripten SDK. Based on the web target of the official
Sonic 3 A.I.R. source (`sonic3air_online_stable_source/Oxygen/sonic3air/build/_emscripten`).

## What the player needs
- The player supplies their own `Sonic_Knuckles_wSonic3.bin` (4 MB). The page asks for it on the first visit, checks it
  and stores it in the browser only (IndexedDB). It is never uploaded and is not part of the build.
- A browser with WebAssembly and WebGL (Chrome, Edge, Firefox, Safari released after 2017). No special server headers
  are needed (the build uses no threads and no SharedArrayBuffer).
- Download size: about 12.7 MB raw, about 6.2 MB gzip (wasm 5.06 MB, data package 7.30 MB, js 0.24 MB). The remastered
  soundtrack (130 MB) is not included, the emulated soundtrack is used.

## Build (Windows)
1. Install the official SDK once: `git clone https://github.com/emscripten-core/emsdk`, then `py -3 emsdk.py install latest`
   and `py -3 emsdk.py activate latest` (tested with Emscripten 7.0.0). CMake and Ninja must be on the PATH.
2. Build the Windows game once (the web packaging uses its `-pack` option and one short start to create the data packages
   and the compiled scripts).
3. `powershell -File checks\build_web.ps1 -EmsdkDir <path of emsdk>` builds from scratch (about 3 minutes) into
   `web_build\` in the workspace root. That folder is untracked. Parameters: `-OutDir`, `-PackExe`, `-Jobs`.
   Write the SDK folder with backslashes, and start the script in a normal PowerShell window: from Git Bash the SDK
   writes its PATH in MSYS form and the script stops with "emcmake is not recognized".
   The SDK's clang/wasm-opt on Windows sometimes crashes at random; the script re-runs ninja up to 4 times.
4. **The leak guard** (`checks\web_leak_guard.ps1`) is the last step of the script, because the folder goes to a public page.
   It reads EVERY file of `web_build\` in full (text and binary, the `.wasm` and the `.data` too) and fails the build, and deletes
   the folder, when it finds: the Windows user name or the computer name of the builder, a folder of the builder's machine (the
   workspace, the profile, AppData, Temp, the emsdk; also as `/`, `\\` and `%20` forms and as 8.3 short names), any `<drive>:\Users\`
   path, an e-mail address (the one of the builder's git set-up, or any other; only the four licence-header addresses of the
   upscaler shaders are allowed), a token or private key, a ROM (a file of exactly 4 MiB or 2 MiB, or a Mega Drive header at
   0x100 or 0x300), or a name that belongs to save data, storage, settings, a ROM or `.git`. Every pattern is also looked for as UTF-16 and
   UTF-32 (a wide string in a binary). The values are read from the environment when it runs (nothing is written into a committed
   file), a failure names the file, the rule and the byte offset but never prints the value, and a folder with no file in it fails.
   Some parts are compressed, so a name in them would not show in the raw bytes: the compiled scripts inside `sonic3air.data`
   (zlib behind an `LMD|` header) and the metadata of the pictures (PNG zTXt / iTXt text, the iCCP colour profile, EXIF written
   as hex). The guard inflates or decodes every such part and searches its content too (names also as UTF-16 big endian, the
   form of an ICC profile); a part it cannot read fails the build.
   On success the script prints one line starting with `guard passed:`. Run it alone: `checks\web_leak_guard.ps1 -Dir web_build`;
   `-SelfTest` plants 43 made-up leaks (seven of them in compressed or encoded parts) and must catch each one (it is run_checks B8).
   The script also writes `web_build.stamp` next to the folder (not inside it): the id of the source tree it was built from.

## Run locally
`py -3 -m http.server --bind 127.0.0.1 8000 --directory web_build`, then open http://127.0.0.1:8000/sonic3air_web.html
(`index.html` is the info page). Stop the server afterwards.

## The relay address of the page
`window.S3AIR_RELAY_DEFAULT` in `relay_config.js` (source: `sonic3air_online_stable_source/Oxygen/sonic3air/build/_emscripten/dist/relay_config.js`,
copied next to the page by `build_web.ps1`) is **empty** and stays empty until there is a relay server with an address. A visitor of a page
published like that can play alone, offline, as usual. Choosing ONLINE LOBBY works but shows "No server is set. Choose SETTINGS, then RELAY SERVER." in
orange and "No lobby server is set, so there are no public lobbies"; CREATE or JOIN says "Online play in the browser needs a lobby server (or relay
server). Choose SETTINGS, then RELAY SERVER, and enter its address first." and goes back; the settings line reads "RELAY SERVER: NOT SET". The info
page (`index.html`) explains the relay in one paragraph. Nobody can play online on that page until somebody types a relay address into RELAY SERVER
(saved in that browser only) or opens the page as `sonic3air_web.html?relay=wss://...`. The direct (WebRTC) connections need the relay too (it is
their signalling server), so there is no online play at all without one.
When there is a relay: how to host one for free, its `ALLOWED_ORIGINS` (the page's origin) and where the address goes is in
[`checks/relay_ws/HOSTING.md`](../relay_ws/HOSTING.md). Put the address (`wss://...`) into `relay_config.js`, build again, run the browser
checks, publish again (below). The file is a plain script: it can also be edited in the published folder without a rebuild.

## Publish on GitHub Pages (only with the owner's explicit yes)
The page is the branch `gh-pages` of the repository. Publishing changes what the public sees and replaces the history of that branch, so it is
done only after the owner has said yes. `checks\publish_pages.ps1` does nothing to the repository unless it is given `-Push`.
1. Build (above) from a clean checkout of `browser-multiplayer` or `main` (`git status` shows nothing), then run the browser checks
   `node checks/web_net/run_web_net_checks.mjs` against the build (they use `web_build\`; every check must pass).
2. **Dry run:** `powershell -NoProfile -ExecutionPolicy Bypass -File checks\publish_pages.ps1`. In a temporary repository it makes a FRESH
   branch `gh-pages` of ONE commit (nothing of the older history; author and committer are the owner's GitHub noreply identity; a Co-Authored-By
   line; the files byte for byte, no line-end conversion; plus an empty `.nojekyll`), proves that every committed file is identical to
   `web_build\`, runs the leak guard on `web_build\` and AGAIN on the exact tree it exported from the commit, and prints the file list with
   sizes. It pushes nothing and deletes the temporary repository (`-KeepTemp` keeps it). If a real push would be refused it says why.
3. **Real push, after the yes:** `powershell -NoProfile -ExecutionPolicy Bypass -File checks\publish_pages.ps1 -Push` = a force-push of
   `gh-pages:gh-pages` of that one commit. It refuses (exit 1, nothing pushed) when: the leak guard fails (either run); the committed tree
   differs from `web_build\`; the current branch is not `browser-multiplayer` or `main`; the working tree is not clean; `web_build.stamp` is
   missing, was made with uncommitted source changes, or names another source tree than the current one (the page is stale: build again); the
   commit is not the single noreply commit; origin is not a plain `https://github.com/<owner>/<repo>` address; or `-SourceDir` was used.
4. After the push, wait about a minute, open the page (`https://<owner>.github.io/<repo>/`), play a level, then check that no leak is served:
   download the published files again into an empty folder (not a clone: a `.git` folder fails the guard) and run
   `checks\web_leak_guard.ps1 -Dir <that folder>`. Look at `relay_config.js` there: it must hold the address you meant (or be empty).
`publish_pages.ps1 -SelfTest` (run_checks B9) tests every refusal rule, the dry run and five leaking fixtures, and never pushes.

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
  So it is only used in a lobby that is NOT on the public list (a private lobby, whose code the host passes on). A lobby on the public list,
  which anybody can join, always plays through the relay: its host switches direct connections off when it publishes the lobby, and a friend
  learns it from the lobby server (the code lookup marks the lobby LISTED), whether it came from the list, QUICK JOIN or a typed 6 digit
  code; no RTCPeerConnection is made, so no address is even gathered (web_net P7; P8 = a private lobby joined with its code still goes
  direct). A lobby joined with the long 13 digit code skips the lobby server: the friend offers, and the host of a listed lobby ignores it.
  The info page says this. Switch direct connections off completely with `?p2p=off` in the page link, `"Online": { "DirectConnection": false }` in the browser's settings.json, or
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
- Automated check: `node checks/web_net/run_web_net_checks.mjs` (browser sessions A, B, C, P = direct connections (P7: none in a listed lobby, P8: a private lobby joined with its code), L = lobby, T = tab in the background, M = right-click menu), `--native` adds the Windows game sessions.
  `--web-dir <folder>` checks another build folder than `web_build\`.
- Netplay, relay and Blue Sphere multi-runner code are the same as in the Windows version; the Blue Sphere multi-runner was not tried in the browser.

## Differences to the Windows version
- Update check: shows "Updates are not available" (reload the page for a new version). Bundled-mod installer unused.
- Save data and settings live in the browser (IndexedDB, mounted at /sonic3air/savedata); clearing site data deletes them.
  The in-game file manager (Options) can export/import files. A right click in the game opens a small menu whose "Open saved data folder" and "Open mods folder" entries open the same file manager in that folder (a browser cannot start the computer's own file manager).
- Remastered music: not included.
- Sound starts after the first click/key on the page (browser autoplay rules).
- No mods, and the local split-screen mod is not part of this build. Online play needs a relay (above).
