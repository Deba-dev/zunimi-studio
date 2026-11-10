## Zunimi Studio 0.3.0 (preview)

The big one: everything OBS does for editing and recording, plus alerts, chat and widgets built in (no browser needed), a vertical canvas, multistream, and a helper that watches your stream for you.

**Install:** download **ZunimiStudio-Setup.exe** below and run it. Already on 0.2? The app offers this update by itself. If Windows SmartScreen warns you, click **More info > Run anyway** (the installer isn't code-signed yet).

### Editing
- **Undo and redo** (Ctrl+Z, Ctrl+Y) for moving, resizing, cropping, adding, removing, renaming and scene changes. Removing a scene can be undone too.
- **Crop:** hold Alt and drag a corner. **Rotate and flip** from the right-click menu (Ctrl+R turns 90°).
- **Edit size and place:** exact position, size, rotation, crop and fill mode, with live preview.
- **Copy size and place** from one source to another (Ctrl+C, Ctrl+V).
- **Blend modes** (screen, multiply, add...), **scaling** (sharp pixels, Lanczos, area), and a **show/hide effect** per source.
- **A scene inside another scene.** Play, pause and restart video and sound files from the right-click menu.

### Scenes
- **Studio mode:** edit a scene on the left while the right shows what's on air, then press **Go**.
- **Scene collections** (whole sets of scenes) and **profiles** (stream key, quality and recording settings), from **Menu**.
- **Import from OBS Studio**, or an overlay pack's scene collection (.json). File paths that pointed at someone else's PC are found again.
- **Stinger transitions** (your own video), luma wipe, and a different transition per scene.
- **Projectors:** what's on air (or one scene or source) fullscreen on another screen, or in a window.

### Sound
- **Listen in headphones** per source (also "only I hear it"), **sync offset**, **balance**, and **recording tracks** 1-6 (the **⋯** button in the mixer).
- **Push to talk** and **push to mute** hotkeys.

### Recording and output
- **Formats:** crash-safe MP4 and MOV, MKV, classic MP4/MOV, FLV, MPEG-TS. **Pause** without ending the file. **A new file every N minutes or MB.** File names from a template ({scene}, date and time).
- **Separate sound tracks** in recordings (game, mic, Discord apart for editing).
- **Recording quality of its own** (high, near lossless, smaller) with HEVC or AV1 when your graphics card can.
- **HEVC and AV1 streaming**, rate control, speed vs quality, keyframe interval.
- **SRT, RIST and WHIP (WebRTC)** besides RTMP. **Stream delay.** **Record automatically while live.**
- **Virtual camera:** what's on air as a webcam in Discord, Zoom or Meet.
- **Screenshots** of what's on air, a scene or a source.
- **Multistream:** the same stream to several platforms at once (costs upload, not CPU).

### Alerts, chat and widgets (no browser source needed)
- **Alert box** with a **notification builder:** title with {name}/{amount}, picture or GIF, sound, animation, layout, fonts, colours, a minimum amount, bigger-amount versions, and **text to speech** (Windows voices, Hindi included when installed). Four looks to start from; designs can be exported and shared.
- **Chat box** and a **chat panel** with Twitch, YouTube and Kick together. Typing into Twitch chat once you log in.
- **Goal bar, labels** (latest follower, top tipper...), **timers** (countdown, subathon, clock, time live) and a **recent events** list.
- **Connections:** Twitch (chat, subs, gift subs, bits and raids with just your name; follows with a login), YouTube (chat, Super Chats, memberships, no login), Kick (chat, subs, gifts), StreamElements and Streamlabs (tips through UPI, cards and PayPal, plus their events).
- **Browser source** for StreamElements and Streamlabs overlay links and any web page. It comes with the installer but only starts once you use it, so it costs no memory until then.

### Zunimi extras
- **Vertical canvas** (9:16 for Shorts, Reels, TikTok): it follows the scene on air, can have its own webcam and widgets, and records or streams beside the landscape one.
- **Pre-flight check** before Go Live: empty scene, muted mic, low disk space, CPU encoding when your GPU could.
- **Stream Doctor:** when the health bar shows a problem, a **Fix it** button does the fix.
- **Alarms** while live: the mic has been silent for minutes, or the stream went black.
- **After-stream report:** bitrate, drops, the roughest minute, and what to change.
- **Automatic scene switching** by the app in front (for example Valorant > "Gaming").
- **Privacy guard:** screen captures hide while a password manager, banking or stream-key window is in front.
- **Mark moment** hotkey: a chapter in the recording ("chapters.txt" for YouTube) and a saved replay. A moments log for Clip Studio.
- **Battery warning** for laptops.
- **Crash reports:** a crash writes where it happened to the log.

### Changed
- **Esc asks before hiding** the app to the tray (Cancel is the default), and quitting asks too. "Don't ask again" is in the question and in Settings > Window.

### Still in this preview
- **Game capture and anti-cheat (Valorant/Vanguard and others):** the capture hook is unsigned and may be blocked. Use a **Screen** source for those games until the signed hook ships.
- **Twitch follow alerts and typing in Twitch chat** need Zunimi's Twitch app id, which isn't set in this build yet (Settings > Connections > Advanced takes your own).
- **YouTube chat** is read from YouTube's own live chat page, which YouTube can change at any time.

### Source
Zunimi Studio is free software under the GPL-2.0-or-later. It uses libobs from [OBS Studio 33.0.1](https://github.com/obsproject/obs-studio/tree/33.0.1), built with `scripts/build-obs.ps1` in this repo.
