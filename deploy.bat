@echo off
REM ============================================================================
REM deploy.bat - Deploy glyph plugin as an MO2 mod
REM ============================================================================
REM This script:
REM   1. Verifies the build and MO2 paths exist
REM   2. Stages files and creates a zip archive
REM   3. Installs the archive to MO2 mods directory
REM   4. Enables the mod in MO2's modlist.txt
REM ============================================================================

setlocal enabledelayedexpansion

echo ============================================================================
echo                            GLYPH DEPLOY SCRIPT
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
    set "MO2_MODS=D:\Nolvus\Instance\MODS"
)

:: MO2 active profile directory
if defined GLYPH_MO2_PROFILE (
    set "MO2_PROFILE=%GLYPH_MO2_PROFILE%"
) else (
    set "MO2_PROFILE=D:\Nolvus\Instance\profiles\Default"
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

REM ============================================================================
REM STEP 1: Verify Prerequisites
REM ============================================================================
echo [1/4] Verifying prerequisites...
echo ----------------------------------------------------------------------------

if not exist "build\Release\glyph.dll" (
    echo ERROR: build\Release\glyph.dll not found
    echo Run build.bat first
    exit /b 1
)
echo   Found: build\Release\glyph.dll

if not exist "%MO2_MODS%" (
    echo ERROR: MO2 mods directory not found: %MO2_MODS%
    echo Set GLYPH_MO2_MODS to your MO2 mods directory
    exit /b 1
)
echo   Found: %MO2_MODS%

if not exist "%MO2_PROFILE%\modlist.txt" (
    echo ERROR: modlist.txt not found: %MO2_PROFILE%\modlist.txt
    echo Set GLYPH_MO2_PROFILE to your active MO2 profile directory
    exit /b 1
)
echo   Found: %MO2_PROFILE%\modlist.txt
echo.

REM ============================================================================
REM STEP 2: Create Archive
REM ============================================================================
echo [2/4] Creating archive...
echo ----------------------------------------------------------------------------

:: Clean previous staging
if exist "build\staging" rmdir /S /Q "build\staging"
mkdir "build\staging\SKSE\Plugins\glyph"

:: Stage files
copy /Y "build\Release\glyph.dll" "build\staging\SKSE\Plugins\glyph.dll" >nul
copy /Y "skse\plugins\glyph.ini" "build\staging\SKSE\Plugins\glyph.ini" >nul
xcopy /E /I /Y "skse\plugins\glyph" "build\staging\SKSE\Plugins\glyph" >nul

:: Create zip (remove old one first)
if exist "build\glyph-dev.zip" del /Q "build\glyph-dev.zip"
powershell -NoProfile -Command "Compress-Archive -Path 'build\staging\*' -DestinationPath 'build\glyph-dev.zip'"
if %ERRORLEVEL% neq 0 (
    echo ERROR: Failed to create archive
    exit /b %ERRORLEVEL%
)
echo   Created: build\glyph-dev.zip

:: Clean staging
rmdir /S /Q "build\staging"
echo.

REM ============================================================================
REM STEP 3: Install to MO2
REM ============================================================================
echo [3/4] Installing to MO2...
echo ----------------------------------------------------------------------------

:: Remove previous installation
if exist "%MO2_MODS%\!MOD_NAME!" (
    rmdir /S /Q "%MO2_MODS%\!MOD_NAME!"
    echo   Removed previous: %MO2_MODS%\!MOD_NAME!
)

:: Extract archive
powershell -NoProfile -Command "Expand-Archive -Path 'build\glyph-dev.zip' -DestinationPath '%MO2_MODS%\!MOD_NAME!'"
if %ERRORLEVEL% neq 0 (
    echo ERROR: Failed to extract archive
    exit /b %ERRORLEVEL%
)
echo   Installed: %MO2_MODS%\!MOD_NAME!
echo.

REM ============================================================================
REM STEP 4: Update modlist.txt
REM ============================================================================
echo [4/4] Updating modlist.txt...
echo ----------------------------------------------------------------------------

powershell -NoProfile -Command ^
    "$modName = '!MOD_NAME!';" ^
    "$modlist = '%MO2_PROFILE%\modlist.txt';" ^
    "$lines = [System.IO.File]::ReadAllLines($modlist);" ^
    "$found = $false;" ^
    "for ($i = 0; $i -lt $lines.Length; $i++) {" ^
    "    if ($lines[$i] -eq \"+$modName\") {" ^
    "        $found = $true;" ^
    "        break;" ^
    "    }" ^
    "    if ($lines[$i] -eq \"-$modName\") {" ^
    "        $lines[$i] = \"+$modName\";" ^
    "        $found = $true;" ^
    "        break;" ^
    "    }" ^
    "}" ^
    "if (-not $found) {" ^
    "    $newLines = @($lines[0], \"+$modName\") + $lines[1..($lines.Length-1)];" ^
    "    $lines = $newLines;" ^
    "}" ^
    "[System.IO.File]::WriteAllLines($modlist, $lines);"
if %ERRORLEVEL% neq 0 (
    echo ERROR: Failed to update modlist.txt
    exit /b %ERRORLEVEL%
)
echo   Enabled: +!MOD_NAME! in modlist.txt
echo.

REM ============================================================================
REM SUMMARY
REM ============================================================================
echo ============================================================================
echo                            DEPLOY COMPLETE
echo ============================================================================
echo.
echo Archive: build\glyph-dev.zip
echo Mod:     %MO2_MODS%\!MOD_NAME!
echo Profile: %MO2_PROFILE%\modlist.txt
echo.
echo Installed Files:
echo   - SKSE\Plugins\glyph.dll
echo   - SKSE\Plugins\glyph.ini
echo   - SKSE\Plugins\glyph\  (assets)
echo.
echo  *** Launch Skyrim through MO2 to test the plugin ***
echo.
echo ============================================================================

endlocal
pause
