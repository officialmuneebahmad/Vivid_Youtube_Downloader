@echo off
setlocal enabledelayedexpansion

echo ===================================================
echo   Compiling Vivid YouTube Downloader (GUI)...
echo ===================================================

:: Detect compiler
where g++ >nul 2>nul
if %errorlevel% equ 0 (
    set "CXX=g++"
    set "RC=windres"
) else if exist "C:\w64devkit\bin\g++.exe" (
    set "PATH=C:\w64devkit\bin;%PATH%"
    set "CXX=C:\w64devkit\bin\g++.exe"
    set "RC=C:\w64devkit\bin\windres.exe"
) else if exist "%USERPROFILE%\w64devkit\bin\g++.exe" (
    set "PATH=%USERPROFILE%\w64devkit\bin;%PATH%"
    set "CXX=%USERPROFILE%\w64devkit\bin\g++.exe"
    set "RC=%USERPROFILE%\w64devkit\bin\windres.exe"
) else if exist "%LOCALAPPDATA%\w64devkit\bin\g++.exe" (
    set "PATH=%LOCALAPPDATA%\w64devkit\bin;%PATH%"
    set "CXX=%LOCALAPPDATA%\w64devkit\bin\g++.exe"
    set "RC=%LOCALAPPDATA%\w64devkit\bin\windres.exe"
) else (
    echo [ERROR] MinGW g++ compiler not found in PATH or standard directories.
    echo Please install MinGW or w64devkit to build from source.
    exit /b 1
)

echo [Compiler] Using !CXX!
echo.
echo [1/3] Compiling Application Resources...
!RC! src/resource.rc -O coff -o src/resource.res
if %errorlevel% neq 0 (
    echo [ERROR] Failed to compile resources.
    exit /b %errorlevel%
)

echo [2/3] Compiling C++ Source Code...
!CXX! -std=c++14 -O3 src/main.cpp src/Downloader.cpp src/History.cpp src/resource.res -o yt-downloader.exe -mwindows -lwininet -lcomctl32 -lole32 -lmsimg32
if %errorlevel% neq 0 (
    echo [ERROR] Compilation failed.
    exit /b %errorlevel%
)

echo [3/3] Build Completed Successfully!
echo Output executable: yt-downloader.exe
copy yt-downloader.exe web\yt-downloader.exe /Y >nul 2>nul
copy yt-downloader.exe docs\yt-downloader.exe /Y >nul 2>nul

echo Build finished.
exit /b 0
