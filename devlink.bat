@echo off
rem Starts the devlink PC app (SPEC section 8). Double-click, or run from any folder.
setlocal
set "ROOT=%~dp0"
set "PY=%ROOT%pc\.venv\Scripts\python.exe"

if not exist "%PY%" (
    echo The PC app's Python environment is missing. Set it up once with:
    echo   py -3.12 -m venv "%ROOT%pc\.venv"
    echo   "%PY%" -m pip install -r "%ROOT%pc\requirements.txt"
    pause
    exit /b 1
)

"%PY%" "%ROOT%pc\devlink.py" %*
if errorlevel 1 pause
