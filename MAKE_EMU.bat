@echo off
rem ---------------------------------------------------------------------------
rem Builds the emulator itself: tamaemu-sdl.exe, tamaemu.exe, test_all.exe.
rem
rem This is the one to double-click after changing anything in src\ - the link
rem layer, the CPU, the LCD. MAKE_LAUNCHER.bat builds the launcher window and
rem does not rebuild the emulator; they are separate programs.
rem
rem Needs MSYS2 with the UCRT64 toolchain and its SDL2 package. The launcher
rem uses the same compiler and resource tools, but does not use SDL2.
rem
rem The three binaries are deliberately built differently:
rem   tamaemu-sdl.exe  the SDL-backed emulator launched by tama-launcher.exe
rem   tamaemu.exe      no SDL, no icon resource - headless runs and diagnostics
rem   test_all.exe     the unit suite: tests\test_all.c plus src\ without
rem                    src\main.c, because the suite brings its own main()
rem
rem It finishes by running the suite, which should report "all tests passed".
rem
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

echo [1/4] resources ^(app icon, version^)
windres packaging\tamaps.rc -O coff -o packaging\tamaps.res || goto :fail

echo [2/4] tamaemu-sdl.exe   ^(the one you play^)
gcc -std=c11 -O2 -Wall -Wextra -DUSE_SDL -DSDL_MAIN_HANDLED ^
    -o tamaemu-sdl.exe src\*.c packaging\tamaps.res ^
    -lSDL2 -lwinmm -lws2_32 || goto :fail

echo [3/4] tamaemu.exe       ^(no SDL - headless / diagnostics^)
gcc -std=c11 -O2 -Wall -Wextra ^
    -o tamaemu.exe src\*.c ^
    -lwinmm -lws2_32 || goto :fail

echo [4/4] test_all.exe      ^(unit suite^)
gcc -std=c11 -O2 -Wall -Wextra ^
    -o test_all.exe tests\test_all.c ^
    src\cpu.c src\mem.c src\periph.c src\lcd.c src\link.c src\disasm.c src\panel.c ^
    src\device.c src\pn512.c src\nfcpeer.c tools\dlc.c tools\swapreq.c ^
    tools\dlc_scan.c tools\dlc_names_4u.c ^
    -lwinmm -lws2_32 || goto :fail

echo.
echo   Running the unit suite...
rem ".\" matters: a bare name is not always resolved from the current directory.
".\test_all.exe" > test_results.log 2>&1

rem The suite prints "all tests passed", or "<n> FAILURES" and nothing else.
rem Look for the pass line rather than counting, so a run that dies before it
rem prints anything is not mistaken for a clean one.
set "FAILS=?"
for /f "tokens=1" %%N in ('findstr /r /c:"[0-9][0-9]* FAILURES" test_results.log') do set "FAILS=%%N"
findstr /c:"all tests passed" test_results.log >nul && set "FAILS=0"

echo.
if "%FAILS%"=="0" (
  echo   All tests passed.
) else (
  echo   *** %FAILS% failures - expected none. ***
  echo   Full output is in test_results.log
)

echo.
echo   Built:
dir /b /a-d tamaemu-sdl.exe tamaemu.exe test_all.exe
echo.
echo   Your changes are now in the emulator. Start it with tama-launcher.exe.
echo.
echo   Please re-check VISIT / PLAY / ITEM EXCHANGE after touching the
echo   emulator - the unit suite does not cover them.
echo.
pause
exit /b 0

:fail
echo.
echo   BUILD FAILED - see the messages above.
echo.
echo   If it cannot find SDL2, install it in the MSYS2 UCRT64 shell with:
echo       pacman -S mingw-w64-ucrt-x86_64-SDL2
echo.
pause
exit /b 1
