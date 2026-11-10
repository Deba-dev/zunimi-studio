## Zunimi Studio 0.2.0 (preview)

Zunimi Studio now has scenes, a full source editor, filters, an audio mixer, a replay buffer, hotkeys, and its own logo.

**Install:** download **ZunimiStudio-Setup.exe** below and run it. It installs just for you, so no admin prompt. Already on 0.1.1? The app offers this update by itself. If Windows SmartScreen warns you, click **More info > Run anyway** (the installer isn't code-signed yet).

### New
- **All your screens:** every monitor now has its own entry in **Add source** (for example "Screen 2: Built-in display"), plus **All screens, side by side**, laid out the way Windows arranges them. Before, the second screen had no name and covered the first one.
- **Scenes:** add, copy, rename, reorder and remove scenes. Switching uses a Fade, Cut, Swipe, Slide or Fade-to-colour transition with the length you choose. A copied scene shares its sources, so the same webcam or game appears in both. You can also add a source from another scene.
- **Move and resize in the preview:** click a source to select it, drag it to move (it snaps to the edges and the centre; hold Ctrl to place freely), and drag a corner to resize (hold Shift to stretch). Arrow keys nudge it. Right-click for quick layouts: fit to screen, stretch, centre, bottom-right corner, left or right half. You can also lock a source in place.
- **Every option:** **Options** shows everything a source can do, such as the capture method, cursor, game or window audio, camera resolution, text font and colour, media looping, and slideshow timing. Changes show live, and Cancel undoes them.
- **Filters:** green screen (chroma key), colour correction, crop, sharpen, LUT, image mask and scroll for pictures. Noise suppression, noise gate, compressor, gain, limiter, expander and 3-band EQ for sound, including on the mic and desktop audio.
- **More sources:** app audio (the sound of one app only), slideshow, video or sound file, colour background, and extra microphones or speakers.
- **Audio mixer:** live level meters for every source that has sound, with volume in dB, mute and filters.
- **Replay buffer:** keeps the last 5 to 600 seconds in memory. **Save replay** (button, tray or hotkey) writes the moment that just happened to an MP4. It can start with the app.
- **Hotkeys that work in games:** Go Live, Record, Save replay, Replay on/off, Mute mic, Mute desktop, and Switch to scene 1-9. They work while a fullscreen game has focus. Set them in **Settings > Hotkeys**.
- **Stays live on a bad connection:** when frames start dropping, Zunimi lowers the bitrate step by step (never below 40%), then raises it again once your internet is steady for a minute. You can switch it off in **Settings > Stream**.
- **Logo:** Zunimi Studio has its own icon on the window, taskbar, tray, Start menu and installer.

### Fixed
- The preview's empty edges were brown instead of the app's dark background.
- Quitting right after starting could, rarely, free a scene twice.

### Still in this preview
- **Game capture and anti-cheat (Valorant/Vanguard and others):** the capture hook is unsigned and may be blocked. Use a **Screen** source for those games until the signed hook ships.
- No vertical canvas, multistream, studio mode or browser source yet.

### Source
Zunimi Studio is free software under the GPL-2.0-or-later. It uses libobs from [OBS Studio 33.0.1](https://github.com/obsproject/obs-studio/tree/33.0.1), built with `scripts/build-obs.ps1` in this repo.
