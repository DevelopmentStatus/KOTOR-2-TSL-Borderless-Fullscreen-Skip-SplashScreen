# KOTOR II Borderless Proxy (dinput8.dll)

> **Important: restart after every resolution change**  
> If you change the in-game resolution, change `Width`/`Height` in `dinput8.ini`, switch monitors, or change other `dinput8.ini` settings that affect size or mode, **fully quit and restart the game**. The proxy applies window layout **once** at startup; changing resolution while the game is running will not update correctly and can break rendering or crash later.
>
> **Important: turn off overlays before playing**  
> Disable **Discord overlay** (including **Legacy Overlay** in Discord settings) and the **Steam in-game overlay** for `swkotor2.exe`.  
> KOTOR II uses legacy OpenGL; overlays that hook OpenGL can crash after the proxy resizes the window (often `ACCESS_VIOLATION` in `nvoglv32.dll`).  
> If the game still crashes, set `EnableLog=1` in `dinput8.ini` and check `dinput8.log` next to `swkotor2.exe`.
## Description

A **32-bit `dinput8.dll` proxy** for *Star Wars: Knights of the Old Republic II* (Steam `swkotor2.exe` build). The game loads this DLL for DirectInput; the proxy forwards input to the real `dinput8.dll` and optionally adjusts the game window for borderless fullscreen on modern monitors.

**Compatibility:** Built and tested on the **latest Steam version** of KOTOR II only. It has **not** been tested on GOG, disc, older Steam builds, or other releases.

- **Fill**: Keeps the engine’s configured resolution and adds black letterbox borders on the monitor (centered or aligned).
- **NoFill**: Same engine resolution and alignment as **Fill**, but no black backdrop (desktop shows in the letterbox area).
- **Windowed**: DirectInput proxy only; no window or INI changes.

The proxy waits until the engine’s main window is stable, then applies window changes **once** (no resize loop, no subclassing) to avoid desyncing the OpenGL viewport and crashing the renderer later. **You must restart the game** whenever you change resolution (in-game, via `dinput8.ini` `Width`/`Height`, or other relevant `dinput8.ini` options).

Optional features: hide the taskbar while focused, automatic windowed-mode and resolution setup for the engine (see below), skip BioWare/Obsidian splash screens (Steam build), and a debug console.

## Install

<img width="773" height="228" alt="{028A93E8-82D1-426B-BED3-8835D3E03A32}" src="https://github.com/user-attachments/assets/a9051e68-7378-4434-a571-4fe650b5d412" />

### Pre-built (recommended)

A pre-built copy of the DLL and INI file. Simply drag the two files into your Steam version of `...\steamapps\common\Knights of the Old Republic II` (the same folder as `swkotor2.exe`).

