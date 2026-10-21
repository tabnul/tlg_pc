@echo off
rem DLAA jitter session: dev build, F7 jitter on/off, F6 mark (log + frame capture + screenshot).
rem Optional argument: a render size, e.g. start.cmd 1920x1080.
rem Session files go to port\out\jitter; edit control.txt there for live rules (re-read twice a second).
rem lab.txt there holds the menu's Lab rows (re-read when saved).
set OUT=%~dp0..\..\out\jitter
if not exist "%OUT%\captures" mkdir "%OUT%\captures"
if not exist "%OUT%\control.txt" type nul > "%OUT%\control.txt"
if not exist "%OUT%\lua_exec.lua" copy "%~dp0lua_exec.lua" "%OUT%\lua_exec.lua" > nul
if not exist "%OUT%\lab.txt" copy "%~dp0lab.txt" "%OUT%\lab.txt" > nul
set PATH=C:\msys64\clang64\bin;%PATH%
set TLG_JITTER_DEBUG=1
set TLG_JITTER_CONTROL=%OUT%\control.txt
set TLG_CAPTURE_DIR=%OUT%\captures
set TLG_LUA_EXEC=%OUT%\lua_exec.lua
set TLG_LAB=%OUT%\lab.txt
if not "%~1"=="" set TLG_RESOLUTION=%~1
start "jitter watch" /min powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0watch.ps1"
cd /d %~dp0..\..
out\devenv\Scripts\python launcher\tlg_run.py
