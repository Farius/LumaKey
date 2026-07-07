@echo off
setlocal

rc /nologo LumaKey.rc
if errorlevel 1 exit /b 1

cl ^
  /std:c++17 ^
  /O2 ^
  /EHsc ^
  /DUNICODE ^
  /D_UNICODE ^
  /MT ^
  main.cpp ^
  LumaKey.res ^
  ole32.lib ^
  oleaut32.lib ^
  wbemuuid.lib ^
  shell32.lib ^
  user32.lib ^
  gdi32.lib ^
  advapi32.lib ^
  /Fe:LumaKey.exe ^
  /link /SUBSYSTEM:WINDOWS

endlocal
