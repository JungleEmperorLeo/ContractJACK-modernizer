@echo off
setlocal EnableExtensions
rem ====================================================================
rem  Copies the retail Contract J.A.C.K. server-side source (ObjectDLL, the
rem  shared game code and the engine SDK) from  ..\Tools\Source  into
rem  src\Server  so it can be modified as part of this fork.
rem  Safe to run repeatedly: it does nothing if the copy already exists.
rem ====================================================================
cd /d "%~dp0"
set "HERE=%CD%"
for %%I in ("%HERE%\..") do set "GAME=%%~fI"
set "BASE=%GAME%\Tools\Source"
set "DST=%HERE%\src\Server"

if exist "%DST%\Game\ObjectDLL\Stdafx.h" (
  echo         Server source already present in src\Server.
  exit /b 0
)
if not exist "%BASE%\Game\ObjectDLL\Stdafx.h" (
  echo [ERROR] Retail server source not found at %BASE%
  echo         Expected  Tools\Source\Game\ObjectDLL  inside your Contract J.A.C.K. folder.
  exit /b 1
)

echo         Copying server source from %BASE% ...
xcopy /e /i /y /q "%BASE%\Game\ObjectDLL" "%DST%\Game\ObjectDLL" >nul || goto :copyfail
xcopy /e /i /y /q "%BASE%\Game\Shared"    "%DST%\Game\Shared"    >nul || goto :copyfail
xcopy /e /i /y /q "%BASE%\Engine\sdk\inc" "%DST%\Engine\sdk\inc" >nul || goto :copyfail
xcopy /e /i /y /q "%BASE%\Engine\sdk\lib" "%DST%\Engine\sdk\lib" >nul || goto :copyfail
copy /y "%BASE%\Game\bison.exe" "%DST%\Game\bison.exe" >nul || goto :copyfail
copy /y "%BASE%\Game\flex.exe"  "%DST%\Game\flex.exe"  >nul || goto :copyfail
echo         Done.
exit /b 0

:copyfail
echo [ERROR] Copying the server source failed.
exit /b 1
