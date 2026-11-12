The Sims Bustin' Out - native Windows port (static recompilation)
==================================================================

This is the original Xbox game's code, translated instruction-by-instruction
into C and compiled as a 64-bit Windows program with the xboxrecomp toolkit.
No emulator runs underneath it. NO GAME DATA IS INCLUDED: you supply your own
disc image of The Sims Bustin' Out (Xbox, USA).

FIRST RUN
---------
1. Put SimsBustinOut.exe in its own folder.
2. Do ONE of these:
     - drag your .iso onto SimsBustinOut.exe, or
     - put the .iso in the same folder and double-click the exe, or
     - double-click the exe and pick the .iso in the window that opens.
3. The game checks that the image really is The Sims Bustin' Out (USA)
   (title ID 45410043) and unpacks it into a "game" folder next to the exe
   (about 2 GB). This happens once. You can delete the .iso afterwards.

To reinstall from a different image, drag that image onto the exe again.

CONTROLS
--------
An Xbox-style controller (XInput: Xbox 360/One/Series pads) works as-is.
Keyboard (controller 1):
  Arrows = D-pad          W A S D = left stick     I J K L = right stick
  Z = A   X = B   C = X   V = Y   Q = White   E = Black
  1 = left trigger   3 = right trigger   Enter = Start   Backspace = Back

SAVES
-----
Saves go to the "saves" folder next to the exe (change it in settings.ini).

GRAPHICS
--------
The game draws with your graphics card through Direct3D 11 (any card from
roughly 2008 on: Direct3D 10.0 or newer). If none is found it falls back to
the slower software renderer by itself; the first lines of sims_bustinout.log
say which one is in use. To force the software renderer, set SIMS_GPU=0 in
settings.ini.

GRAPHICS MENU (F6)
------------------
Press F6 in the game window. Arrow keys select and change, F6 closes.
  Internal resolution  1x-4x (640x480 up to 2560x1920), drawn on your GPU
  Frame rate           30 (original) or 60 (experimental); also caps the menus
  Left stick           EXPERIMENTAL: "Moves your Sim" turns on direct
                       movement (see below). Off by default.
    Camera distance    how far behind your Sim the camera sits
    Camera angle       how steeply it looks down
    Camera aim         how high on your Sim it aims
    Camera follow      how quickly it swings round behind her when she
                       walks (Off, Slowly, Normal, Quickly)
  Show FPS             a frame-rate counter in the corner
  Dump textures        saves every texture the game loads, once, as
                       textures\dump\<name>.png next to the exe
  Replace textures     loads textures\replace\<name>.png instead, if it
                       exists. Copy a dumped file there, keep its name,
                       edit it or upscale it (any size works).
  Reload replacement   picks up new or edited files without restarting
Choices are saved in settings.ini. Higher resolutions need a faster GPU.

DIRECT MOVEMENT (EXPERIMENTAL)
------------------------------
With F6 > Left stick set to "Moves your Sim":
  Left stick    walks your Sim, relative to the camera. She steers round
                furniture and walls; doors open as she reaches them and
                close behind her. Moving the stick while she is busy
                cancels what she is doing (so she can get out of the bath).
  Right stick   left/right orbits the camera round her, up/down zooms.
  A             if something is lit up (glowing gold) under her, opens its
                menu as usual. Otherwise it lists everything she can use
                nearby, in the game's own bubbles; pick one with the stick
                and A.
The cursor is hidden; only the plumbob shows. Build/buy mode and menus keep
the normal cursor. Known limits: holding the stick sideways makes her curve
as the camera follows; the nearby list can reach through walls; in very
cramped spots the game may refuse a route.

SETTINGS
--------
Edit settings.ini (window scale, fullscreen, filtering, renderer, save folder).

TROUBLESHOOTING
---------------
SimsBustinOut_compat.exe is the same game built with a different compiler
(GCC instead of Clang). It runs somewhat slower; use it only if
SimsBustinOut.exe will not start on your PC.

Every run writes sims_bustinout.log next to the exe. If the game stops or
crashes, that file says where.

Requirements: Windows 10 or 11, 64-bit; a Direct3D 10/11 graphics card for full speed.
