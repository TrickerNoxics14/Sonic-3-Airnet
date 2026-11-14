# Sonic 3 AirNet for Android

An installable APK of the online fork. It plays single player and online with the Windows version: same UDP netplay, same
lobby / join codes, same compatibility id (`airnet-1`), same optional relay fallback (`Online.RelayServer` in `settings.json`).
No browser or WebRTC parts are involved.

## Install

1. Get `Sonic3Airnet-<version>.apk` (built with `checks\build_android.ps1`, or from a release).
2. Phone: copy the APK to the phone (USB, cloud, chat), open it, allow "install unknown apps" for the app you opened it with.
   PC with USB debugging: `adb install -r Sonic3Airnet-<version>.apk`.
3. Start "Sonic 3 AirNet". The game asks for your own `Sonic_Knuckles_wSonic3.bin` (Sonic 3 & Knuckles, 4 MB):
   pick it in the file dialog (for example from Downloads). It is copied into the app's own folder; the ROM is **never** inside the APK.
   Alternative: tap the ROM file in a file manager and choose Sonic 3 AirNet, or copy it to
   `Android/data/org.eukaryot.sonic3airnet/files/` (name `Sonic_Knuckles_wSonic3.bin`).

Android 6.0 (API 23) or newer, OpenGL ES 2. The APK holds the libraries for arm64-v8a, armeabi-v7a, x86_64 and x86; the phone picks its own.

## Names, folders, saves

- Application id `org.eukaryot.sonic3airnet` (the stock Sonic 3 A.I.R. app is `org.eukaryot.sonic3air`): both can be installed side by side and do not share saves.
  Saves, settings and the log live in `Android/data/org.eukaryot.sonic3airnet/files/` (visible from a PC over USB; removed when the app is uninstalled, so copy `saves` out first).
- The launcher icon is the game's own icon of the stock app (no new art).
- Not in the APK: the remastered soundtrack (`audioremaster.bin`, 130 MB). The game's own download entry in the options can fetch it as in the stock app.

## Controls

Touch controls (virtual gamepad) are on by default and can be moved in Options > Controls; Bluetooth/USB gamepads and keyboards work as on other platforms.
Typing the join code opens the Android keyboard. "COPY CODE" in the lobby copies the code to the clipboard.

## Networking (same as the Windows game)

- Host or join exactly as on Windows (Online Lobby, create lobby / join with code). The join code goes through the same matchmaking server.
- **Mobile data and CGNAT:** many mobile networks put phones behind carrier NAT that cannot be punched through, so a direct connection can fail.
  Wi-Fi is the reliable case; a phone on mobile data usually needs the relay (`Online.RelayServer`, see `DEPLOY_RELAY.txt`). The relay fallback is unchanged.
- Keep the screen on and the game in the foreground: Android pauses apps in the background and the other players will wait.
- Phone CPUs are ARM, the Windows game x86: the game logic is integer based and is meant to be identical, but a long cross-platform session has not been tested yet (see the handoff).

## Updates

Android cannot install an APK from inside the game. Options > System > Update > "Check for updates" asks the GitHub API for the newest release,
shows the new version number and, with the button, opens the release page in the browser (`github.com/<repo>/releases/latest`). Download the new APK and install it **over** the old one: saves are kept
**only if the new APK is signed with the same key** (see below). Nothing is downloaded or run by the game itself.

## Build

`checks\build_android.ps1 -ToolsDir <folder>` builds from scratch (clean start, exit code 0 on success). `-Abis arm64-v8a` builds one ABI only (about 2 minutes instead of about 8), `-DebugApk` a debug APK, `-Version x.y.z` overrides the version (default: newest local tag).

The tools folder is outside the repository and holds:

| what | where | notes |
|---|---|---|
| JDK 17 or 21 | `<ToolsDir>\jdk\<jdk folder>` | Gradle 8.14 does not run on Java 22+ (Temurin 21 was used) |
| Android SDK | `<ToolsDir>\sdk` | `sdkmanager "platforms;android-36" "build-tools;36.0.0" "ndk;29.0.14206865" "cmake;3.22.1"` and accept the Google licences |
| Gradle cache, short temp folder | `<ToolsDir>\gradle-home`, `<ToolsDir>\tmp` | created by the script. The temp folder must have a short path: with a long Windows temp path Gradle fails with "Unable to establish loopback connection" |
| signing key | `<ToolsDir>\keystore\airnet-release.jks` + `airnet-release.password.txt` | created on the first release build |

The script also needs the Windows build (`checks\build_online_stable.ps1`): its `-pack` option makes the data packages (`gamedata.bin`, `audiodata.bin`, `enginedata.bin`) and one start writes the compiled scripts (`scripts.bin`); these go into the APK assets with `config.json` and `data/metadata.json`, `data/font`.

Native code: `sonic3air\build\_android\app\jni\CMakeLists.txt` compiles everything with recursive globs, so every source file of the engine, netcode, relay transport and game is included automatically (the same sources as the Windows project, checked file by file).

Guards at the end of the build: `apksigner verify` must pass; no entry of the APK may be 4 MB or called like the ROM; no entry (including the compiled libraries) may contain the workspace path, the tools path or the Windows user name (the libraries are compiled with `-ffile-prefix-map`).

## Signing

Release APKs are signed with `<ToolsDir>\keystore\airnet-release.jks` (alias `airnet`, RSA 4096, valid 100 years, subject `CN=Sonic 3 AirNet`). It is created by the first release build and is never part of the repository; its password is in the text file next to it.
**Every later version must be signed with this same key**, otherwise Android refuses to install it over the old one (the user would have to uninstall first and loses the saves). Back up both files. If they are lost, a new key means a one-time uninstall for everybody.
This is a self-signed key for sideloading, not a Play Store upload key.

## Limits

- Not on Google Play; sideloading only. No in-app APK update (see above).
- `targetSdk 29` is kept from the stock project (needed for the legacy storage access the game's data folder uses); Android may show an "built for an older version" notice on the newest versions.
- Touch play of Sonic 3 is playable but a gamepad is better; the lobby is menu driven and works with the touch overlay or a gamepad.
- No remastered soundtrack in the APK.
