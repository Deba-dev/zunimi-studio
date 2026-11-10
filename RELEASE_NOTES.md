## Zunimi Studio 0.1.1 (preview)

Zunimi Studio now has a real installer and updates itself.

**Install:** download **ZunimiStudio-Setup.exe** below and run it. It installs just for you, so no admin prompt. Windows 10/11, 64-bit, and a DirectX 11 graphics card. If Windows SmartScreen warns you, click **More info > Run anyway** (the installer isn't code-signed yet).

### New
- **Installer:** Start menu entry, desktop shortcut and a normal uninstall from Windows Settings. Your settings, scene and recordings are kept when you update or uninstall.
- **Updates:** a few seconds after start, Zunimi Studio checks for a newer version. If there is one, a bar offers **Update now**: it downloads the installer, checks it is genuine and complete, installs it and reopens. You can't update while you're live or recording. Turn the check off, or check by hand, in **Settings > About this PC**.
- **Cleans up after itself:** downloaded installers are deleted after the update, and an update replaces the old program files instead of mixing them.

### Still in this preview
- **Game capture and anti-cheat (Valorant/Vanguard and others):** this build's capture hook is unsigned and may be blocked. Use **Screen** capture for those games until the signed hook ships.
- Only one scene so far.
- Coming from 0.1.0 (the zip)? Install this once with the Setup .exe. From now on the app updates itself. You can delete the old unzipped folder.

### Source
Zunimi Studio is free software under the GPL-2.0-or-later. It uses libobs from [OBS Studio 33.0.1](https://github.com/obsproject/obs-studio/tree/33.0.1), built with `scripts/build-obs.ps1` in this repo. A portable zip is attached too.
