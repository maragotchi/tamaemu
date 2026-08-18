@echo off
rem ---------------------------------------------------------------------------
rem Builds tama-launcher.exe - the friendly front door.
rem
rem Needs MSYS2 with the UCRT64 toolchain (gcc + windres), the same one that
rem builds the emulator. Nothing else: the launcher is plain Win32, so there is
rem no SDL, no Python and no DLL to ship beside it.
rem
rem Add DEBUG as the first argument to also build tama-launcher-dbg.exe, which
rem appends a layout/rows self-check to launcher_debug.txt on every start. That
rem exists because a themed list renders blank under PrintWindow and a
rem foreground screen grab is not always possible - so the window is checked by
rem numbers instead of by eye.
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

set "PATH=C:\msys64\ucrt64\bin;%PATH%"
where gcc >nul 2>&1 || (
  echo.
  echo   gcc not found. Install MSYS2 and its UCRT64 toolchain, or edit the
  echo   PATH line in this file to point at your own.
  echo.
  pause
  exit /b 1
)

echo [1/2] resources ^(icon, version, manifest^)
windres packaging\launcher.rc -O coff -o packaging\launcher.res || goto :fail

echo [2/2] tama-launcher.exe
gcc -std=c11 -O2 -Wall -Wextra -mwindows -DUNICODE -D_UNICODE -Itools ^
    -o tama-launcher.exe ^
    tools\launcher_win32.c tools\logcap.c tools\dlc.c tools\dlc_scan.c tools\dlc_names_4u.c tools\swapreq.c packaging\launcher.res ^
    -lcomctl32 -lcomdlg32 -lshell32 -lole32 -luuid -lgdiplus -ladvapi32 || goto :fail

if /i "%~1"=="DEBUG" (
  echo [+]   tama-launcher-dbg.exe
  gcc -std=c11 -O2 -Wall -Wextra -mwindows -DUNICODE -D_UNICODE -DLAUNCHER_DEBUG -Itools ^
      -o tama-launcher-dbg.exe ^
      tools\launcher_win32.c tools\logcap.c tools\dlc.c tools\dlc_scan.c tools\dlc_names_4u.c tools\swapreq.c packaging\launcher.res ^
      -lcomctl32 -lcomdlg32 -lshell32 -lole32 -luuid -lgdiplus -ladvapi32 || goto :fail
)

echo.
echo   Built tama-launcher.exe
dir /b /a-d tama-launcher.exe
echo.
echo   Double-click it to run. Nothing else needs installing.
echo.
pause
exit /b 0

:fail
echo.
echo   BUILD FAILED - see the messages above.
echo.
pause
exit /b 1
