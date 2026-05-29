# KOTOR II Borderless Proxy (dinput8.dll)

<font color="red">

> **Important — restart after every resolution change**  
> If you change the in-game resolution, edit `swkotor2.ini` resolution, switch monitors, or change `dinput8.ini` settings that affect size or mode, **fully quit and restart the game**. The proxy applies window layout **once** at startup; changing resolution while the game is running will not update correctly and can break rendering or crash later.
>
> **Important — turn off overlays before playing**  
> Disable **Discord overlay** (including **Legacy Overlay** in Discord settings) and the **Steam in-game overlay** for `swkotor2.exe`.  
> KOTOR II uses legacy OpenGL; overlays that hook OpenGL can crash after the proxy resizes the window (often `ACCESS_VIOLATION` in `nvoglv32.dll`).  
> If the game still crashes, check `dinput8.log` next to `swkotor2.exe`.

</font>

## Description

A **32-bit `dinput8.dll` proxy** for *Star Wars: Knights of the Old Republic II* (Steam `swkotor2.exe` build). The game loads this DLL for DirectInput; the proxy forwards input to the real `dinput8.dll` and optionally adjusts the game window for borderless fullscreen on modern monitors.

- **Fill** — Keeps the engine’s configured resolution and adds black letterbox borders on the monitor (centered or aligned).
- **NoFill** — Same engine resolution and alignment as **Fill**, but no black backdrop (desktop shows in the letterbox area).
- **Windowed** — DirectInput proxy only; no window or INI changes.

The proxy waits until the engine’s main window is stable, then applies window changes **once** (no resize loop, no subclassing) to avoid desyncing the OpenGL viewport and crashing the renderer later. **You must restart the game** whenever you change resolution (in-game, in `swkotor2.ini`, or relevant `dinput8.ini` options).

Optional features: hide the taskbar while focused, force windowed mode in `swkotor2.ini`, skip BioWare/Obsidian splash screens (Steam build), and a debug console.

## Install

### Pre-built (recommended)

1. Turn off **Discord** and **Steam** overlays for `swkotor2.exe` (see above).
2. Copy from the `dist` folder into your KOTOR II game directory (same folder as `swkotor2.exe`):
   - `dinput8.dll`
   - `dinput8.ini`
3. Edit `dinput8.ini` if you want a different mode or alignment (restart required after changes — see above).
4. Launch the game normally. After changing resolution in-game or in `swkotor2.ini`, **exit completely and launch again**.

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
| **Mode** | `Fill`, `NoFill`, `Windowed` | **Fill** — engine resolution + black borders on the monitor (letterbox). **NoFill** — same sizing/placement as Fill without the black backdrop. **Windowed** — pass-through only; no proxy window changes. |
| **Alignment** | `Centered`, `TopLeft`, `TopRight`, `BottomLeft`, `BottomRight`, `Top`, `Bottom`, `Left`, `Right` | Where the game window sits on the monitor in **Fill** / **NoFill** (default: `Centered`). |
| **HideTaskbar** | `0` or `1` | `1` — while the game has focus, raise the window (and Fill backdrop) above the taskbar; release when you alt-tab away. Fill mode no longer stays topmost when unfocused. |
| **ForceWindowed** | `0` or `1` | `1` — patch `swkotor2.ini` so the engine starts in windowed mode (needed for borderless behavior). |
| **EnableConsole** | `0` or `1` | `1` — allocate a debug console for proxy logging. Does **not** remove the black backdrop in Fill mode. |
| **SplashScreens** | `0` or `1` | `1` — show BioWare/Obsidian logo splashes on startup. `0` — skip them (Steam `swkotor2.exe` build only). |

Example defaults (see `dist/dinput8.ini`):

```ini
[Borderless]
Mode=Fill
Alignment=Centered
HideTaskbar=1
ForceWindowed=1
EnableConsole=0
SplashScreens=0
```

## Logs and troubleshooting

- **`dinput8.log`** — Written beside `swkotor2.exe` when something goes wrong or logging is enabled.
- Wrong size, black bars, or glitches after changing resolution → **quit and restart**; do not expect hot-reload.
- Crashes right after resize → overlays still enabled, or another hook conflicting with OpenGL.
- Wrong size in NoFill → confirm `swkotor2.ini` resolution settings match what you want, then **restart**.

## License

See repository license (if provided). Not affiliated with Lucasfilm, BioWare, Obsidian, or Valve.
