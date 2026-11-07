@echo off

setlocal
set BXT_DISABLE_VSYNC=1

set "WINEDLLOVERRIDES=opengl32=n,b;%WINEDLLOVERRIDES%"
set "ROOT=%~dp0"
set "HL=%~dp0Half-Life"
set "MARK=%HL%\.vkgl-installed"

if not exist "%HL%\hl.exe" (
  echo Half-Life\hl.exe not found next to this file.
  pause
  exit /b 1
)

if exist "%HL%\opengl32.dll" (
  findstr /x /c:"opengl32.dll" "%MARK%" >nul 2>&1 || (
    echo Half-Life\opengl32.dll is not vkgl's; remove it to use vkgl.
    pause
    exit /b 1
  )
)
copy /y "%ROOT%vkgl\opengl32.dll" "%HL%\opengl32.dll" >nul || (
  echo Could not copy opengl32.dll, is the game still running?
  pause
  exit /b 1
)
findstr /x /c:"opengl32.dll" "%MARK%" >nul 2>&1 || >>"%MARK%" echo opengl32.dll
if not exist "%HL%\vkgl.ini" copy "%ROOT%vkgl\vkgl.ini" "%HL%\vkgl.ini" >nul

call "%ROOT%Bunnymod XT\update.bat"
cd /d "%HL%"
"%ROOT%Bunnymod XT\Injector.exe" "%HL%\hl.exe" -steam -gl -32bpp -game valve_WON -noforcemparms
