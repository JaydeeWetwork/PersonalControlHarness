@echo off
rem ============================================================
rem  LithoLattice combined release - pre-publish prep (Windows)
rem
rem  Run this INSIDE the extracted release folder.
rem  It checks the folder structure, counts the files, and - if
rem  the Git command-line tools are installed - runs git init
rem  and makes the first commit using YOUR configured identity.
rem  It deletes nothing and uses no network. Every action is
rem  echoed before it runs. Plain text - read it in Notepad.
rem ============================================================
setlocal EnableExtensions
cd /d "%~dp0"
title LithoLattice release - pre-publish prep

echo ============================================================
echo  LithoLattice combined release - pre-publish prep
echo  Folder: %CD%
echo ============================================================
echo.

rem --- 1. Structure check --------------------------------------
set "MISSING=0"
for %%F in (LICENSE README.md MANIFEST.md) do (
    if not exist "%%~F" (
        echo  MISSING FILE:   %%~F
        set "MISSING=1"
    )
)
for %%D in (engine game pch) do (
    if not exist "%%~D\" (
        echo  MISSING FOLDER: %%~D
        set "MISSING=1"
    )
)
if "%MISSING%"=="1" (
    echo.
    echo  Structure check FAILED.
    echo  This script must run inside the extracted release folder -
    echo  the folder that contains LICENSE, README.md and MANIFEST.md.
    echo  Re-extract the whole zip, open the extracted folder, and
    echo  run the script from there.
    echo.
    pause
    exit /b 1
)
echo  Structure check: OK - LICENSE, README.md, MANIFEST.md,
echo  engine\, game\ and pch\ are all present.
echo.

rem --- 2. File count --------------------------------------------
set "COUNT=0"
for /f %%A in ('dir /s /b /a-d 2^>nul ^| find /c /v ""') do set "COUNT=%%A"
echo  Files in this folder (including this script): %COUNT%
echo.

rem --- 3. Git ----------------------------------------------------
where git >nul 2>nul
if errorlevel 1 (
    echo  Git command-line tools were not found on this PC.
    echo  No problem - GitHub Desktop will handle init and the
    echo  first commit. See PUBLISH-GUIDE.md, Part 3.
    echo.
    pause
    exit /b 0
)
echo  Git found:
git --version
echo.

if exist ".git\" (
    echo  A .git folder already exists here - init skipped.
) else (
    echo  Running: git init
    git init
)
echo.

set "UNAME="
set "UEMAIL="
for /f "delims=" %%A in ('git config user.name 2^>nul') do set "UNAME=%%A"
for /f "delims=" %%A in ('git config user.email 2^>nul') do set "UEMAIL=%%A"
if not defined UNAME goto :noident
if not defined UEMAIL goto :noident

echo  Git identity: %UNAME% ^<%UEMAIL%^>
echo.
echo  Running: git add -A
git add -A
echo.
echo  Running: git commit
git commit -m "Initial release: LithoLattice engine, LithoKraft game, Timing Handshake, Personal Control Harness - MIT"
echo.
echo  Done. Open GitHub Desktop, add this folder as a local
echo  repository, and click Publish Repository.
echo  See PUBLISH-GUIDE.md, Part 6.
echo.
pause
exit /b 0

:noident
echo  Git does not have your name/email configured yet, so the
echo  first commit was NOT made. Run these two commands once,
echo  then run this script again:
echo.
echo      git config --global user.name "Your Name"
echo      git config --global user.email "you@example.com"
echo.
echo  (Use the name and email of your GitHub account.)
echo.
pause
exit /b 0
