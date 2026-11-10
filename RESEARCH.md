# Zunimi Studio — Pre-build Research

_Date: 2026-11-10 · Status: research only, no code yet_

> Confidence note: market facts come from web searches on 2026-11-10 plus prior knowledge.
> Items marked **(verify)** could not be confirmed against an official source.

---

## 1. The landscape — what exists and how it works

### How every streaming app works (the pipeline)

```
Sources ──► Compositor ──► Encoder ──► Output
(capture)    (GPU scene)    (H.264/HEVC/AV1)   (RTMP/SRT/WebRTC stream, file recording)
   ▲              ▲              ▲
 game/window/   scenes, layers, x264 (CPU) or
 screen/webcam/ filters, transitions NVENC / AMF / QuickSync (GPU)
 browser/audio
```

- **Capture:** Windows uses Game Capture (a DLL hook into DirectX/Vulkan/OpenGL games, fastest), Windows Graphics Capture (window/display), DXGI Desktop Duplication, DirectShow/Media Foundation (webcams), WASAPI (audio).
- **Compositing:** Every frame is drawn on the GPU (D3D11 on Windows) at canvas resolution, then scaled to output resolution.
- **Encoding:** This is the expensive part. A hardware encoder (NVENC/AMF/QSV) costs almost nothing on CPU. x264 on a weak CPU is the #1 cause of "Encoding overloaded".
- **Output:** RTMP to Twitch/YouTube/Kick, newer SRT/WHIP (WebRTC) for low latency, and MP4/MKV/Hybrid MP4 for recording.
- **Browser sources** (alerts, chat widgets) embed a full Chromium (CEF). Each one is a mini browser, and they are a hidden RAM/CPU cost on low-end PCs.

### The apps

| App | Engine / how it's built | Price | Strengths | Weaknesses |
|---|---|---|---|---|
| **OBS Studio** (v32.x) | `libobs` core in C, Qt UI, GPL-2.0 | Free, open source | Most powerful, light core, huge plugin ecosystem, all encoders, WHIP/SRT | Intimidating UI, cryptic errors, no built-in alerts/chat/multistream/vertical, needs plugins for basics |
| **Streamlabs Desktop** | Electron UI on top of `libobs` | Free + **Ultra** subscription | Themes, alerts, widgets, chat built in | Heavier (Electron + widget layer), key features paywalled (multistream, apps), cluttered, upsells |
| **PRISM Live Studio** (Naver) | Built on OBS/`libobs` | Free; some features moved to subscription from Jul 2025 | Clean UI, multistream, effects, beauty filters, mobile app | Ecosystem tied to Naver, some features now paid, Korea-focused |
| **Meld Studio** | Proprietary GPU-first engine | Free (beta-era) **(verify)** | Modern UI, built-in effects, free multistream, light | Closed source, smaller community, fewer integrations |
| **XSplit Broadcaster** | Proprietary, Windows only | Freemium/paid | Polished, analytics, moderation | Watermark/paywall, Windows only, declining mindshare |
| **Twitch Studio** | Twitch's own beginner app | Free | Simplest onboarding | Twitch only; reported discontinued **(verify)** |
| **Lightstream** | Cloud/browser studio | Subscription (~$7–19/mo, sources conflict) | Zero install, works on a Chromebook | Needs strong internet, latency, monthly cost, limited capture |

**Key insight:** Streamlabs and PRISM are skins over `libobs`. Nobody has really out-engineered OBS's core. They compete on **UI, built-ins and onboarding**. Being "lightweight" mostly comes from the **UI layer, browser sources and default settings**, not the capture core.

---

## 2. Feature inventory

### Table stakes (must have at launch)
- Scenes and sources: game, window and display capture, webcam, image, video, text, audio input/output
- Audio mixer with per-source volume, mute, monitoring, basic filters (noise gate, noise suppression, compressor)
- Hardware encoders (NVENC, AMF, QSV) plus x264 fallback
- Stream (RTMP/RTMPS) and record (MP4/MKV with crash-safe remux)
- Replay buffer (save the last N seconds on a hotkey)
- Hotkeys, studio mode (preview/program), transitions
- Stream key / OAuth login for Twitch, YouTube, Kick

