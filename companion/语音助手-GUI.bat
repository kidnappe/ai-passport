@echo off
cd /d %~dp0
.venv\Scripts\python.exe fre_app.py %*
pause
