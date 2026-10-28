DesktopTV - your PC's desktop on Jodi's TV and PC, in The Killing Antidote
=============================================================================

What it does
------------
- Jodi's TV shows a monitor of your PC, live (play the "!Desktop" video on the TV).
- Jodi's PC screen shows the same monitor (it replaces her wallpaper: the game's own, or a
  wallpaper mod such as PCWallpaperQuiet).
- PC mode (Insert): your mouse and keyboard work on that monitor while you watch it in game.
  A cursor is drawn on Jodi's screen, her own PC interface is hidden, the game stays in front,
  and the cursor starts where the game's cursor was (and goes back to the same spot).
  Insert again = back to the game.
- Sound: programs on that monitor (YouTube in a browser...) fade with Jodi's distance to her PC
  and to the TV, and come from the left or right depending on where the camera looks.
  Outside her office they are muted; when the game closes, everything is given back.
- Outside Jodi's office it does nothing at all (no capture, no checks, Insert left alone).

It is a ReShade add-on (DesktopTV.addon64) plus a small UE4SS Lua script (DesktopTVPos).
It only works on what the game draws; nothing in the game's own files is changed.

What you need
-------------
1. ReShade 6.8.0 WITH FULL ADD-ON SUPPORT (reshade.me, the second download button).
   Install it on TheKillingAntidote\Binaries\Win64\TheKillingAntidote-Win64-Shipping.exe,
   DirectX 10/11/12. The normal ReShade build will NOT work.
2. UE4SS 3.0.1 (for the sound-follows-Jodi part). Without it everything else works and the sound
   simply plays while the TV shows the desktop or PC mode is on.
3. A second screen to show in game. With a single monitor the TV just shows the game itself.
   A free virtual monitor works perfectly:
   - Download the control app:
     https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/25.7.23/VDD.Control.25.7.23.zip
     (NOT a "Driver.Only" file, NOT "Source code").
   - Extract, run "VDD Control.exe", click "Install Driver", accept the Windows prompts.
   - Windows Display settings: "Extend these displays", keep your real monitor as the main
     display, set the virtual screen to 1920x1080.
   - Uninstall it from the same app before any big graphics-driver update.

Install
-------
Copy the contents of the "TheKillingAntidote" folder in this package into the game's
"TheKillingAntidote" folder (the one that contains Binaries, Content, Videos...):
  Binaries\Win64\DesktopTV.addon64                      the add-on (next to dxgi.dll)
  Binaries\Win64\DesktopTV_spots.txt                    where Jodi's PC and TV are in her office
  Binaries\Win64\Mods\DesktopTVPos\Scripts\main.lua     the UE4SS script
  Binaries\Win64\Mods\DesktopTVPos\enabled.txt
  Videos\TV\!Desktop.mp4                                the marker video for the TV

Settings
--------
Start the game once and close it: the add-on adds a [DesktopTV] section to
Binaries\Win64\ReShade.ini. Replace that section with the one in "DesktopTV settings.ini"
(the settings that worked best here). Edit ReShade.ini with the game CLOSED (it is read at
start, and ReShade may rewrite the file on exit).

  Monitor=2          which screen to show (ReShade.log lists every monitor, numbered)
  FPS=30             desktop updates per second (15 lighter, 60 for 60 fps video)
  SRGB=2             TV colours like the game's own TV videos (or force 0 / 1)
  TVOpaque=1         makes the desktop opaque for the TV (without it: washed out, too bright)
  TVGamma=100        TV contrast curve in percent (120 = darker mid-tones, 80 = lighter)
  Width / Height     size of the marker video (1264 x 720 - keep)
  PCScreen=1         show the desktop on Jodi's PC screen
  PCSRGB=1           colours on her PC screen (2 = automatic also works)
  PCWidth / PCHeight size of a wallpaper-mod picture like PCWallpaperQuiet (1920 x 1080 - keep);
                     the game's own wallpaper is recognised automatically
  PCModeKey=45       PC mode key (Windows key codes: 45 = Insert, 35 = End)
  HideUIKey=0        extra key to hide Jodi's PC interface (145 = Scroll Lock, 0 = none)
  HideUI=0           1 = her interface hidden from the start
  HideUIInPCMode=1   hide her interface while PC mode is on
  KeepFront=1        keep the game in front during PC mode
  CursorScale=1.25   size of the cursor drawn on her screen (1 = like Windows; decimals ok)
  CursorOutline=0    1 = dark outline around that cursor (only 0 or 1)
  Sound=1            control the sound of programs on the second screen
  Spatial=1          sound follows Jodi (needs the DesktopTVPos script)
  NearCm=150         full volume within 1.5 m of her PC / the TV
  FarCm=1200         silent beyond 12 m
  SoundOutside=0     outside her office: 0 = second-screen programs muted, 1 = normal
  PCCorner=0         keep at 0 (experimental, not working)
  PCScreenLeft/Top, PCScreenTRx/TRy, PCScreenRight/Bottom, PCScreenBLx/BLy
                     where the 4 corners of her screen are in the game window when she leans in
                     (see "Cursor calibration")

