# Zunimi Studio

A streaming and recording studio for Windows, built on libobs (the OBS engine). It is meant to be as powerful as OBS, as easy as it gets for beginners, and light enough for low-end PCs. Every heavy feature can be switched off, and a switched-off feature is never loaded.

Status: **v0.3 preview**. See [RESEARCH.md](RESEARCH.md) and [ENGINE.md](ENGINE.md) for the plan behind it, and [RELEASE_NOTES.md](RELEASE_NOTES.md) for what's new.

## What works (0.3)
- **Auto-Tune:** on first launch it reads the CPU, RAM, GPU and hardware encoders, then picks **Potato** (720p30), **Balanced** or **Ultra** (1080p60).
- **Scenes:** add, copy, rename, reorder, remove, scenes inside scenes, **studio mode** (edit, then Go), **scene collections**, **profiles**, **import from OBS Studio** or an overlay pack's .json. Transitions: Fade, Cut, Swipe, Slide, Fade to colour, Luma wipe, **Stinger**, and a different one per scene.
- **Editing:** select, drag (snaps), resize, **Alt+drag to crop**, rotate, flip, an exact **size and place** dialog, copy/paste size and place, blend modes, scaling filter, show/hide effects, lock. **Undo/redo** (Ctrl+Z/Ctrl+Y) for all of it.
- **Sources:** Game, every screen (and all screens side by side), Window, Webcam, App audio, Image, Slideshow, Video or sound file (with play/pause/restart), Text, Colour, extra mics and speakers, another scene, and Zunimi's own **alert box, chat box, goal bar, labels, timers and recent events**. A **browser source** add-on for web pages and StreamElements/Streamlabs overlays.
- **Filters** for picture and sound; **mixer** with meters, volume, mute, and per-source **headphone listening, sync offset, balance and tracks**. Push to talk and push to mute.
- **Outputs:** Go Live over RTMP/RTMPS, SRT, RIST or **WHIP**; HEVC and AV1 where the GPU can; stream delay; **multistream** to more platforms; **vertical canvas** (9:16) stream and recording; recording in crash-safe MP4/MOV, MKV and more, with **pause**, **split**, **separate sound tracks** and its own quality; **replay buffer**; **virtual camera**; screenshots; projectors.
- **Alerts and chat:** Twitch (chat, subs, gifts, bits, raids anonymously; follows with a login), YouTube (chat, Super Chats, memberships, no login), Kick (chat, subs, gifts), StreamElements and Streamlabs (tips and their events). A **notification builder** with live preview, sounds, GIFs, text to speech and bigger-amount versions. Chat and events panels beside the mixer.
- **Helper:** pre-flight check before Go Live, **Stream Doctor** fixes, alarms for a silent mic or a black stream, an after-stream report, automatic scene switching by the app in front, a **privacy guard** for private windows, **Mark moment** chapters, a laptop battery warning.
- **Global hotkeys** that work in fullscreen games (Go Live, record, pause, replay, mute, push to talk, scenes 1-9, studio Go, virtual camera, screenshot, mark moment, undo).
- **Health bar** in plain words, self-healing bitrate, x264 fallback when a hardware encoder fails.
- **Window modes** (Windowed, Borderless, Fullscreen with F11), tray, and Esc/quit that always ask first.
- **Installer and updates** from GitHub Releases (SHA-256 checked). Crashes write where they happened to the log.

## Measured on the dev PC (GTX 1660 Ti, 12 threads, 2026-11-10)
| | App CPU | App RAM |
|---|---|---|
| Idle, preview on | ~2% | ~155 MB |
| Recording 1080p60, NVENC (Ultra) | ~2.5% | ~300 MB |
| Recording 720p30, x264 (Potato) | ~2% | ~270 MB |

CPU is the app's share of the whole processor. On a 4-thread laptop expect roughly 3× these numbers.

## Build
Everything lives on `D:`.
1. Run `scripts\build-obs.ps1` once. It installs CMake 3.30 into `D:\zunimi-build\tools`, clones OBS 33.0.1, and builds libobs, the 14 core plugins Zunimi loads and the browser source add-on (Chromium is downloaded by OBS's CMake). Expect about 30 minutes.
2. Run `scripts\build.ps1 [-Run]`. It builds Zunimi Studio and stages a runnable copy in `build\stage` (about 120 MB).

`scripts\build.ps1 -Browser` also puts the browser source add-on into `build\stage` (it is built by `build-obs.ps1`
into `D:\zunimi-build\obs-studio\build_zunimi\rundir`).

Self-tests (run with `ZUNIMI_DATA` pointing to a scratch folder; each logs `ok`/`FAIL` lines):
- `--selftest` opens the app and quits.
- `--selftest-edit`: undo/redo, rotate, flip, crop, nested scenes, studio mode, collections, import, profiles, screenshot, a paused MKV recording with two tracks.
- `--selftest-live`: chat and alert parsing for Twitch, YouTube, Kick, StreamElements and Streamlabs, a real anonymous Twitch chat connection, alert and chat overlays drawing, text to speech.
- `--selftest-assist`: pre-flight, fixes, multistream settings, Mark moment chapters.
- `--selftest-vertical`: the vertical canvas follows the scene on air, has its own sources, records, saves.
- `--selftest-browser` (with the add-on and `ZUNIMI_TEST_FILE` or `ZUNIMI_TEST_URL`): the browser source draws a page.
- `--selftest-scenes`, `--selftest-screens`, `--selftest-record`, `--selftest-update` as before. `--demo-dialogs` opens dialogs for screenshots.

## Release
1. Bump the version in `CMakeLists.txt` and `resources/app.rc`, and write `RELEASE_NOTES.md`.
2. `scripts\build.ps1 -Package` builds `build\ZunimiStudio-<version>-windows-x64.zip` (the app with its browser source).
3. Commit, tag `v<version>`, push both, then `gh release create v<version> build\ZunimiStudio-<version>-windows-x64.zip --notes-file RELEASE_NOTES.md`.
4. The Installer workflow turns the zip into `ZunimiStudio-Setup.exe` (what the in-app updater looks for), attaches it and deletes the zip. A finished release has only the .exe.

App data is stored in `%LOCALAPPDATA%\ZunimiStudio` (settings, scene, the last 10 logs).

## Known gaps (next)
- **Game capture and anti-cheat:** our self-built `graphics-hook` DLLs are unsigned. Vanguard (Valorant) and other anti-cheats may block an unsigned hook. Before release we ship OBS's official signed hook binaries (GPL, redistributable).
- **Twitch app id:** follow alerts and typing in Twitch chat need a Twitch app registered for Zunimi (public client, device login); set it with `-DZUNIMI_TWITCH_CLIENT_ID=...` or in Settings > Connections > Advanced.
- **Kick follows and Ko-fi** need webhooks, so a small relay (Cloudflare Worker) is planned. **UPI tips** wait on a payment provider that allows tips.
- **Planned:** overlay store and partner packs, WebSocket remote (Stream Deck), phone remote, Simple/Pro modes and goal-based setup, Hindi UI, optional AI (captions, background removal), Clip Studio bridge beyond the moments log, groups.
- The app look (graphite + cyan) is still provisional.

## Licence
GPL-2.0-or-later, because Zunimi Studio links libobs. See [LICENSE](LICENSE). Paid tiers live outside this binary (cloud, AI and content), as described in ENGINE.md §4.
