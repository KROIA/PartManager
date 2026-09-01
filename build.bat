@echo off
setlocal EnableExtensions EnableDelayedExpansion
set "projectRootPath=%CD%"

REM ----------------------------------------------------------------
REM Available targets:
REM   build.bat                        - configure + build x64-Debug and x64-Release
REM   build.bat <preset> [<preset>..]  - configure + build named CMake preset(s)
REM   build.bat list                   - list available CMake presets
REM   build.bat clean                  - wipe build outputs (keep dep cache and per-preset caches)
REM   build.bat clean-deps             - wipe build\dependencies\cache\*-build and *-subbuild (keep *-src)
REM   build.bat clean-all              - wipe everything under build\ and installation\
REM   build.bat help                   - print this list
REM
REM Multi-threaded build is enabled by default. Set CMAKE_BUILD_PARALLEL_LEVEL
REM to override the worker count (defaults to %%NUMBER_OF_PROCESSORS%%).
REM
REM Uses cmake --preset / cmake --build --preset so the build artifacts share
REM the same binary directories as Visual Studio's CMake integration
REM (build\x64-Debug, build\x64-Release, ...). Running this script does NOT
REM clash with VS open on the same project.
REM ----------------------------------------------------------------

REM dispatch for non-build commands
if /i "%~1"=="help"       goto :help
if /i "%~1"=="list"       goto :list_presets
if /i "%~1"=="clean"      goto :clean
if /i "%~1"=="clean-deps" goto :clean_deps
if /i "%~1"=="clean-all"  goto :clean_all

REM ---- tool detection ----
where cmake >nul 2>&1
if errorlevel 1 (
    echo ERROR: cmake not found on PATH. Install CMake 3.20 or newer.
    exit /b 2
)

REM CMake presets use the Ninja generator. We also need cl.exe / link.exe
REM for the build itself. If the user runs this script from a plain cmd,
REM we initialize the Visual Studio Developer Command Prompt environment
REM (which puts ninja, cl, link, etc. on PATH).
if not defined VCINSTALLDIR (
    set "VS_PATH="
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" (
        echo ERROR: vswhere.exe not found. Install Visual Studio 2017 or newer,
        echo        or run this script from a VS Developer Command Prompt.
        exit /b 2
    )
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -property installationPath`) do set "VS_PATH=%%i"
    if not defined VS_PATH (
        echo ERROR: vswhere did not return a Visual Studio installation.
        echo        Run this script from a VS Developer Command Prompt.
        exit /b 2
    )
    if not exist "!VS_PATH!\VC\Auxiliary\Build\vcvarsall.bat" (
        echo ERROR: vcvarsall.bat not found under "!VS_PATH!".
        exit /b 2
    )
    call "!VS_PATH!\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
    if errorlevel 1 (
        echo ERROR: vcvarsall.bat failed.
        exit /b 2
    )
)

where ninja >nul 2>&1
if errorlevel 1 (
    echo ERROR: ninja not found on PATH even after VS dev env setup.
    echo        Install Ninja or use a different generator preset.
    exit /b 2
)

REM ---- target selection ----
set "presets=%*"
if "%presets%"=="" set "presets=x64-Debug x64-Release"

REM ---- parallelism ----
if not defined CMAKE_BUILD_PARALLEL_LEVEL set "CMAKE_BUILD_PARALLEL_LEVEL=%NUMBER_OF_PROCESSORS%"
echo Using %CMAKE_BUILD_PARALLEL_LEVEL% parallel build workers.

set "ANY_FAIL=0"
for %%P in (%presets%) do (
    echo.
    echo === %%P : configure ===
    cmake --preset %%P
    if errorlevel 1 (
        set "ANY_FAIL=1"
        goto :report
    )
    echo.
    echo === %%P : build + install ===
    cmake --build --preset %%P --target install --parallel %CMAKE_BUILD_PARALLEL_LEVEL%
    if errorlevel 1 (
        set "ANY_FAIL=1"
        goto :report
    )
)

:report
if not "%ANY_FAIL%"=="0" (
    echo.
    echo Build FAILED.
    exit /b 1
)
echo.
echo Build successful.
exit /b 0

REM ---------------------------------------------------------------
REM Subcommands
REM ---------------------------------------------------------------

:list_presets
cmake --list-presets=configure
exit /b 0

REM Clean targets. Idempotent: missing folders are silently ignored.

:clean
echo Cleaning build\Debug, build\Release, installation\ (dependency cache and per-preset caches preserved)...
rmdir /s /q "%projectRootPath%\build\Debug" 2>nul
rmdir /s /q "%projectRootPath%\build\Release" 2>nul
rmdir /s /q "%projectRootPath%\installation" 2>nul
echo Done.
exit /b 0

:clean_deps
echo Cleaning build\dependencies\cache\*-build and *-subbuild (source downloads preserved)...
if exist "%projectRootPath%\build\dependencies\cache" (
    for /d %%G in ("%projectRootPath%\build\dependencies\cache\*-build") do rmdir /s /q "%%G" 2>nul
    for /d %%G in ("%projectRootPath%\build\dependencies\cache\*-subbuild") do rmdir /s /q "%%G" 2>nul
)
echo Done.
exit /b 0

:clean_all
echo Cleaning everything under build\ and installation\...
rmdir /s /q "%projectRootPath%\build" 2>nul
rmdir /s /q "%projectRootPath%\installation" 2>nul
echo Done.
exit /b 0

:help
echo Available targets:
echo   build.bat                        - configure + build x64-Debug and x64-Release
echo   build.bat ^<preset^> [^<preset^>..]  - configure + build named CMake preset(s)
echo   build.bat list                   - list available CMake presets
echo   build.bat clean                  - wipe build outputs (keep dep cache and per-preset caches)
echo   build.bat clean-deps             - wipe build\dependencies\cache\*-build and *-subbuild (keep *-src)
echo   build.bat clean-all              - wipe everything under build\ and installation\
echo   build.bat help                   - print this list
echo.
echo Set CMAKE_BUILD_PARALLEL_LEVEL to override the worker count.
exit /b 0
