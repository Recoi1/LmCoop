@echo off
rem Starts LmCoop and shows a live window of what goes wrong (warnings, errors, crashes). Closes with the game.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0LmCoop-Monitor.ps1"
