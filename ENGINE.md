# Zunimi Studio — Engine Comparison & Architecture Decision

_Date: 2026-11-10 · Target: Windows only, low-end to high-end, every heavy feature can be switched off_

---

## 1. The three engines side by side

| | **libobs** (OBS core; also under Streamlabs and PRISM) | **Meld Studio engine** | **XSplit engine** |
|---|---|---|---|
| Origin | Open source since 2014, C | Written from scratch, closed source | Proprietary since about 2012, closed source |
| GPU API on Windows | **Direct3D 11** (plus an OpenGL backend) | **Direct3D 12** (Metal on Mac) | DirectX (not public) |
| Threads | Separate graphics, video/encode and audio threads; the graphics thread drives the encoder through a frame cache | Not public | Not public |
| Idle cost | Skips color conversion and frame downloads when nothing is encoding | Vendor says it is lower than OBS (unverified ~14% vs 26% CPU claim) | Not published |
| Official minimum spec | None official; runs on very old hardware | Not published | **8 GB RAM, 2nd-gen Core i5** (4th-gen i7 for 1080p) |
| Game capture | Mature DLL hook for DX9–12, Vulkan and OpenGL; anti-cheat friendly (whitelisted by most games) | Own implementation | Own implementation |
| Encoders | x264, NVENC, AMF, QSV, AV1, HEVC | NVENC, AMF, x264 fallback | NVENC, AV1, x264 |
| Plugins | Huge ecosystem; modules load as DLLs | None (built-ins only) | Paid extensions |
| Licence | **GPL-2.0** | n/a | n/a |
| Can we use it? | ✅ Yes | ❌ No | ❌ No |

### What each one teaches us
- **libobs:** The heavy lifting (capture, A/V sync, encoders) is solved and battle-tested on millions of PCs. Its weight comes from **what gets loaded around it**: the Qt UI, CEF browser sources and always-on preview. It is not slow at its core.
- **Meld:** A modern from-scratch engine *can* be lighter and look nicer. But it is DX12-first, and **many low-end iGPUs (Intel HD 4000–500 era) have weak or no DX12 support**. D3D11 is actually *better* for our low-end goal. It also has no plugins, and it took a funded team years to build.
- **XSplit:** Official minimums of 8 GB RAM and a Core i5 show that a polished closed engine is not automatically light. It is a model for polish and built-ins (background blur, multi-output), not for low-end.

### Building our own engine — what it would really take
| Piece | Difficulty | Notes |
|---|---|---|
| Display/window capture (WGC, DXGI) | Medium | Crates and samples exist |
| **Game capture hook** | **Very high** | DLL injection into DX9–12/Vulkan games; anti-cheat (Vanguard, EAC) may flag a new unknown hook. **This matters for Valorant streamers.** |
| GPU compositor | Medium-high | Scenes, filters, scaling, color formats |
| A/V sync and timing | High | The thing that quietly breaks every new engine |
| Encoders | Medium | NVENC/AMF/QSV SDKs via FFmpeg |
| Output (RTMP/SRT/WHIP, reconnect) | Medium | |
| **Total** | **Years** | Before even reaching OBS parity |

**Conclusion:** A from-scratch engine gains maybe 5–10% CPU in the best case, and costs years plus anti-cheat risk. Not worth it for v1.

---

## 2. Decision: libobs core + Zunimi shell, built as switchable modules

```
┌──────────────────────── Zunimi Studio (our app) ────────────────────────┐
│  UI shell: C++ Qt 6 (QML, GPU drawn)   ~40–70 MB idle                   │
│  ├─ Simple mode / Pro mode                                              │
│  ├─ Stream Doctor, Auto-Tune, self-healing controller                   │
│  └─ Settings › Modules: each one on/off, shows its RAM/CPU cost         │
├──────────────────────── libobs (unchanged core) ────────────────────────┤
│  graphics thread (D3D11) · encode thread · audio thread                 │
├─────────────── Modules (DLLs loaded ONLY when switched on) ──────────────┤
│  Always:   game/window/display capture, webcam, audio, encoders, RTMP   │
│  Optional: Native overlays (alerts/chat, NO Chromium)   ← ours, light   │
│            Browser source (CEF, ~100 MB+)               ← off by default│
│            Vertical canvas · Multistream · Replay buffer · Studio mode  │
│            Noise suppression (RNNoise light / NVIDIA Broadcast heavy)   │
│            AI: captions, background removal (only if hardware passes)   │
│            Phone remote · Clip Studio bridge                            │
└─────────────────────────────────────────────────────────────────────────┘
```

### How "light for low-end, full for high-end" works
1. **Lazy modules.** A module that is switched off is never loaded: zero RAM, zero CPU. libobs already loads plugins as separate DLLs, so we only control *when*.
2. **Performance profiles.** Auto-Tune picks one on first launch, and the user can change it any time:

