@echo off
REM ============================================================================
REM deploy.bat - Deploy whois plugin as an MO2 mod
REM ============================================================================
REM This script:
REM   1. Verifies the build and MO2 paths exist
REM   2. Stages files and creates a zip archive
REM   3. Installs the archive to MO2 mods directory
REM   4. Enables the mod in MO2's modlist.txt
REM ============================================================================

setlocal enabledelayedexpansion

echo ============================================================================
echo                            WHOIS DEPLOY SCRIPT
echo ============================================================================
echo.

:: Deprecation warning for old env var
if defined WHOIS_DEPLOY_PATH (
    echo WARNING: WHOIS_DEPLOY_PATH is deprecated. Use WHOIS_MO2_MODS and
    echo          WHOIS_MO2_PROFILE instead.
    echo.
)

:: MO2 mods directory
if defined WHOIS_MO2_MODS (
    set "MO2_MODS=%WHOIS_MO2_MODS%"
) else (
    set "MO2_MODS=D:\Nolvus\Instance\MODS"
)

:: MO2 active profile directory
if defined WHOIS_MO2_PROFILE (
    set "MO2_PROFILE=%WHOIS_MO2_PROFILE%"
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

set "MOD_NAME=whois-dev-%VERSION%"

REM ============================================================================
REM STEP 1: Verify Prerequisites
REM ============================================================================
echo [1/4] Verifying prerequisites...
echo ----------------------------------------------------------------------------

if not exist "build\Release\whois.dll" (
    echo ERROR: build\Release\whois.dll not found
    echo Run build.bat first
    exit /b 1
)
echo   Found: build\Release\whois.dll

if not exist "%MO2_MODS%" (
    echo ERROR: MO2 mods directory not found: %MO2_MODS%
    echo Set WHOIS_MO2_MODS to your MO2 mods directory
    exit /b 1
)
echo   Found: %MO2_MODS%

if not exist "%MO2_PROFILE%\modlist.txt" (
    echo ERROR: modlist.txt not found: %MO2_PROFILE%\modlist.txt
    echo Set WHOIS_MO2_PROFILE to your active MO2 profile directory
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
mkdir "build\staging\SKSE\Plugins\whois"

:: Stage files
copy /Y "build\Release\whois.dll" "build\staging\SKSE\Plugins\whois.dll" >nul
copy /Y "skse\plugins\whois.ini" "build\staging\SKSE\Plugins\whois.ini" >nul
xcopy /E /I /Y "skse\plugins\whois" "build\staging\SKSE\Plugins\whois" >nul

:: Create zip (remove old one first)
if exist "build\whois-dev.zip" del /Q "build\whois-dev.zip"
powershell -NoProfile -Command "Compress-Archive -Path 'build\staging\*' -DestinationPath 'build\whois-dev.zip'"
if %ERRORLEVEL% neq 0 (
    echo ERROR: Failed to create archive
    exit /b %ERRORLEVEL%
)
echo   Created: build\whois-dev.zip

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
powershell -NoProfile -Command "Expand-Archive -Path 'build\whois-dev.zip' -DestinationPath '%MO2_MODS%\!MOD_NAME!'"
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
echo Archive: build\whois-dev.zip
echo Mod:     %MO2_MODS%\!MOD_NAME!
echo Profile: %MO2_PROFILE%\modlist.txt
echo.
echo Installed Files:
echo   - SKSE\Plugins\whois.dll
echo   - SKSE\Plugins\whois.ini
echo   - SKSE\Plugins\whois\  (assets)
echo.
echo  *** Launch Skyrim through MO2 to test the plugin ***
echo.
echo ============================================================================

endlocal
pause
