@echo off
cd /d "%~dp0"
rem ---------------------------------------------------------------------------
rem Builds the shareable zip: unzip-and-double-click, no GitHub needed.
rem
rem The file list below is an explicit ALLOWLIST, deliberately. A
rem copy-everything-minus-exclusions approach is one typo away from shipping
rem someone a ROM dump or a save file, so nothing is copied unless it is named
rem here.
rem ---------------------------------------------------------------------------
setlocal
set "VER=%~1"
if "%VER%"=="" for /f %%D in ('powershell -NoProfile -Command "Get-Date -Format yyyy-MM-dd"') do set "VER=%%D"
set "NAME=tamaemu-%VER%"
set "STAGE=release\%NAME%"

echo.
echo   Packaging %NAME%
echo.

if not exist tamaemu-sdl.exe     (echo   ERROR: tamaemu-sdl.exe not found - build it first.        & pause & exit /b 1)
if not exist SDL2.dll            (echo   ERROR: SDL2.dll not found.                                & pause & exit /b 1)
if not exist tama-launcher.exe (echo   ERROR: tama-launcher.exe not found - run MAKE_LAUNCHER.bat & pause & exit /b 1)
if not exist docs\device-map.md (echo   ERROR: docs\device-map.md not found.                       & pause & exit /b 1)

if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"           2>nul
mkdir "%STAGE%\src"       2>nul
mkdir "%STAGE%\tests"     2>nul
mkdir "%STAGE%\tools"     2>nul
mkdir "%STAGE%\packaging" 2>nul
mkdir "%STAGE%\docs"      2>nul

rem --- the program ---
copy /y tamaemu-sdl.exe          "%STAGE%\" >nul
copy /y SDL2.dll                 "%STAGE%\" >nul

rem --- the launcher is the only user-facing entry point; it starts the emulator.
copy /y tama-launcher.exe      "%STAGE%\" >nul

copy /y packaging\README.txt     "%STAGE%\README.txt" >nul

rem --- source required by GPLv3; build scripts stay in the repository so the
rem     download remains a double-click package with one user-facing program. ---
copy /y src\*.c                  "%STAGE%\src\"   >nul
copy /y src\*.h                  "%STAGE%\src\"   >nul
copy /y tests\test_all.c         "%STAGE%\tests\" >nul
copy /y tools\launcher_win32.c   "%STAGE%\tools\" >nul
copy /y tools\dlc.c              "%STAGE%\tools\" >nul
copy /y tools\dlc_scan.c         "%STAGE%\tools\" >nul
copy /y tools\dlc_names_4u.c     "%STAGE%\tools\" >nul
copy /y tools\dlc.h              "%STAGE%\tools\" >nul
copy /y tools\swapreq.c          "%STAGE%\tools\" >nul
copy /y tools\swapreq.h          "%STAGE%\tools\" >nul
rem The resource files are source too: the .rc, the .ico it names, and the
rem manifest are all inputs to the shipped binaries.
copy /y packaging\launcher.rc       "%STAGE%\packaging\" >nul
copy /y packaging\launcher.manifest "%STAGE%\packaging\" >nul
copy /y packaging\launcher.ico      "%STAGE%\packaging\" >nul
copy /y packaging\tamaemu.rc        "%STAGE%\packaging\" >nul
copy /y packaging\tamaemu.ico       "%STAGE%\packaging\" >nul
copy /y docs\device-map.md           "%STAGE%\docs\" >nul
if exist LICENSE copy /y LICENSE "%STAGE%\" >nul

rem --- prove no ROM, save, log or personal path went in ---
set "BAD="
for %%X in (bin sav log) do if exist "%STAGE%\*.%%X" set "BAD=1"
if exist "%STAGE%\rom_path.txt" set "BAD=1"
if defined BAD (echo   ERROR: staging contains a ROM/save/log - aborting. & pause & exit /b 1)
findstr /s /m /c:"Users\Mara" "%STAGE%\*.*" >nul 2>&1 && (echo   ERROR: a personal path survived into the package. & pause & exit /b 1)

rem --- zip it ---
if exist "release\%NAME%.zip" del /q "release\%NAME%.zip"
powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%' -DestinationPath 'release\%NAME%.zip' -Force"
if not exist "release\%NAME%.zip" (echo   ERROR: zip step failed. & pause & exit /b 1)

rem --- checksum, so people can verify what they downloaded ---
powershell -NoProfile -Command "(Get-FileHash 'release\%NAME%.zip' -Algorithm SHA256).Hash" > "release\%NAME%.zip.sha256.txt"

echo.
echo   Done:
echo     release\%NAME%.zip
echo     release\%NAME%.zip.sha256.txt
echo.
echo   Share BOTH files. The .sha256.txt lets people check the zip they
echo   got is the one you published.
echo.
echo   The ROM is NOT included - that is intentional.
echo.
pause
