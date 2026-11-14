# Sonic 3 AirNet in the browser (single player, preview)

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

## Differences to the Windows version
- ONLINE LOBBY: browsers have no UDP. The entry stays in the menu and shows "Online play is not available in the browser
  version yet." (a WebSocket/WebRTC relay would be needed). Netplay, relay and the Blue Sphere multi-runner are not usable.
- Update check: shows "Updates are not available" (reload the page for a new version). Bundled-mod installer unused.
- Save data and settings live in the browser (IndexedDB, mounted at /sonic3air/savedata); clearing site data deletes them.
  The in-game file manager (Options) can export/import files.
- Remastered music: not included.
- Sound starts after the first click/key on the page (browser autoplay rules).
- The online fork features and the local split-screen mod are not part of this build.
