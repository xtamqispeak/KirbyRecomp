@echo off
setlocal
cd /d "%~dp0"
set "KIRBY_EXE=%~dp0generated-kirby\probe\KirbyRecomp.exe"
if exist "kirby-launcher.path" set /p KIRBY_EXE=<"kirby-launcher.path"
if not exist "%KIRBY_EXE%" (
  echo Run Build Kirby.cmd and select your dumped ExeFS and RomFS first.
  pause
  exit /b 1
)
for %%I in ("%KIRBY_EXE%") do cd /d "%%~dpI"
"%KIRBY_EXE%" >runtime.stdout.log 2>runtime.log
if errorlevel 1 (
  echo The development build stopped. See generated-kirby\probe\runtime.log.
  pause
)
