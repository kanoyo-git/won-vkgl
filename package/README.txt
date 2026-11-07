vkgl for Half-Life (WON)
========================

vkgl runs the game's OpenGL renderer on Vulkan and enables high-FPS physics,
view interpolation, the FPS cap unlock, and raw mouse input by default.
Vulkan 1.2 is required. Engine fixes target hw.dll build 3248.

Install
-------
Extract the archive into the pack folder containing Half-Life and Bunnymod XT.
Start:

    4. Half-Life (WON) + BXT + vkgl.bat

The game and Bunnymod XT are not included. Wine and Proton need
WINEDLLOVERRIDES=opengl32=n,b before the game starts.

Settings
--------
Edit Half-Life\vkgl.ini before starting the game. The [engine] options
physics_fix, physics_interp, fps_unlock, and raw_mouse are enabled by default.
Set an option to 0 to disable it, and set fps_max in the game to choose a cap.
Disable physics_fix and fps_unlock for speedruns requiring original behavior.
Raw mouse input falls back to cursor input when unavailable.

Upgrades keep an existing vkgl.ini. Set physics_fix, physics_interp,
fps_unlock, and raw_mouse to 1 there if needed. Use the new launcher.

Choose Vulkan or OpenGL in Options > Video > Renderer. If a capture is black,
set direct_present=0 under [capture] or capture the window. The driver log is
Half-Life\vkgl.log. Run vkgl\uninstall.bat to remove the driver; the
configuration is kept.
