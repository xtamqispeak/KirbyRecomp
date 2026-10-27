@echo off
setlocal
cd /d "%~dp0"
if not exist ".venv\Scripts\python.exe" (
  where py >nul 2>nul
  if not errorlevel 1 (
    py -3 -m venv .venv
  ) else (
    python -m venv .venv
  )
  if errorlevel 1 goto failed
)
.venv\Scripts\python.exe -c "import sys,struct; assert sys.version_info >= (3,12) and struct.calcsize('P') == 8, 'Python 3.12+ 64-bit is required'"
if errorlevel 1 goto failed
.venv\Scripts\python.exe -m pip install --disable-pip-version-check -r requirements-build.txt
if errorlevel 1 goto failed
.venv\Scripts\python.exe tools\setup_kirby.py %*
if errorlevel 1 goto failed
pause
exit /b 0
:failed
echo Build failed. Python 3.12+ 64-bit is required. See the error above.
pause
exit /b 1
