@echo off
rem Runs the automated checks for Drop-In Co-op. Pass -Build, -Package or -Quick if you want (see run_checks.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_checks.ps1" %*
echo.
pause
