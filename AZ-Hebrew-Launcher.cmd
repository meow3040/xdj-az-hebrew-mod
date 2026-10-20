@echo off
rem SPDX-License-Identifier: GPL-3.0-only
rem The Hebrew launcher: double-click to build AZ-MODS with the Hebrew mod, put it on the
rem USB stick and turn it on at the deck (launcher\app.py). Needs Python 3.8+ with the py
rem launcher (python.org).
setlocal
set "APP=%~dp0launcher\app.py"
where pyw >nul 2>nul
if not errorlevel 1 (
    start "" pyw -3 "%APP%"
    exit /b 0
)
where py >nul 2>nul
if not errorlevel 1 (
    start "" /min py -3 "%APP%"
    exit /b 0
)
echo The launcher needs Python 3 from python.org, with the "py launcher" option.
pause
exit /b 1