1. Turn off **Discord** and **Steam** overlays for `swkotor2.exe` (see above).
2. Get `dinput8.dll` and `dinput8.ini` from either:
   - the [latest release](https://github.com/DevelopmentStatus/KOTOR-2-TSL-Borderless-Fullscreen-Skip-SplashScreen/releases) (download the release assets), or
   - the `dist` folder in this repository.
3. Copy both files into your KOTOR II game directory (same folder as `swkotor2.exe`).
4. Edit `dinput8.ini` if you want a different mode or alignment (restart required after changes; see above).
5. Launch the game normally. After changing resolution in-game or in `dinput8.ini`, **exit completely and launch again**.

### Build from source

Requirements: Visual Studio 2022 (or compatible) with **Desktop development with C++** and **MSVC v143**, **x86** toolset.

```text
msbuild dinput8.sln /p:Configuration=Release /p:Platform=x86
```

Copy `Win32\Release\dinput8.dll` (or `dinput8\Win32\Release\dinput8.dll`) and `dist\dinput8.ini` next to `swkotor2.exe`.

## Configuration (`dinput8.ini`)

All settings live in `[Borderless]` next to `swkotor2.exe`. Legacy mode names are still accepted: `Stretch` → `NoFill`, `Off` → `Windowed`.

| Setting | Values | What it does |
|--------|--------|----------------|
| **Mode** | `Fill`, `NoFill`, `Windowed` | **Fill**: engine resolution + black borders on the monitor (letterbox). **NoFill**: same sizing/placement as Fill without the black backdrop. **Windowed**: pass-through only; no proxy window changes. |
| **Alignment** | `Centered`, `TopLeft`, `TopRight`, `BottomLeft`, `BottomRight`, `Top`, `Bottom`, `Left`, `Right` | Where the game window sits on the monitor in **Fill** / **NoFill** (default: `Centered`). |
| **HideTaskbar** | `0` or `1` | `1`: while the game has focus, raise the window (and Fill backdrop) above the taskbar; release when you alt-tab away. Fill mode no longer stays topmost when unfocused. |
| **ForceWindowed** | `0` or `1` | `1`: before startup, write `FullScreen=0` and `AllowWindowedMode=1` into `swkotor2.ini` (needed for borderless behavior). `0`: leave the game’s display-mode keys alone. |
| **Width** / **Height** | *(optional)* positive integers | Omit both (default): engine uses whatever is already in `swkotor2.ini` (e.g. from the in-game menu). Set both `>0` only if you want the proxy to overwrite `[Graphics Options]` at startup. |
| **EnableLog** | `0` or `1` | `1`: append diagnostics and crash reports to `dinput8.log` beside the exe. `0` (default): no log file. |
| **EnableConsole** | `0` or `1` | `1`: allocate a debug console for proxy logging. Does **not** remove the black backdrop in Fill mode. |
| **SplashScreens** | `0` or `1` | `1`: show BioWare/Obsidian logo splashes on startup. `0`: skip them (Steam `swkotor2.exe` build only). |
| **StretchViewport** | `0` or `1` | `1` (default): hook `glViewport`/`glScissor` to shift up and extend by the measured top chrome band. `0`: HWND/DWM fixes only. |

### Leave `swkotor2.ini` alone

You do not need to edit the game’s native `swkotor2.ini` for borderless or resolution setup. The proxy updates it automatically on each launch, before the engine reads it:

- With **`ForceWindowed=1`** (default), it sets `FullScreen=0` and `AllowWindowedMode=1` in the appropriate sections.
- **Resolution** is not managed in `dinput8.ini` by default — the engine reads `Width`/`Height` from `swkotor2.ini` (set once via the in-game options, or whatever is already there). Only add `Width`/`Height` to `dinput8.ini` if you explicitly want the proxy to override those keys.

Use **`dinput8.ini`** for borderless mode, alignment, taskbar, splashes, etc. If you change resolution in the in-game menu, **restart** after quitting so the proxy can re-apply window layout at startup.

Example defaults (see `dist/dinput8.ini`):

```ini
[Borderless]
Mode=Fill
Alignment=Centered
HideTaskbar=1
ForceWindowed=1
EnableLog=0
EnableConsole=0
SplashScreens=0
```

## Logs and troubleshooting

- **`dinput8.log`**: Set `EnableLog=1` to write diagnostics and crash reports beside `swkotor2.exe` (off by default).
- Wrong size, black bars, or glitches after changing resolution → **quit and restart**; do not expect hot-reload.
- Native resolution shows letterbox/crop (e.g. 1440 looks like ~1406) → the proxy expands the client to match `[Graphics Options]` Width/Height, uses `WS_POPUP`, DWM client bleed, and a top chrome band estimate; when filling the monitor it pins top-left. Restart after changes.
- Crashes right after resize → overlays still enabled, or another hook conflicting with OpenGL.
- Wrong size in NoFill → set `Width`/`Height` in `dinput8.ini` (or match what the in-game menu last wrote), then **restart**.

## License

See repository license. Not affiliated with Lucasfilm, BioWare, Obsidian, or Valve.