### What competitors added on top
- Built-in alerts, chat, widgets (Streamlabs, PRISM)
- Multistreaming (PRISM, Meld; paid in Streamlabs)
- Vertical/dual canvas for Shorts/TikTok (OBS needs the Aitum Vertical plugin)
- WebRTC/WHIP and simulcast (OBS 32.1)
- Themes and overlay marketplaces
- Plugin manager (OBS 32, basic)
- Mobile remote control / phone as camera

### Newer features we could add (gaps nobody owns well)
1. **Auto-Tune engine.** A 20-second hardware benchmark on first launch picks encoder, resolution, FPS and bitrate. OBS's wizard exists but is shallow.
2. **Live self-healing.** When the encoder is overloaded or the network is unstable, step quality down automatically (60→30 fps, 1080→720, bitrate down) instead of dropping frames, then step back up. Show a plain-language toast: _"Your PC was struggling, so we lowered FPS to 30."_
3. **Stream Doctor.** Turns warnings like "Encoding overloaded" and "dropped frames" into causes and one-click fixes ("Your CPU is at 98% from x264 → Switch to NVENC [Fix]").
4. **Native alerts and chat overlay without Chromium.** Render alerts and chat with the GPU compositor instead of browser sources. This is a big RAM/CPU win on low-end PCs, and nobody does it as a default.
5. **Built-in vertical canvas.** One click gives a 9:16 layout that streams or records alongside landscape.
6. **Built-in free multistream.** Optional relay mode, so a weak upload link sends one stream to a relay that fans it out.
7. **Clip pipeline.** A "Mark moment" hotkey plus the replay buffer, sent straight to **Zunimi Clip Studio** for auto Shorts. This is the ecosystem advantage.
8. **Goal-based setup.** "What are you streaming? Gaming / Just chatting / Podcast / Tutorial" builds the scenes for you.
9. **Potato Mode.** One switch that turns the preview off (or limits it to 10 fps), uses a single canvas, hardware encoding only, 720p30, and pauses heavy filters.
10. **Unlimited undo and scene history.** Beginners break their setup; let them rewind it.
11. **Phone remote and phone-as-webcam** over LAN.
12. **Localization from day one**, including Hindi and Hinglish. The Indian creator market is underserved, and low upload speeds there make adaptive bitrate (point 2) and SRT especially valuable.
13. **Optional AI, off by default:** RNNoise noise suppression (cheap), auto captions, and background removal. Heavy models load only if the hardware passes a check.

---

## 3. What's lacking, and how we stand out

| Pain point (evidence) | Who suffers | Zunimi answer |
|---|---|---|
| "Encoding overloaded" and dropped frames are the most common OBS forum threads, even on strong PCs; fixes are trial and error | Beginners, low-end PCs | Auto-Tune, self-healing, Stream Doctor |
| OBS needs 3–6 plugins for alerts, chat, multistream and vertical | Everyone | All built in, native, free |
| Streamlabs runs heavier than OBS (Electron plus widgets) and paywalls features | Low-end PCs, free users | Native UI, no paywall on core streaming |
| Browser sources each spin up Chromium | Low-end PCs | Native-rendered overlays; browser source stays optional |
| Errors are technical jargon | Beginners | Plain-language messages and one-click fixes |
| Setup takes hours of tutorials | Beginners | Goal-based setup, ready in under 2 minutes |
| Cloud studios need fast internet and a subscription | India and other low-bandwidth regions | Local-first; adaptive bitrate; SRT |

**Positioning line:** _"The power of OBS, the ease of Twitch Studio, light enough for a 4 GB laptop."_

