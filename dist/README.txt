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
Do not hand-edit swkotor2.ini for borderless setup. At startup the proxy
updates swkotor2.ini from dinput8.ini before the engine reads it:

Setting in dinput8.ini              Effect on swkotor2.ini
ForceWindowed=1 (default)           Sets FullScreen=0 in [Display Options]
                                    and [Graphics Options], and
                                    AllowWindowedMode=1 in [Graphics Options]
ForceWindowed=0                     Leaves those display-mode keys alone
Width / Height both > 0             Also writes [Graphics Options] Width and Height
Width=0 / Height=0 (default)        Leaves existing Width/Height in swkotor2.ini alone

Resolution (e.g. 2560x1440) otherwise stays in swkotor2.ini - set via in-game options.
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
