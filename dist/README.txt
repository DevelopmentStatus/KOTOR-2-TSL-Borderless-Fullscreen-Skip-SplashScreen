KOTOR II Borderless Proxy (dinput8.dll)
========================================

Install: copy dinput8.dll and dinput8.ini next to swkotor2.exe.

Modes (dinput8.ini)
-------------------
Mode=Fill
  Engine resolution + black borders (letterbox). Use Alignment=Centered, etc.

Mode=NoFill
  Full monitor, no black borders. Use ForceMonitorResolution=1.

Mode=Windowed
  Pure DirectInput proxy; no window or INI changes.

Legacy names still accepted: Stretch -> NoFill, Off -> Windowed.

Overlays (important)
--------------------
KOTOR II uses legacy OpenGL. In-game overlays that hook OpenGL can crash
after the proxy resizes the window (ACCESS_VIOLATION in nvoglv32.dll).

Disable these for swkotor2.exe before playing:

  - Discord overlay (including "Legacy Overlay" in Discord settings)
  - Steam in-game overlay (Steam -> swkotor2 Properties -> disable overlay)

If the game crashes, check dinput8.log beside swkotor2.exe for details.

Build
-----
msbuild dinput8.sln /p:Configuration=Release /p:Platform=x86
