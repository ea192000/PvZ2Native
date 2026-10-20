# PvZ2Native

📖 [Español](README_SPANISH.md) · **English**

**Runs the real native library of *Plants vs. Zombies 2* for Android (`libPVZ2.so`, ARM32) on PC.** The only thing that is emulated is the **CPU**: PvZ2 exists only compiled for ARM (there is no x86/x64 build), so a JIT translates its ARM instructions to x86_64. Everything else — Android, JNI, libc, OpenGL ES, the filesystem — **is not emulated, it is reimplemented natively**, the same way **Wine** does with Windows.

This repository is a customized branch of [OptiJuegos/PvZ2Native](https://github.com/OptiJuegos/PvZ2Native) with PC-focused improvements (in-game settings, keyboard shortcuts, controller support, quit confirm, and more). Upstream source: https://github.com/OptiJuegos/PvZ2Native

> [!WARNING]
> Experimental project. **The game is not included**: you must supply your own `libPVZ2.so` and your own `.obb`, obtained legally from the Android release.

---

## ✨ What's included in this build

Aimed at *Plants vs. Zombies 2: It's About Time* **v4.5.2** on Windows and Linux, with a smoother mouse / keyboard / controller experience.

### Visual and desktop integration
- Custom executable icon and custom cursor (scales with window/screen size).
- Startup splash and loading progress overlay (avoids a long black screen).
- On Windows, improved executable version metadata.

### In-game settings
- Open with `Esc` or from the main menu via the **Google Play** button (bottom-right).
- Change language, currency, fullscreen, monitor/display, FPS limit, free purchases, events, keyboard shortcuts, and controller options without editing files by hand.
- Most changes apply immediately (including moving the window to another monitor). Changing language, free purchases or the Events options relaunches the process so the engine reloads them.
- Older or incomplete `config.ini` files are healed on launch: missing keys get safe defaults; invalid values are corrected.

### Events
- Settings → **Events**: **Enable events** (`[runtime] use_cdn`) turns the emulated CDN that feeds events and daily content on or off; **Force local events** (`[runtime] rewrite_cdn`) re-copies the event files from the OBB at every launch. Either change relaunches the game.

### Achievements
- Local achievements unlocked while playing; progress is saved in `prefs.dat`.
- Animated unlock toast (with SFX); open the full list from settings (**View achievements**).
- Optional: turn off toast/SFX via settings or `[game] achievement_notifications = 0` — unlocks still save.

### Exit confirmation
- Confirm or cancel before quitting (mouse, keyboard, or controller).
- `Esc` opens the settings screen; pressing `Esc` again from settings opens this dialog (fixed, not remappable). `F11` always toggles fullscreen.

### Configuration
- First-run auto-detection for language, currency, and related values.
- Console/log window hidden by default (`[log] console = 0`).
- Everyday options belong in the settings screen; advanced keys remain available in `config.ini`.
- Command-line overrides for almost all options, e.g. `--fullscreen=0`, `--video.fullscreen=0`, `--console=1`.

### Keyboard and mouse
- Configurable shortcuts that simulate taps on screen (touch-era UI).
- Defaults include: `1`–`8` plant slots, `Space` speed, `Enter` pause, `F` Plant Food, `S` shovel, `Q`/`W`/`E` powers, `R` replay, `Backspace` back, plus almanac / connect / vasebreaker / zen binds.
- Edit shortcuts from the settings screen.
- Mouse-wheel list scrolling (touch-drag simulation), right-click = Back, middle-click = Menu where supported.
- Fullscreen / resize click mapping fixes and defensive input recovery.

### Controller
- Left stick moves the cursor; **A** confirms/clicks.
- D-pad jumps between on-screen controls and lawn cells; shoulders switch menu areas.
- Right stick drag (swipe-like); remappable actions (pause, powers, Plant Food, shovel, …).
- Stick speed and deadzone sliders; pick which pad to use or disable pads.
- Mouse, keyboard, and controller work together.

### Other
- More reliable text fields (click outside to leave typing mode).
- Improved frame-limiter pacing and quieter output when logging is off.
- External Help / License / Privacy links open in the system browser.
- Persistent per-installation player ID.

---

## 📖 What it is (and what it is NOT)

It is not a *port* of the game's source code. It is a **hybrid of two techniques**, and the distinction matters:

- **Only the CPU is emulated.** The game binary is ARM machine code and PvZ2 was never compiled for x86/x64, so there is no way to run it directly on a PC processor. A JIT (dynarmic) reads those ARM instructions and translates them to x86_64 on the fly. This is the only "emulated" part.
- **Android is NOT emulated: it is reimplemented.** When the game calls an Android function (open a file, draw with OpenGL ES, allocate memory, invoke Java…), that call is serviced by **native PC code** that does the real work using the equivalent desktop API. There is no virtual Android running underneath; there is a **reimplementation** of the functions the game needs.

This is exactly **Wine's** approach — *"Wine Is Not an Emulator"* — taken one step further: Wine reimplements Windows calls without emulating anything because `.exe` files are already x86; here, because the game is ARM, the CPU has to be emulated as well. The rest of the philosophy is identical: **translate calls, don't simulate a whole machine.**

```text
┌──────────────────────────────────────────────┐
│  libPVZ2.so  (ELF ARM32, EABI5, untouched)   │   ← the real Android game
└───────────────────────┬──────────────────────┘
                        │  ARM instructions
                        ▼
┌──────────────────────────────────────────────┐
│  dynarmic  —  JIT ARM32 → x86_64             │   ← ONLY emulated component
└───────────────────────┬──────────────────────┘     (the CPU only)
                        │  "Android" calls
                        ▼
┌──────────────────────────────────────────────┐
│  Compatibility layer (this project)          │   ← NATIVE reimplementation,
│                                              │     Wine-style (nothing emulated)
│  Fake JNI / JavaVM   ·   libc / libm / libz  │
│  OpenGL ES → OpenGL  ·   OpenSL ES → SDL     │
│  Own GuestHeap       ·   VFS (paths + .obb)  │
└───────────────────────┬──────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────┐
│  Windows / Linux x64  ·  SDL2  ·  OpenGL 2.0 │
└──────────────────────────────────────────────┘
```

When the game asks the operating system for something (open a file, allocate memory, draw, play a sound, call into Java), that call leaves the emulated code as a trap and is serviced by a *handler* in this layer, backed by the equivalent PC API.

---

## 🧩 Architecture

The code lives in [pvz2native/](pvz2native/) and is split into layers under one strict rule: **only one layer may contain concrete addresses from the `.so`.** That is what makes supporting another game version a matter of adding data, not rewriting logic.

| Layer | Question it answers | May it hold a `.so` address? |
| --- | --- | :---: |
| [runtime/](pvz2native/src/runtime/) | How to run ARM code (JIT, heap, threads, synchronization) | **Never** |
| [game/symbols.cpp](pvz2native/src/game/symbols.cpp) | Where each thing lives in *this* binary | **The only one that may** |
| [engine/](pvz2native/src/engine/) | What to call and in what order (boot, lifecycle, frame) | Only via `sym()` |
| [dependencies/](pvz2native/src/dependencies/) | How to answer each Android `.so` (libc, libm, GLES, …) | No |
| [dex/](pvz2native/src/dex/) | How to answer the Java side (fake JNIEnv + per-class hooks) | No |
| [diagnostics/](pvz2native/src/diagnostics/) | How to interrogate the guest (watchpoints, PC sampling) | Only via `sym()` |

Notable pieces:

- **JIT:** [dynarmic](dynarmic/) (A32 frontend only). One JIT per guest thread, with an identity *page table* over the `.so`'s memory for speed.
- **Native dependencies:** one module per Android `.so` the game links against. libc is split by header (`string`, `stdio`, `stdlib`, `time`, `pthread`, `locale`, `ctype`, …), plus `libm`, `libz`, `liblog`, `libgles`, `libstdcxx`, `libopensles`, `libdl`. Each handler is a `void(GuestCall&)` registered by name.
- **Java side:** [jni_env.cpp](pvz2native/src/dex/jni_env.cpp) implements a fake `JNIEnv`/`JavaVM` (objects, strings, arrays, references); [dex/hooks/](pvz2native/src/dex/hooks/) has one file per Java class (`AndroidGameApp`, `AndroidSurfaceView`, HTTP, Google Play, Facebook…).
- **Graphics:** the game's GLES calls are translated to **OpenGL 2.0 (compatibility profile)** via `glad`; SDL provides the window and GL context, playing the role of Android's `GLSurfaceView`.
- **Resources:** a VFS translates Android paths to PC paths and reads assets from the game's real `.obb` (RSB/RSG format).

---

## 🎮 Supported game versions

The version is detected by a **byte fingerprint** at two known native functions; if none matches, boot is refused rather than running blind.

| Version | Typical OBB | Status |
| --- | --- | --- |
| **1.6.10** (2013) | `main.7.com.ea.game.pvz2_na.obb` | 🟢 Playable |
| **4.5.2** (2016) | `main.147.com.ea.game.pvz2_row.obb` (often shipped / renamed as `data.obb`) | 🟢 Playable (primary target of this build) |

Adding a new version = one entry in `kVersions` in [symbols.cpp](pvz2native/src/game/symbols.cpp). Nothing in `runtime/` or `engine/` changes between versions.

---

## ⚙️ Requirements

- **(64-bit) Windows or Linux** (the executable is static x86_64; there is no 32-bit build — dynarmic only ships backends for x86_64/arm64/riscv64).
- **CMake** (3.5+ recommended).
- **Windows:** MinGW-w64 (GCC with C++20) **or** Visual Studio with C++ / CMake / Ninja (`compile-ninja.bat`).
- **Linux:** GCC or Clang with C++20 support.
- A **GPU with OpenGL 2.0** or higher.
- Your own copy of **`libPVZ2.so`** (armeabi-v7a) and the matching **`.obb`**.
- **Boost headers** under `third_party/boost_1_84_0` (as used by the build scripts).

---

## 🔨 Building

### Windows — Visual Studio + Ninja (recommended)

```bat
compile-ninja.bat
```

Uses `vswhere` to find Visual Studio (any edition), then builds a Release x64 binary with Ninja. Output:

```text
out\build\x64-Release\pvz2native\pvz2.exe
```

### Windows — MinGW

If `gcc`, `g++`, and `make` are on your `PATH`:

```bat
compile.bat
```

> [!NOTE]
> `compile-hardcoded.bat` is a machine-specific MinGW helper (absolute toolchain paths). Prefer `compile.bat` or `compile-ninja.bat`.

Output:

```text
build\pvz2native\pvz2.exe
```

### Linux

```bash
./compile.sh
```

Useful options: `--clean`, `--debug`, `--jobs N`, `--no-assets`, `--verbose`, `--help`.

Output:

```text
build/pvz2native/pvz2
```

`compile.sh` also copies host UI assets to `build/pvz2native/assets/` (splash, settings art, cursor, achievements). On Windows those files are embedded in the `.exe` via `windows_resources.rc`; on Linux they must sit next to the binary under `assets/`.

### Linux — AppImage

```bash
./compile-appimage.sh
```

Builds via `compile.sh`, then packs a type-2 AppImage. Output:

```text
build/pvz2native/pvz2.AppImage
```

The AppImage contains **only** the `pvz2` binary and `assets/`. Place `libPVZ2.so`, `data.obb`, and `config.ini` **next to** the `.AppImage` (same portable layout as the loose binary). First run may fetch `appimagetool` into `tools/` if it is missing.

---

## ▶️ Running

Place the Android game files **next to the executable** (not in a `lib/` subfolder). Defaults resolve to `libPVZ2.so` and `data.obb` in that same folder:

```text
<folder with pvz2 / pvz2.exe / pvz2.AppImage>/
├── pvz2(.exe)  or  pvz2.AppImage
├── config.ini                 ← created / healed on first launch
├── libPVZ2.so                 ← required (armeabi-v7a)
├── data.obb                   ← required (or set [paths] obb to your OBB name)
└── assets/                    ← Linux loose binary: UI/splash/cursor/achievements
                                   (Windows: embedded in the exe; AppImage: inside the image)
```

You can keep the original OBB filename and point to it in `config.ini`:

```ini
[paths]
so  = libPVZ2.so
obb = main.147.com.ea.game.pvz2_row.obb
```

Relative paths are resolved against the folder that contains the executable. Absolute paths are also accepted.

On first launch the host writes a documented `config.ini`. Everyday options are meant to be changed from the in-game settings screen; advanced keys stay in the file / CLI.

---

## 🔧 Configuration (`config.ini`)

Single source of truth for paths and debugging (legacy `PVZ2_*` environment variables are not read). Excerpt:

| Section | Key | What it does |
| --- | --- | --- |
| `[paths]` | `so`, `obb` | Paths to the `.so` and `.obb` (relative to the executable, or absolute). |
| `[paths]` | `save` | Optional save folder; unset stores `prefs.dat` next to the exe. |
| `[log]` | `console` | Show console/log output (default `0`). |
| `[log]` | `verbose` / `trace` / `input` | Boot / import / input diagnostics. |
| `[video]` | `fullscreen`, `display`, `width`, `height`, `fps_limit` | Display / monitor index (also in settings UI). |
| `[game]` | `user_locale`, `currency_*`, `emulate_iap` | Locale, store cosmetics, free IAP (also in settings UI). |
| `[game]` | `achievement_notifications` | Unlock toast + SFX (`1` default; `0` silences UI only). |
| `[runtime]` | `use_cdn`, `rewrite_cdn` | Emulated CDN on/off (default on) and forced re-seed from the OBB (also in settings UI → Events). |
| `[shortcuts]` | `bind_*` | Keyboard → on-screen tap (also in settings UI). |
| `[board]` / `[gamepad]` / `[gamepad_binds]` | … | Controller focus grid and binds (also in settings UI). |

---

## ⌨️ Controls (host)

| Action | Control |
| --- | --- |
| Tap / select | Left click (or controller **A**) |
| Drag (swipe) | Left click + move (or right stick) |
| Scroll lists | Mouse wheel |
| Back (menus) | Right click / `Backspace` / controller Back (works like Esc: opens settings, then quit confirm) |
| Settings → quit confirm | `Esc` (fixed): first press opens settings, second opens quit confirm |
| Toggle fullscreen | `F11` (fixed) |
| Settings | Google Play button on the main menu |
| Achievements list | From settings (**View achievements**) |
| Shortcuts / pad | Configurable in settings |

The window cannot be resized with the mouse (that would stretch the picture); pick the size in settings or `config.ini`, or use `F11` for fullscreen.

---

## 📊 Project status

| Area | Status |
| --- | --- |
| `.so` loading and engine boot | 🟢 |
| Lifecycle (init / surface / frames) | 🟢 |
| Resource loading (RSB/RSG from `.obb`) | 🟢 |
| Native imports (libc/libm/GLES/…) | 🟢 |
| Rendering | 🟢 |
| Audio (OpenSL ES → SDL) | 🟢 |
| Input (mouse / keyboard / controller) | 🟢 |
| In-game settings / quit confirm | 🟢 |
| Local achievements (list + toast) | 🟢 |
| Online services | 🔴 Not implemented (deliberately offline) |

---

## 📁 Project structure

```text
PvZ2Native/
├── pvz2native/            ← host / compatibility layer
│   ├── src/
│   │   ├── runtime/       ← JIT (dynarmic), GuestHeap, threads, sync, RSB
│   │   ├── dependencies/  ← one module per Android .so + VFS
│   │   ├── dex/           ← fake JNIEnv/JavaVM + per-class hooks
│   │   ├── engine/        ← boot / lifecycle / frame
│   │   ├── game/          ← symbols.cpp: addresses per binary version
│   │   ├── diagnostics/
│   │   ├── gfx/, audio/, input/, patch/, elf32/
│   │   ├── config.cpp / config_screen.cpp / achievements.cpp
│   │   └── main.c
│   ├── windows_resources.rc
│   └── include/pvz2native/
├── assets/                ← splash, settings UI, cursor, achievements (embedded on Windows; copied on Linux)
├── dynarmic/  SDL/  glad/  zlib/  stb/  third_party/
├── compile-ninja.bat      ← Windows VS + Ninja
├── compile.bat            ← Windows MinGW (PATH)
├── compile.sh             ← Linux
├── compile-appimage.sh    ← Linux AppImage (`pvz2.AppImage`)
├── CMakeLists.txt
└── README.md
```

---

## 📜 License

Code under the **MIT license** (see [LICENSE](LICENSE)). The project reuses third-party infrastructure (dynarmic, SDL, zlib, glad, stb), each with its own license.

**Plants vs. Zombies 2**, its trademarks and all its assets belong to **Electronic Arts / PopCap Games**. This project **does not distribute or replace** any game file: you must supply your own, obtained legally.

## ⚠️ Disclaimer

Independent project, **not affiliated with, endorsed by, or sponsored by** Electronic Arts, PopCap Games or any entity related to Plants vs. Zombies. Its purpose is strictly **technical and educational**: research into ARM binary emulation, compatibility layers, and API translation.

---

<p align="center">
  <b>PvZ2Native</b> — Running Android's <code>libPVZ2.so</code> on PC through ARM emulation
</p>
