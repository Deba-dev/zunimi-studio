# Zunimi Studio

A streaming and recording studio for Windows, built on libobs (the OBS engine). It is meant to be as powerful as OBS, as easy as it gets for beginners, and light enough for low-end PCs. Every heavy feature can be switched off, and a switched-off feature is never loaded.

Status: **v0.2 preview**. See [RESEARCH.md](RESEARCH.md) and [ENGINE.md](ENGINE.md) for the plan behind it.

## What works (0.2)
- **Auto-Tune:** on first launch it reads the CPU, RAM, GPU and hardware encoders, then picks **Potato** (720p30), **Balanced** or **Ultra** (1080p60). The person can change everything in Settings.
- **Scenes:** add, copy (sources shared), rename, reorder, remove. Transitions: Fade, Cut, Swipe, Slide, Fade to colour, with a chosen length. A source can be added from another scene.
- **Sources:** Game (whichever game is fullscreen), every screen by name plus "All screens, side by side" (laid out like Windows arranges them), Window, Webcam, App audio (one app's sound), Image, Slideshow, Video or sound file, Text, Colour background, extra microphones and speakers.
- **Edit in the preview:** select, drag (snaps to edges and centre; Ctrl places freely), resize from a corner (Shift stretches), arrow keys nudge, right-click for fit/stretch/centre/corner/half layouts, lock in place.
- **Options for everything:** a properties editor built from what libobs reports for each source or filter (check boxes, numbers, sliders, lists, text, files, colours, fonts, buttons, file lists, groups). Changes apply live; Cancel restores.
- **Filters:** chroma key, colour correction, crop, sharpen, colour/luma key, image mask, LUT, scroll, scale, delay; noise suppression, noise gate, compressor, gain, limiter, expander, 3-band EQ, upward compressor, invert polarity. Mic and desktop audio filters are saved too.
- **Audio mixer:** level meters (peak with hold) for every source with sound, volume in dB, mute, filters. Mic noise clean-up (RNNoise) is on by default.
- **Go Live** to Twitch, YouTube (RTMPS), Kick or any custom RTMP server. The stream key is encrypted with Windows DPAPI. Reconnects by itself, and **self-healing** lowers the bitrate step by step when frames drop (never below 40%), raising it again after a steady minute.
- **Record** to Hybrid MP4, which survives a crash or power cut. **Replay buffer** (5-600 s) with Save replay from the button, tray or a hotkey.
- **Global hotkeys** that work while a fullscreen game has focus: Go Live, Record, Save replay, Replay on/off, Mute mic/desktop, Switch to scene 1-9.
- **Hardware encoders first:** NVENC, AMF or QuickSync, with x264 as the fallback when a hardware encoder fails to start.
- **Health bar:** green, amber or red, with the reason in plain words, plus live app CPU, RAM and FPS.
- **Low-end extras:** the preview pauses while hidden, minimized or (on Potato) while live. Potato uses lighter x264 settings.
- **Window modes:** Windowed, Borderless or Fullscreen (F11). Esc and the close button send the app to the tray (each can be switched off).
- **Installer and updates:** `ZunimiStudio-Setup.exe` installs for the current user (no admin prompt). The app checks GitHub Releases after start, downloads the Setup .exe (only from this repo, SHA-256 checked), installs silently and reopens.
- **Logo:** `resources/logo.svg` is the source; `scripts\build.ps1 -Icon` renders `zunimi.ico` and the PNGs.

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
- `--selftest-update` checks the update link rules, asks GitHub for releases and logs what it found. With `ZUNIMI_PRETEND_VERSION=0.0.1` it also downloads and verifies the newest installer (never runs it).
- `--selftest-scenes` adds, copies, shares and removes scenes and saves a replay.
- `--selftest-screens` lists and captures every screen.
- `--selftest-record` (with `ZUNIMI_DATA` pointing to a scratch folder) adds the screen, records 5 s and logs health.

## Release
1. Bump the version in `CMakeLists.txt` and `resources/app.rc`, and write `RELEASE_NOTES.md`.
2. `scripts\build.ps1 -Package` builds `build\ZunimiStudio-<version>-windows-x64.zip`.
3. Commit, tag `v<version>`, push both, then `gh release create v<version> build\ZunimiStudio-<version>-windows-x64.zip --notes-file RELEASE_NOTES.md`.
4. The Installer workflow turns that zip into `ZunimiStudio-Setup.exe` and attaches it to the release. That file is what the in-app updater looks for.

App data is stored in `%LOCALAPPDATA%\ZunimiStudio` (settings, scene, the last 10 logs).

## Known gaps (next)
- **Game capture and anti-cheat:** our self-built `graphics-hook` DLLs are unsigned. Vanguard (Valorant) and other anti-cheats may block an unsigned hook. Before release we ship OBS's official signed hook binaries (GPL, redistributable).
- Native alerts and chat, vertical canvas, multistream, studio mode and the Clip Studio bridge are planned modules (ENGINE.md).
- The app look (graphite + cyan) is still provisional; the logo follows the ZunimiLabs Z (cyan > blue > violet).

## Licence
GPL-2.0-or-later, because Zunimi Studio links libobs. See [LICENSE](LICENSE). Paid tiers live outside this binary (cloud, AI and content), as described in ENGINE.md §4.
