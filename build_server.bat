@echo off
rem Builds everything (client CShell.dll + server Object.lto), then packs and installs the mod.
rem Incremental: only changed files are recompiled.
call "%~dp0build.bat" /server
