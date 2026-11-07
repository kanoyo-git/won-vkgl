# vkgl

vkgl runs Half-Life (WON)'s OpenGL renderer on Vulkan. It includes fixes for
high-FPS physics, view interpolation, the FPS cap, and mouse input. The driver
is a 32-bit Windows DLL and also works with Wine and Proton.

The engine fixes target `hw.dll` build 3248. Other builds may use the renderer,
but engine fixes are applied only when the expected code is found. Vulkan 1.2
is required.

## Install

1. Download the [latest release](https://github.com/kanoyo-git/won-vkgl/releases/latest).
2. Extract it into the pack folder containing `Half-Life` and `Bunnymod XT`.
3. Start `4. Half-Life (WON) + BXT + vkgl.bat`.

The archive does not include Half-Life or Bunnymod XT. Wine and Proton
need `WINEDLLOVERRIDES=opengl32=n,b` before the game starts. Run
`vkgl/uninstall.bat` to remove the driver; your configuration is kept.

## Settings

The fixes below are enabled by default. Edit `Half-Life/vkgl.ini` before
starting the game and set an option to `0` to disable it.

| Setting in `[engine]` | Effect |
| --- | --- |
| `physics_fix` | Use 10 ms physics steps above 100 FPS. |
| `physics_interp` | Smooth the view and moving objects between physics steps. |
| `fps_unlock` | Allow `fps_max` above 100 without `developer 1`. |
| `raw_mouse` | Use raw mouse input when available. |

Set `fps_max` in the game to choose a frame-rate limit. The physics fix changes
game timing; disable `physics_fix` and `fps_unlock` for speedruns that require
original behavior. Raw mouse input falls back to cursor input when unavailable.

An existing `Half-Life/vkgl.ini` is kept during upgrades. If it has
`physics_fix=0`, `physics_interp=0`, `fps_unlock=0`, or `raw_mouse=0`, change
those values to `1` to enable them. Use the new launcher after upgrading.

Choose Vulkan or OpenGL in Options > Video > Renderer. If OBS or Discord shows
a black capture, set `[capture] direct_present=0` in `vkgl.ini` or capture the
window. The driver log is `Half-Life/vkgl.log`. Changing resolution in the video
menu and the Steam overlay need further work.

## Build

Install CMake, Ninja, Python 3, the i686 MinGW toolchain, Vulkan headers, and
`glslc`, then run:

```bash
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake
cmake --build build
tools/package.sh
```

The package is written to `dist/vkgl-hl1-won.zip`.