### Low-end target spec (proposal)
- **Minimum:** dual-core CPU from about 2015, 4 GB RAM, Intel HD 520-class iGPU with QuickSync, Windows 10.
- **Goals:** idle RAM under 150 MB, under 5% CPU overhead while streaming 720p30 with a hardware encoder, installer under 80 MB.

---

## 4. UI/UX principles
- **Two modes:** _Simple_ (the default: big Go Live button, scene cards, three sliders) and _Pro_ (docks, studio mode, advanced output). This is the same app with progressive disclosure, not two apps.
- **One health bar** (green/yellow/red) instead of five stats panels.
- **Drag and drop:** drop a webcam, image or window straight onto the canvas.
- **Every setting explains itself** in one line, with a "Recommended" badge.
- **Test stream before going live:** a private bandwidth and encoder test.
- **Visual identity:** to be defined, consistent with the ZunimiLabs family.

---

## 5. Build approach — the big decision

| Option | Effort | Lightweight | Licence impact | Verdict |
|---|---|---|---|---|
| **A. `libobs` core + our own native UI** (the PRISM/Streamlabs route) | Medium | Core is already efficient | **GPL-2.0**: the app must be open source; we can still sell cloud services, Pro features through a separate service, and the Clip Studio integration | **Recommended for v1** |
| B. Own engine in Rust (WGC/DXGI capture, wgpu compositor, FFmpeg/NVENC SDK encoding) | Very high: game-hook capture, A/V sync and encoders are years of work | Potentially lightest | Closed source possible (FFmpeg LGPL build) | Long-term option only |
| C. Electron/web UI on `libobs` | Low-medium | ❌ the Streamlabs problem | GPL | Avoid |

**UI toolkit for option A:** Qt 6 (native, light, what OBS uses) or a Rust/C++ shell with WebView2 (Tauri). WebView2 makes a modern UI much easier, but costs about 80–150 MB RAM, which conflicts with the 150 MB goal. Decide after a quick RAM prototype of both.

---

## 6. Decisions (2026-11-10)
1. Name: **Zunimi Studio** ✅
2. Engine: see [ENGINE.md](ENGINE.md). Proposed: libobs core + Qt 6 shell + switchable modules (GPL), awaiting OK.
3. Platform: **Windows only** for now ✅
4. Business: **free core + paid tiers** ✅ (tier split proposal in ENGINE.md §4)
5. App-wide: Windowed/Borderless/Fullscreen modes and a close-to-tray toggle ✅ (ENGINE.md §3)

---

## Sources
- OBS 32 release: https://betanews.com/2025/09/23/obs-studio-32-introduces-plugin-manager-new-formats-and-major-fixes/
- OBS 32 crash logs / Hybrid MOV: https://www.neowin.net/amp/obs-studio-32-launches-with-automatic-crash-log-uploads-hybrid-mov-support-and-more/
- OBS 32.1 audio mixer + WebRTC simulcast: https://www.gamingonlinux.com/2026/01/obs-studio-32-1-beta-is-out-with-a-new-audio-mixer-webrtc-simulcast-support-and-more
- OBS 32.2: https://www.neowin.net/amp/obs-studio-3220-final/
- OBS vs Streamlabs resource use: https://obs-versions.com/blog/streaming-software-comparison-2026 , https://obs-versions.com/compare
- Meld comparison (vendor): https://meldstudio.co/blog/obs-studio-vs-streamlabs-vs-meld-studio-which-streaming-software-in-2025/
- PRISM subscription change / specs: https://guide.prismlive.com/mobile-draft/guides/test-minimum-supported-specifications
- Lightstream pricing: https://toolradar.com/compare/lightstream-vs-xsplit , https://www.g2.com/compare/lightstream-studio-vs-xsplit-broadcaster
- Lightstream absorbing Prism multiplayer: https://golightstream.com/?p=11200
- Encoder overload threads: https://obsproject.com/forum/tags/encoder-overloaded/ , https://obsproject.com/forum/threads/help-11-09-2023-obs-drop-frames-at-30-cpu-usage.169544/latest
