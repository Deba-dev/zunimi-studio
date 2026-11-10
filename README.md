# Zunimi Studio

A streaming and recording studio for Windows, built on libobs (the OBS engine). It is meant to be as powerful as OBS, as easy as it gets for beginners, and light enough for low-end PCs. Every heavy feature can be switched off, and a switched-off feature is never loaded.

Status: **v0.1, first working build**. See [RESEARCH.md](RESEARCH.md) and [ENGINE.md](ENGINE.md) for the plan behind it.

## What works in 0.1
- **Auto-Tune:** on first launch it reads the CPU, RAM, GPU and hardware encoders, then picks **Potato** (720p30), **Balanced** or **Ultra** (1080p60). The person can change everything in Settings.
- **Sources:** Game (captures whichever game is fullscreen), Screen, Window, Webcam (placed in the corner), Image, Text. You can show or hide each one, move it up or down, change it, and remove it. The scene is saved automatically.
- **Audio:** desktop and mic volume and mute. Mic noise clean-up (RNNoise, light) is on by default.
- **Go Live** to Twitch, YouTube (RTMPS), Kick or any custom RTMP server. The stream key is encrypted with Windows DPAPI. The stream reconnects by itself after an internet drop.
- **Record** to Hybrid MP4, which survives a crash or power cut. Recording while live reuses the stream encoder, so it costs almost nothing extra.
- **Hardware encoders first:** NVENC, AMF or QuickSync, with x264 as the fallback. If a hardware encoder fails to start, the app switches to x264 by itself instead of failing.
- **Health bar:** green, amber or red, with the reason in plain words (GPU overloaded, encoder struggling, internet dropping frames) plus live app CPU, RAM and FPS.
- **Low-end extras:** the preview pauses while hidden, minimized or (on Potato) while live. Potato also uses lighter x264 settings that use about 28% less RAM.
- **Installer and updates (0.1.1):** `ZunimiStudio-Setup.exe` installs for the current user (no admin prompt). The app checks GitHub Releases a few seconds after start (can be switched off in Settings > About) and offers the update in a bar; it downloads the Setup .exe, checks its SHA-256, installs silently and reopens. It only ever downloads a Setup .exe from this repo's releases, and deletes downloaded installers on the next start.
- **Window modes:** Windowed, Borderless or Fullscreen. F11 toggles fullscreen. Esc and the close button send the app to the tray (each can be switched off). The window remembers its size. It asks before quitting while live or recording, and refuses to start twice.

## Measured on the dev PC (GTX 1660 Ti, 12 threads, 2026-11-10)
| | App CPU | App RAM |
|---|---|---|
| Idle, preview on | ~2% | ~155 MB |
| Recording 1080p60, NVENC (Ultra) | ~2.5% | ~300 MB |
| Recording 720p30, x264 (Potato) | ~2% | ~270 MB |

CPU is the app's share of the whole processor. On a 4-thread laptop expect roughly 3× these numbers.

## Build
Everything lives on `D:`.
1. Run `scripts\build-obs.ps1` once. It installs CMake 3.30 into `D:\zunimi-build\tools`, clones OBS 33.0.1, and builds libobs plus the 13 core plugins Zunimi loads. Expect about 20 minutes.
2. Run `scripts\build.ps1 [-Run]`. It builds Zunimi Studio and stages a runnable copy in `build\stage` (about 103 MB).

Self-tests:
- `zunimi-studio.exe --selftest` opens the app and quits.
- `--selftest-update` checks the update link rules, asks GitHub for releases and logs what it found.
- `--selftest-record` (with `ZUNIMI_DATA` pointing to a scratch folder) adds the screen, records 5 s and logs health.

## Release
1. Bump the version in `CMakeLists.txt` and `resources/app.rc`, and write `RELEASE_NOTES.md`.
2. `scriptsuild.ps1 -Package` builds `build\ZunimiStudio-<version>-windows-x64.zip`.
3. Commit, tag `v<version>`, push both, then `gh release create v<version> build\ZunimiStudio-<version>-windows-x64.zip --notes-file RELEASE_NOTES.md`.
4. The Installer workflow turns that zip into `ZunimiStudio-Setup.exe` and attaches it to the release. That file is what the in-app updater looks for.

App data is stored in `%LOCALAPPDATA%\ZunimiStudio` (settings, scene, the last 10 logs).

## Known gaps (next)
- **Game capture and anti-cheat:** our self-built `graphics-hook` DLLs are unsigned. Vanguard (Valorant) and other anti-cheats may block an unsigned hook. Before release we ship OBS's official signed hook binaries (GPL, redistributable).
- Simple mode only: no studio mode, transitions, multiple scenes or source filters UI yet.
- Self-healing (automatic quality step-down while live), native alerts and chat, vertical canvas, multistream and the Clip Studio bridge are planned modules (ENGINE.md).
- The look (graphite + cyan) is a placeholder until a visual identity is chosen.

## Licence
GPL-2.0-or-later, because Zunimi Studio links libobs. See [LICENSE](LICENSE). Paid tiers live outside this binary (cloud, AI and content), as described in ENGINE.md §4.
