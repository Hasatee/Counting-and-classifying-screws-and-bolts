@echo off
cd /d "%~dp0"
if exist "D:\python\python.exe" (
    "D:\python\python.exe" server.py
) else (
    python server.py
)
pause
