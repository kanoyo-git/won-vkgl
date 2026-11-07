@echo off

setlocal
set "HL=%~dp0..\Half-Life"
set "MARK=%HL%\.vkgl-installed"
if not exist "%MARK%" (
  echo vkgl is not installed.
  pause
  exit /b 0
)
findstr /x /c:"opengl32.dll" "%MARK%" >nul && del "%HL%\opengl32.dll"
findstr /x /c:"valve_WON/resource/gameui_english.txt" "%MARK%" >nul && del "%HL%\valve_WON\resource\gameui_english.txt"
del "%MARK%"
if exist "%HL%\vkgl.log" del "%HL%\vkgl.log"
echo vkgl removed. Half-Life\vkgl.ini is kept with your settings.
pause
