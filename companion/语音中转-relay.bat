@echo off
cd /d %~dp0
.venv\Scripts\python.exe relay.py %*
pause
