@echo off
rem Starts the devlink web UI (SPEC section 8): a local server, and the page opens in the browser.
rem Close this window to stop the server. The Qt desktop app is devlink-qt.bat.
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

title devlink web UI
"%PY%" "%ROOT%pc\devlink_web.py" %*
if errorlevel 1 pause
