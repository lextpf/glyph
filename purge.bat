@echo off
REM ============================================================================
REM purge.bat - Remove glyph plugin mod from MO2
REM ============================================================================
REM This script:
REM   1. Removes the glyph-dev mod folder from MO2 mods directory
REM   2. Removes the entry from MO2's modlist.txt
REM ============================================================================

setlocal enabledelayedexpansion

echo ============================================================================
echo                            GLYPH PURGE SCRIPT
echo ============================================================================
echo.

:: Deprecation warning for old env var
if defined GLYPH_DEPLOY_PATH (
    echo WARNING: GLYPH_DEPLOY_PATH is deprecated. Use GLYPH_MO2_MODS and
    echo          GLYPH_MO2_PROFILE instead.
    echo.
)

:: MO2 mods directory
if defined GLYPH_MO2_MODS (
    set "MO2_MODS=%GLYPH_MO2_MODS%"
) else (
    set "MO2_MODS=D:\Nolvus\Instance\MODS\mods"
)

:: MO2 active profile directory
if defined GLYPH_MO2_PROFILE (
    set "MO2_PROFILE=%GLYPH_MO2_PROFILE%"
) else (
    set "MO2_PROFILE=D:\Nolvus\Instance\MODS\profiles\Nolvus Awakening"
)

:: Read version from vcpkg.json
for /f "usebackq delims=" %%V in (`powershell -NoProfile -Command "(Get-Content 'vcpkg.json' | ConvertFrom-Json).'version-string'"`) do (
    set "VERSION=%%V"
)
if not defined VERSION (
    echo ERROR: Could not read version from vcpkg.json
    exit /b 1
)

set "MOD_NAME=glyph-dev-%VERSION%"

echo Mod:     %MO2_MODS%\!MOD_NAME!
echo Profile: %MO2_PROFILE%\modlist.txt
echo.
echo ============================================================================
echo.

REM ============================================================================
REM STEP 1: Remove Mod Folder
REM ============================================================================
echo [1/2] Removing mod folder...
echo ----------------------------------------------------------------------------
if exist "%MO2_MODS%\!MOD_NAME!" (
    rmdir /S /Q "%MO2_MODS%\!MOD_NAME!"
    echo   Removed: %MO2_MODS%\!MOD_NAME!
) else (
    echo   Skipped: !MOD_NAME! folder not found
)
echo.

REM ============================================================================
REM STEP 2: Update modlist.txt
REM ============================================================================
echo [2/2] Updating modlist.txt...
echo ----------------------------------------------------------------------------
if exist "%MO2_PROFILE%\modlist.txt" (
    powershell -NoProfile -Command ^
        "$modName = '!MOD_NAME!';" ^
        "$modlist = '%MO2_PROFILE%\modlist.txt';" ^
        "$lines = [System.IO.File]::ReadAllLines($modlist);" ^
        "$lines = $lines | Where-Object { $_ -ne \"+$modName\" -and $_ -ne \"-$modName\" };" ^
        "[System.IO.File]::WriteAllLines($modlist, [string[]]$lines);"
    if %ERRORLEVEL% neq 0 (
        echo   ERROR: Failed to update modlist.txt
    ) else (
        echo   Removed: !MOD_NAME! from modlist.txt
    )
) else (
    echo   Skipped: modlist.txt not found
)
echo.

REM ============================================================================
REM SUMMARY
REM ============================================================================
echo ============================================================================
echo                            PURGE COMPLETE
echo ============================================================================
echo.

:end
endlocal
pause
