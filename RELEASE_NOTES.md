## Zunimi Stream+ 0.5.3 (preview)

Drag your scenes and sources, and Go Live to sits right under Start Streaming.

**Install:** download **ZunimiStudio-Setup.exe** below and run it. Already on 0.5? The app offers this update by itself. If Windows SmartScreen warns you, click **More info > Run anyway** (the installer isn't code-signed yet).

### Drag and drop
- **Scenes:** drag a scene up or down to change the order. Scene hotkeys 1-9 follow the new order.
- **Sources:** drag a source higher to put it in front, lower to put it behind. You can drag several selected sources at once. A source moves among its neighbours; use right-click > Group to put it in a group.
- **Ctrl+Z** undoes a drag. The ↑ ↓ buttons still work.

### Controls
- **Go Live to** (every platform, with its switch and status light, plus Add destination) is now inside the **Controls** dock, under Start Streaming, instead of a tab of its own.
- The Controls dock scrolls when it is short, so no button is ever cut off. **Stats** takes just the room its tiles need, so Controls gets more. If you had your own layout, this happens once and the rest of your layout stays.
- Chat and Events can be made shorter than before.
- All of it in Hindi too.

## Zunimi Stream+ 0.5.2 (preview)

Sign in with YouTube is switched on.

**Install:** download **ZunimiStudio-Setup.exe** below and run it. Already on 0.5? The app offers this update by itself. If Windows SmartScreen warns you, click **More info > Run anyway** (the installer isn't code-signed yet).

### YouTube streams
- **Sign in with YouTube** (Go Live to > Pick stream, or **Tools > Your YouTube streams**) lists your scheduled and live YouTube streams. Pick one and Zunimi fills in its server and stream key for you, so there is nothing to copy and paste.
- The sign-in is **read-only**: Zunimi never posts, uploads, edits or deletes anything on your channel, and your sign-in stays on your PC. **Sign out** removes it. Details: [privacy policy](https://zunimilabs.online/zunimi-stream/privacy/).
- Google is still reviewing the app, so the sign-in page may say "Google hasn't verified this app". Click **Continue** to go on.

## Zunimi Stream+ 0.5.1 (preview)

Go live to several platforms at once, landscape and vertical together, plus fixes from the first streams on 0.5.0.

**Install:** download **ZunimiStudio-Setup.exe** below and run it. Already on 0.5.0? The app offers this update by itself. If Windows SmartScreen warns you, click **More info > Run anyway** (the installer isn't code-signed yet).

### Go Live to: multistream and vertical together
- A new **Go Live to** panel (next to Stats) shows your main stream and every extra platform, each with a status light (live, connecting, reconnecting, failed) and a switch that works **while you're live**, for just that platform.
- Every platform is **Landscape 16:9** or **Vertical 9:16**. Vertical ones get the vertical canvas, which switches itself on, so YouTube Shorts, TikTok and Instagram can all get the vertical picture while your normal stream goes to YouTube or Twitch.
- **+ Add destination** has ready settings for Twitch, YouTube, YouTube Shorts, Kick, Facebook, TikTok and Instagram. If you had set up a vertical stream before, it is now in this list.

### YouTube streams (coming)
- **Pick stream** (Go Live to) and **Tools > Your YouTube streams** will list your scheduled and live YouTube streams after you sign in, and send your stream to the one you pick. The Google sign-in is switched on in a coming update; until then, paste your key in Settings > Stream as before.

### Fixes
- **Buttons answer at once:** Start/Stop Streaming and Recording show Connecting, Stopping, Starting or Saving right away instead of waiting a few seconds.
- **Go Live card:** the "Go Live. Be Legendary." card is now a button that starts or ends the stream and shows how long you've been live. The Controls dock can no longer squeeze it out of sight.
- **Your dock layout is kept** even if the app closes unexpectedly: it is saved within a few seconds of every change.
- **YouTube chat shows up faster:** new messages are fetched at least every 2 seconds (it could be 10 before).
- **A crash while live is less likely:** opening or changing a Window, Game or Screen capture's options while live could crash the graphics driver. Those options no longer touch the running capture.
- All of it in Hindi too.

## Zunimi Stream+ 0.5.0 (preview)

A new look and a window you can arrange your way: every panel is now a dock, like in OBS.

**Install:** download **ZunimiStudio-Setup.exe** below and run it. Already on 0.4? The app offers this update by itself. If Windows SmartScreen warns you, click **More info > Run anyway** (the installer isn't code-signed yet).

### Docks you can move
- **Scenes, Sources, Audio Mixer, Scene Transitions, Controls, Stats, Chat and Events** are docks. Drag one by its title to any side, drop it on another to make tabs, pop it out as its own window (great for a second screen), resize it or close it.
- Your layout is remembered. **Docks** menu: show or hide each dock, **Lock docks** so nothing moves by accident, **Full-height side docks**, and **Reset docks** to go back to the default.
- Simple mode hides the Pro docks and brings them back where you left them.

### New look
- Purple and blue neon glass, with a new **Neon** accent (Violet, Sunset, Ocean and Mint are still in Settings > Window).
- A real menu bar: File, Edit, View, Docks, Profile, Scene Collection, Tools and Help.
- **Controls:** Start Streaming, Start Recording, Virtual Camera, Studio Mode, replay buffer, Settings and Exit in one place.
- **Stats:** live and recording timers, CPU and FPS.
- An eye on each source to show or hide it.
- All of it in Hindi too.

## Zunimi Studio 0.4.0 (preview)

Hindi, a remote for your phone and Stream Deck, ready-made scenes, a first-time setup, game capture that anti-cheat games accept, and a new look.

**Install:** download **ZunimiStudio-Setup.exe** below and run it. Already on 0.3? The app offers this update by itself. If Windows SmartScreen warns you, click **More info > Run anyway** (the installer isn't code-signed yet).

### Hindi
- **Settings > Window > Language: हिन्दी.** Every screen, message and button, and OBS's own options for sources and filters. Zunimi restarts once to switch.
- Scene templates put their titles on stream in Hindi too.

### Remote control
- **Stream Deck, Touch Portal, Streamer.bot, Loupedeck:** Zunimi speaks the same remote language as OBS (obs-websocket 5), so their OBS actions work as they are. Settings > Remote, then connect them to 127.0.0.1, port 4455 and the password shown.
- **Your phone as a remote:** turn on "Allow phones and PCs on my Wi-Fi" and scan the QR code. Go live, record, replay, mark moments, mute, switch scenes, in English or Hindi.
- A password is always needed; after five wrong tries a device has to wait.

### Easier start
- **First-time setup:** where you stream and your key, what viewers see (game, screen, webcam, alerts, chat), and your OBS scenes if you have them.
- **Simple mode** shows just what you need to go live and record; **Pro mode** shows every tool. Switch from Menu.
- **Scene templates:** Starting soon (with a countdown), Be right back, Stream ending, Gaming and Just chatting, in Neon, Clean and Sunset styles. Scenes list > **+** > From a template. More packs will appear there from zunimilabs.online.

### Game capture with anti-cheat
- Game capture now uses OBS Studio's own capture hook, signed by the OBS Project, so Valorant (Vanguard) and other anti-cheat games accept it like they accept OBS.

### Alerts
- **Kick follows and Kicks, and Ko-fi tips and memberships** (Settings > Connections), through the ZunimiLabs relay. These switch on as soon as the relay is live; you'll see it in the app.

### Also new
- **Groups:** select several sources (Ctrl-click), right-click > Group. They move, hide and lock together.
- **Clip Studio bridge:** in Clip Studio, set the OBS address to Zunimi (127.0.0.1, port and password from Settings > Remote). Every recording becomes a stream to clip, and every **Mark moment** becomes a clip.
- **A new look,** the same as zunimilabs.online: blue-black, with a Violet, Sunset, Ocean or Mint accent (Settings > Window).
