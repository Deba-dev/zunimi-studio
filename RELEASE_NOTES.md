## Zunimi Studio 0.1.0 (preview)

This is the first working build: a light streaming and recording studio for Windows, built on the OBS engine.

**Install:** unzip anywhere and double-click **Start Zunimi Studio.cmd**. Nothing else is needed. Windows 10/11, 64-bit, and a DirectX 11 graphics card.

### What's in it
- **Auto-Tune:** on first launch it checks your PC and picks Potato (720p30), Balanced or Ultra (1080p60). You can change everything in Settings.
- **Sources:** Game (captures whichever game is fullscreen), Screen, Window, Webcam, Image, Text.
- **Audio:** desktop and mic volume and mute, plus a light mic noise clean-up.
- **Go Live:** Twitch, YouTube, Kick or any RTMP server. Your stream key is encrypted, and the stream reconnects after an internet drop.
- **Record:** MP4 files that survive a crash or power cut.
- **Hardware encoders first:** NVIDIA, AMD or Intel. If one fails to start, the app falls back to the CPU encoder.
- **Health bar:** tells you in plain words when your GPU, encoder or internet is struggling.
- **Low-end friendly:** only the parts it needs are loaded, the preview pauses while hidden, and Potato uses lighter encoder settings.
- **Window:** Windowed, Borderless or Fullscreen (F11). Esc and the close button send it to the tray, and you can turn each off.

### Known limits
- **Game capture and anti-cheat (Valorant/Vanguard and others):** this build's capture hook is unsigned and may be blocked. Use **Screen** capture for those games until the signed hook ships.
- There's no installer or auto-update yet, and only one scene.

### Source
Zunimi Studio is free software under the GPL-2.0-or-later. It uses libobs from [OBS Studio 33.0.1](https://github.com/obsproject/obs-studio/tree/33.0.1), built with `scripts/build-obs.ps1` in this repo.