Using it
--------
- Before playing: put what you want to watch (YouTube...) on the second screen.
  Select its window and press Win+Shift+Right (or Left) to send it there; the other arrow
  brings it back. Lost a window? Win+P -> "PC screen only" pulls everything back,
  Win+P -> "Extend" restores the second screen.
- TV: in Jodi's office, switch the TV to the "!Desktop" video. If the TV shows the word
  DESKTOP, the add-on is not running (check ReShade is the add-on version).
- Jodi's PC: her screen shows the desktop all the time. Interact with her PC (she leans in),
  press Insert: your cursor appears, her interface hides, mouse and keyboard work on your
  desktop. Insert again to come back. If the game ever leaves the screen anyway
  (Alt+Tab...), the cursor is released onto your main screen.
- The add-on catches Insert before other programs see it (in Jodi's office only).

Cursor calibration (so PC mode starts where the game's cursor is)
------------------------------------------------------------------
"DesktopTV settings.ini" already holds Benja's calibration for a 1920x1080 game window.
If PC mode puts your cursor in the wrong place, redo it: lean in at Jodi's PC, put the game's
cursor exactly on each corner of the visible screen picture and press
  Ctrl+Shift+F8 = top-left    Ctrl+Shift+F10 = top-right
  Ctrl+Shift+F9 = bottom-right    Ctrl+Shift+F11 = bottom-left
ReShade.log confirms each corner ("4 corners set (tilt corrected)" when done).

Sound positions
---------------
DesktopTV_spots.txt already holds the PC and TV spots for Jodi's office. To set them yourself:
stand at Jodi's desk and press Ctrl+F8 (PC), stand in front of the TV and press Ctrl+F9 (TV).
The script only works on maps that have spots. The add-on takes over the volume of
second-screen programs while you are in her office and gives it back when you leave or when
the game closes; after a crash, the next start gives it back.

Logs (if something does not work)
---------------------------------
  Binaries\Win64\ReShade.log   lines starting with "DesktopTV:" (should say "v5.0 loaded")
  Binaries\Win64\UE4SS.log     lines starting with "[DesktopTVPos]" ("v5 loaded", then
                               "following Jodi via ..." once in her office)

Linux (Steam / Proton)
----------------------
The same add-on works under Proton with a small Linux helper (picture, PC mode and sound on
KDE Plasma 6). Everything is in the "Linux" folder; start with Linux\HOW_TO_DesktopTV_Linux.txt.

Building the add-on yourself (optional - a ready DesktopTV.addon64 is included)
---------------------------------------------------------------------------------
"Source" holds the add-on source (src\desktoptv_addon.cpp), the ReShade 6.8.0 add-on SDK
headers (reshade_include, from github.com/crosire/reshade, BSD-3/MIT) and build.bat.
Needs Visual Studio 2022 or newer with the C++ tools ("Desktop development with C++",
the free Build Tools are enough). Double-click build.bat -> build\DesktopTV.addon64.

Version history
---------------
1.2 (add-on v5.0, 2026-10-28)
  - Your browser no longer gets stuck in one ear: DesktopTV always starts from, and gives
    back, an even left/right, and repairs any program it ever panned when the game starts
    and closes.
  - The game keeps its sound in PC mode (Alt+Tab still mutes it as before).
1.1 (add-on v4.9, 2026-10-25)
  - Works with the game's own wallpaper on Jodi's PC (PCWallpaperQuiet no longer needed).
1.0 (add-on v4.8, 2026-10-24)
  - First release.
