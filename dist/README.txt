KOTOR II Borderless Proxy (dinput8.dll)
========================================

Install: copy dinput8.dll and dinput8.ini next to swkotor2.exe.

Modes (dinput8.ini)
-------------------
Mode=Fill
  Engine resolution + black borders (letterbox). Use Alignment=Centered, etc.

Mode=NoFill
  Same engine resolution and Alignment as Fill; no black backdrop.

Mode=Windowed
  Pure DirectInput proxy; no window or INI changes.

Legacy names still accepted: Stretch -> NoFill, Off -> Windowed.

swkotor2.ini
------------
Do not hand-edit swkotor2.ini for borderless setup. Use dinput8.ini for proxy options:
  ForceWindowed=1  -> proxy sets FullScreen=0, AllowWindowedMode=1 at startup
Resolution (e.g. 2560x1440) stays in swkotor2.ini - set via in-game options.
Optional: add Width= and Height= to dinput8.ini only to force an override.
Restart after changing resolution or dinput8.ini size/mode options.

Overlays (important)
--------------------
KOTOR II uses legacy OpenGL. In-game overlays that hook OpenGL can crash
after the proxy resizes the window (ACCESS_VIOLATION in nvoglv32.dll).

Disable these for swkotor2.exe before playing:

  - Discord overlay (including "Legacy Overlay" in Discord settings)
  - Steam in-game overlay (Steam -> swkotor2 Properties -> disable overlay)

If the game crashes, set EnableLog=1 in dinput8.ini and check dinput8.log beside swkotor2.exe.

Build
-----
msbuild dinput8.sln /p:Configuration=Release /p:Platform=x86
