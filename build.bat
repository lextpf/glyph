@echo off
REM ===========================================================================================
REM build.bat - Complete build pipeline for glyph
REM ===========================================================================================
REM This script:
REM   1. clang-format - in-place formatting of src/*.cpp / src/*.hpp / src/*.h / src/*.c
REM   2. cmake        - CMake configure with vcpkg manifest install and VS 17 2022 generator
REM   3. clang-tidy   - static analysis via Ninja compile_commands.json sidecar (build-cdb/)
REM   4. build        - release build of the Release configuration via cmake --build
REM   5. doxide       - API documentation generation via doxide + mkdocs build
REM ===========================================================================================

setlocal enabledelayedexpansion

echo ============================================================================
echo                             GLYPH BUILD PIPELINE
echo ============================================================================
echo.

REM ============================================================================
REM STEP 1: Run clang-format
REM ============================================================================
echo [1/5] Running clang-format...
echo ----------------------------------------------------------------------------

where clang-format >nul 2>&1
if errorlevel 1 (
    echo SKIP: clang-format not found in PATH
) else (
    for %%f in (src\*.cpp src\*.hpp src\*.c) do (
        if exist "%%f" clang-format -i "%%f"
    )
    echo Formatting complete.
)
echo.

REM ============================================================================
REM STEP 2: CMake Configuration
REM ============================================================================
echo [2/5] Configuring with CMake...
echo ----------------------------------------------------------------------------
cmake --preset default
if %ERRORLEVEL% neq 0 (
    echo ERROR: CMake configuration failed
    exit /b %ERRORLEVEL%
)
echo.

REM ============================================================================
REM STEP 3: Run clang-tidy
REM ============================================================================
echo [3/5] Running clang-tidy...
echo ----------------------------------------------------------------------------

where clang-tidy >nul 2>&1
if errorlevel 1 (
    echo SKIP: clang-tidy not found in PATH
) else (
    if not exist "build-cdb\compile_commands.json" (
        echo   Generating compile_commands.json via Ninja sidecar...
        cmake --preset compile-db >nul
        if !ERRORLEVEL! neq 0 (
            echo ERROR: compile-db configure failed
            exit /b 1
        )
        echo   Normalizing compile_commands.json for clang-cl driver mode...
        python scripts/_normalize_compile_db.py
        if !ERRORLEVEL! neq 0 (
            echo ERROR: compile_commands.json normalization failed
            exit /b 1
        )
    )

    REM clang-tidy invocation notes:
    REM  --driver-mode=cl   : the compiler in the Ninja-generated
    REM                       compile_commands.json is the DOS short
    REM                       path "CLANG_~1.EXE", which defeats clang-
    REM                       tidy's auto-detection. With cl mode, MSVC
    REM                       flags like /MT /EHa are parsed correctly.
    REM  -fdelayed-template-parsing
    REM                     : Defer template body parsing to match MSVC's
    REM                       two-phase name lookup. Without this, clang
    REM                       reports errors in CommonLibSSE-NG headers
    REM                       that compile fine with MSVC.
    REM  -Wno-delayed-template-parsing-in-cxx20
    REM                     : Silence the deprecation notice for the flag
    REM                       above; we accept the trade-off.
    REM  -Wno-unused-command-line-argument
    REM                     : Silence "-O3 unused" noise from the hybrid
    REM                       flag set in compile_commands.json.
    for %%f in (src\*.cpp tests\*.cpp) do (
        if exist "%%f" (
            echo   tidy: %%f
            clang-tidy --quiet --header-filter="[/\\]%%~nf\.hpp$" -p build-cdb ^
                --extra-arg-before=--driver-mode=cl ^
                --extra-arg=-fdelayed-template-parsing ^
                --extra-arg=-Wno-delayed-template-parsing-in-cxx20 ^
                --extra-arg=-Wno-unused-command-line-argument ^
                "%%f"
            if !ERRORLEVEL! neq 0 (
                echo ERROR: clang-tidy reported issues in %%f
                exit /b 1
            )
        )
    )
    echo clang-tidy complete.
)
echo.

REM ============================================================================
REM STEP 4: Build Release
REM ============================================================================
echo [4/5] Building Release plugin target...
echo ----------------------------------------------------------------------------
cmake --build build --config Release --target glyph -- /m:1
if %ERRORLEVEL% neq 0 (
    echo ERROR: Build failed
    exit /b %ERRORLEVEL%
)
echo.

REM ============================================================================
REM STEP 5: Generate API Documentation (doxide)
REM ============================================================================
echo [5/5] Generating API documentation...
echo ----------------------------------------------------------------------------
where doxide >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo SKIP: doxide not found in PATH
) else (
    doxide build
    if !ERRORLEVEL! neq 0 (
        echo ERROR: doxide build failed [exit code !ERRORLEVEL!]
        exit /b 1
    )
    python scripts/_promote_subgroups.py
    if !ERRORLEVEL! neq 0 (
        echo ERROR: _promote_subgroups.py failed [exit code !ERRORLEVEL!]
        exit /b 1
    )
    python scripts/_clean_docs.py
    if !ERRORLEVEL! neq 0 (
        echo ERROR: _clean_docs.py failed [exit code !ERRORLEVEL!]
        exit /b 1
    )
    where mkdocs >nul 2>&1
    if !ERRORLEVEL! neq 0 (
        echo SKIP: mkdocs not found in PATH
    ) else (
        mkdocs build
        if !ERRORLEVEL! neq 0 (
            echo ERROR: mkdocs build failed [exit code !ERRORLEVEL!]
            exit /b 1
        )
    )
)
echo.

REM ============================================================================
REM SUMMARY
REM ============================================================================
echo ============================================================================
echo                           BUILD PIPELINE COMPLETE
echo ============================================================================
echo.
echo Build Output:
echo   Release: build\Release\glyph.dll
echo   Linkage: Static (/MT, x64-windows-static)
echo.
echo Documentation:
echo   - Md:   docs\  (if doxide available)
echo   - Html: site\  (if mkdocs available)
echo.
echo  *** Run deploy.bat to install the plugin as an MO2 mod ***
echo.
echo ============================================================================

endlocal
exit /b 0