| Profile | For | Defaults |
|---|---|---|
| 🥔 **Potato** | 4 GB RAM, iGPU | 720p30, hardware encode only, preview at 10 fps or off, browser source off, AI off |
| ⚖️ **Balanced** | Mid-range | 1080p30/60, preview on, native overlays on |
| 🚀 **Ultra** | High-end | 1080p60/1440p, vertical canvas plus multistream, AI on, browser sources on |

3. **Nothing is locked by profile.** A Potato user can still switch on any module. The app just shows the cost first ("Browser source: about 120 MB RAM, your PC has 1.1 GB free").
4. **Self-healing** works within the profile. When frames drop, it lowers FPS, resolution or bitrate, then recovers.

> **Update (2026-11-10, v0.1):** the shell uses **Qt Widgets, not QML**. libobs draws its preview into a native
> window handle, which a Widget gives directly (QML needs workarounds), and Widgets skips the QML engine's ~20–30 MB.
> Built against OBS **33.0.1** with VS 2022 Build Tools; OBS 33 installs modules as `core/<name>/<name>.dll`.

### Why Qt 6 for the UI (not Tauri or Electron)
| Toolkit | Idle RAM | Modern UI ease | Verdict |
|---|---|---|---|
| **Qt 6 QML (C++)** | ~40–70 MB | Good: GPU-drawn, animations | ✅ Chosen; shares the Qt runtime libobs tooling already uses |
| Tauri / WebView2 | ~80–150 MB | Easiest | ❌ Conflicts with the 150 MB idle goal |
| Electron | ~200–400 MB | Easy | ❌ The Streamlabs problem |
| PySide6 (Python, like Clip Studio) | ~70–110 MB | Good | ⚠️ Possible, but adds Python overhead to an always-running app |

### GPL — what it means for free core plus paid tiers
- The Zunimi Studio app (linked with libobs) is **open source under GPL**.
- Paid tiers stay safe when the paid value is **outside the GPL binary**:
  - **Cloud services:** relay multistream, cloud scene backup, AI captions on our servers.
  - **Separate programs that talk to it:** Clip Studio, the overlay and theme marketplace.
  - **Content:** premium overlay packs, alert sounds and themes (assets are not code).
- A license-key check inside GPL code is legal, but anyone could remove it, so it should only gate convenience, never the business.

---

## 3. App-wide window behaviour (every ZunimiLabs app)

| Setting | Options | Default |
|---|---|---|
| **Window mode** | Windowed · Borderless (frameless, custom title bar) · Fullscreen (F11) | Windowed |
| **On close (X)** | Minimize to system tray · Quit fully | Minimize to tray |
| **Esc key** | First exits Fullscreen; otherwise minimizes to tray (can be turned off) | On |
| **Tray icon menu** | Show · Go Live/Stop · Start/Stop recording · Save replay · Quit | — |

Rules:
- If the user chooses **Quit fully while live or recording**, always confirm: "You're live, stop the stream and quit?"
- The first time the app hides to the tray, show a one-time toast: "Zunimi Studio is still running in the tray. Change this in Settings › General."
- Window mode, size and monitor are remembered between launches.

---

## 4. Tiers (proposal; prices decided later)

| Free core (everything to stream well) | Paid tiers (cloud, AI, content) |
|---|---|
| All capture, all encoders, recording, replay buffer | Cloud relay multistream (one upload, many platforms) |
| Auto-Tune, self-healing, Stream Doctor | AI captions and translations (incl. Hinglish) |
| Native alerts and chat overlays | AI background removal (cloud or local Pro models) |
| Vertical canvas | Clip Studio bundle (auto Shorts from "Mark moment") |
| Local multistream (2 platforms) | Premium overlay, theme and alert packs |
| Simple/Pro modes, all window/tray options | Cloud scene backup and sync across PCs |

Payments through **ZunimiLabs Pay / Dodo**, the same as Clip Studio.

---

## Sources
- libobs backend design (threads, encoder handoff): https://docs.obsproject.com/backend-design
- Meld engine claims (vendor): https://meldstudio.co/obs-to-meld/ , https://meldstudio.co/blog/meld-studio-vs-obs-the-complete-2026-comparison/ , https://meldstudio.co/blog/how-to-use-hardware-encoding-for-live-streaming/
- Meld third-party view: https://obs-versions.com/alternatives/meld-studio
- XSplit official requirements: https://support.xsplit.com/en/article/xsplit-broadcaster-system-requirements-1pn5ek5/
- XSplit features: https://www.xsplit.com/broadcaster , https://support.xsplit.com/en/article/the-xsplit-broadcaster-window-11xy8ib/
